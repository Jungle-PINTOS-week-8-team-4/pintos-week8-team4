#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "threads/synch.h"
#include "threads/init.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "devices/input.h"
#include "threads/malloc.h"
#include "lib/string.h"
#include "threads/palloc.h"
void syscall_entry (void);
void syscall_handler (struct intr_frame *);

static int read (int fd, void *buffer, unsigned size);
static int write (int fd, const void *buffer, unsigned size);
static void exit (int status);
static int open (const char *file);
static void close (int fd);
static tid_t fork (const char *thread_name, struct intr_frame *f);
static int wait (tid_t pid);
static tid_t exec(char *file_name);

struct lock syscall_lock;
/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */
#define MAX_FD_CAP 64 // TODO: 100 이하로, TC 통과할 수준으로 만들기. tc 분석해야함

void
syscall_init (void) {
	lock_init(&syscall_lock);
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: Your implementation goes here.
	lock_acquire(&syscall_lock); //TODO: lock 다시보기
	uint64_t call_num = (f->R).rax;

	switch(call_num){
		case SYS_HALT:
			power_off();
			break;
		case SYS_EXIT:
			lock_release(&syscall_lock);
			exit(f->R.rdi);
			return;
		case SYS_FORK:
			lock_release(&syscall_lock);
			f->R.rax = fork((char *)f->R.rdi, f);
			return;
		case SYS_EXEC:
			lock_release(&syscall_lock);
			f->R.rax = exec((char *)f->R.rdi);
			return;
		case SYS_WAIT:

			lock_release(&syscall_lock);
			f->R.rax = wait(f->R.rdi);
			return;
		case SYS_CREATE:
			f->R.rax = filesys_create((char *)f->R.rdi, (unsigned) f->R.rsi);
			break;
		case SYS_REMOVE:
			f->R.rax = filesys_remove((char *)f->R.rdi); 
			break;
		case SYS_OPEN:
			f->R.rax = open((char *)f->R.rdi);
			break;
		case SYS_FILESIZE:
			
			break;
		case SYS_READ:
			f->R.rax = read(f->R.rdi, (char *)f->R.rsi, f->R.rdx);
			break;
		case SYS_WRITE:
			f->R.rax = write(f->R.rdi, (char *)f->R.rsi, f->R.rdx);
			break;
		case SYS_SEEK:
			break;
		case SYS_TELL:
			break;
		case SYS_CLOSE:
			close(f->R.rdi);
			break;
		default: 
			f->R.rax = -1;
		// ... 
	}
	// thread_exit (); - 현재 스레드 위에서 작동하니, exit하면 안됨
	lock_release(&syscall_lock); // TODO: lock release 전에 스레드 끝나는 경우 생각
}

static void close (int fd) { // TODO: 프로세스 종료시 전부 close 필요(file 포인터 free용), + fd_arr free 필요
	struct thread *curr = thread_current();

	if(fd < 2) {
		return;
	} else if( fd - 2 < curr->fd_cnt ){
		struct file **f = &(curr->fd_arr[fd - 2]);
		if(f == NULL)
			return;
		else {
			file_close(*f);
			*f = NULL;
		}
	} else {
		return;
	}
}

static int write (int fd, const void *buffer, unsigned size) {
	struct thread *curr = thread_current();
	
	if(fd == STDIN_FILENO) {
		exit(-1);
	} else if(fd == STDOUT_FILENO) {
		putbuf(buffer, size);
	} else if(fd < 0){
		return -1;
	} else if( fd - 2 < curr->fd_cnt ){
		struct file *f = curr->fd_arr[fd - 2];
		if(f == NULL)
			return 0;
		else {
			return file_write(f, buffer, size);
		}
		
	} else {
		return 0;
	}
}

static int read (int fd, void *buffer, unsigned size) {
	struct thread *curr = thread_current();

	if(fd == STDIN_FILENO) {
		for(unsigned i = 0; i < size; i++ ){
			char input = input_getc(); // TODO: 개행 문자 없으면 block, syscall도 같이 막혀서 성능 저하 - 더 잘게 쪼개야
			((char *)buffer)[i] = input;
			if(input == '\n' || input == '\r'){
				return i+1;
			}
		}
		return size;
	} if(fd == STDOUT_FILENO) {
		exit(-1);
	} else if(fd < 0){
		return -1;
	} else if(fd - 2 < curr->fd_cnt){
		struct file *f = curr->fd_arr[fd - 2];
		if(f == NULL)
			return -1;
		else {
			return file_read(f, buffer, size);
		}
	} else {
		return -1;
	}
}

static int open (const char *file) {
	struct thread *curr = thread_current();
	struct file*** fd_arr = &curr->fd_arr;

	struct file *f = filesys_open(file);
	if(f == NULL) return -1;

	if(curr->fd_cap == 0){
		curr->fd_cap = 2;
		*fd_arr = calloc(2, sizeof(**fd_arr));
		if(*fd_arr == NULL)
			goto fail;
	}
	if(curr->fd_cnt == curr->fd_cap) {
		if(curr->fd_cap * 2 > MAX_FD_CAP)
			goto fail;
		curr->fd_cap *= 2; 
		struct file** new_arr = realloc(*fd_arr, curr->fd_cap * sizeof(**fd_arr));
		if(new_arr != NULL)
			*fd_arr = new_arr;
		else goto fail;
		memset(&(*fd_arr)[curr->fd_cnt], 0, (curr->fd_cap - curr->fd_cnt) * sizeof(**fd_arr));
	}// TODO: realloc후 NULL로 초기화 

	curr->fd_arr[curr->fd_cnt] = f;
	file_deny_write(curr->fd_arr[curr->fd_cnt]);
	return (curr->fd_cnt++) + 2;	

fail:
	file_close(f);
	return -1;
}
}
