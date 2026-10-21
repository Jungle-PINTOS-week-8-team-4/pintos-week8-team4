#include "userprog/syscall.h"
#include "lib/stdio.h"
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

static void check_address (const void *addr) {
	struct thread *curr = thread_current ();
	// 1. NULL이거나 유저 가상 메모리 범위를 벗어났는지 확인
	// 2. 커널 영역이거나 페이지 테이블(pml4)에 실제로 매핑된 주소인지 확인
	if (addr == NULL || !is_user_vaddr (addr) || pml4_get_page (curr->pml4, addr) == NULL) {
		exit (-1);
	}
}

static int read (int fd, void *buffer, unsigned size);
static int write (int fd, const void *buffer, unsigned size);
static void exit (int status);
static int open (const char *file);
static void close (int fd);
static tid_t fork (const char *thread_name, struct intr_frame *f);
static int wait (tid_t pid);
static tid_t exec(char *file_name);
static int filesize(int fd);
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
syscall_handler (struct intr_frame *f) { // idt로 invoke되지 않고, 별도 경로에서 
	// TODO: Your implementation goes here.
	/*
	Thus, when the system call handler syscall_handler() gets control, the system call number is in the rax, and arguments are passed with the order %rdi, %rsi, %rdx, %r10, %r8, and %r9.

The caller's registers are accessible to struct intr_frame passed to it. (struct intr_frame is on the kernel stack.)
	intr_frame에서 가져올 수 있다.	
*/

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
			check_address((const void *) f->R.rdi);
			f->R.rax = filesys_create((char *)f->R.rdi, (unsigned) f->R.rsi);
			break;
		case SYS_REMOVE:
			if (f->R.rdi == NULL || !is_user_vaddr (f->R.rdi))
				exit (-1);
			f->R.rax = filesys_remove((char *)f->R.rdi); 
			break;
		case SYS_OPEN:
			f->R.rax = open((char *)f->R.rdi);
			break;
		case SYS_FILESIZE:
			f->R.rax = filesize(f->R.rdi);
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
	} else if( fd - 2 < curr->fd_cap ){
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


static int read (int fd, void *buffer, unsigned size) {
	struct thread *curr = thread_current();
	check_address(buffer); 
	check_address(buffer + size -1);

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
	} else if(fd - 2 < curr->fd_cap){
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

	check_address(file); 
	struct thread *curr = thread_current();
	struct file*** fd_arr = &curr->fd_arr;

    if (file == NULL || !is_user_vaddr (file))
        exit (-1);

	struct file *f = filesys_open(file);
	if(f == NULL) return -1;

	if(curr->fd_cap == 0){
		curr->fd_cap = 2;
		*fd_arr = calloc(2, sizeof(**fd_arr));
		if(*fd_arr == NULL)
			goto fail;
	}

	for(int i = 0; i < curr->fd_cap; i++){
		if(curr->fd_arr[i] == NULL) {
			curr->fd_arr[i] = f;
			return i + 2;
		}
	}
	if(curr->fd_cap * 2 <= MAX_FD_CAP){
		curr->fd_cap *= 2;
		struct file** new_arr = realloc(*fd_arr, curr->fd_cap * sizeof(**fd_arr));
		if(new_arr != NULL)
			*fd_arr = new_arr;
		else goto fail;
		memset(&(*fd_arr)[curr->fd_cap/2], 0, (curr->fd_cap / 2) * sizeof(**fd_arr));
	} else goto fail;

	curr->fd_arr[curr->fd_cap / 2] = f;
	return (curr->fd_cap / 2) + 2;

fail:
	file_close(f);
	return -1;
}

int write (int fd, const void *buffer, unsigned size) {
    //버퍼 포인터 유효성 검사. NULL이거나 유저 메모리 영역이 아니면 프로세스 종료

	check_address(buffer); 
	check_address(buffer + size -1);

    //표준 출력 (stdout, fd == 1)인 경우 콘솔에 쓰기
    if (fd == STDOUT_FILENO) {      
        putbuf (buffer, size);
        return size;
    }
    //표준 입력 (stdin, fd == 0)인 경우 읽기 전용이므로 쓸 수 없음
    else if (fd == STDIN_FILENO) {
        return -1;
    }
    //그 외의 경우 (일반 파일 디스크립터 또는 잘못된 fd 번호)
    else {
        struct thread *curr = thread_current ();

        // FD 유효성 검사. 범위를 벗어나거나 열린 파일이 없으면 -1 반환
        if (fd < 2 || fd - 2 >= curr->fd_cap || curr->fd_arr[fd-2] == NULL) {
            return -1;
        }

        // 장부에서 실제 파일 객체 가져오기
        struct file *file_obj = curr->fd_arr[fd - 2];

        //파일에 쓰기 수행
        off_t file_written = file_write (file_obj, buffer, size);

        return file_written;
    }
}

static tid_t fork (const char *thread_name, struct intr_frame *f) {
	check_address(thread_name); 

	struct intr_frame *intr_f = malloc(sizeof(*f));
	*intr_f = *f;
	tid_t pid = process_fork(thread_name, intr_f);
	// TODO: 프로세스 복사 전까지는 멈춰야

	return pid;
}

static void exit (int status){
	struct thread *curr = thread_current();
	process_status_arr[curr->tid]->exit_code = status;
	
	thread_exit();
}

static int wait (tid_t pid){
	// list search하고, '찾아야' sema down 접근 가능,  아닌 경우 -1
	return process_wait(pid);

	 // TODO: lock을 더 세분화

	// 찾은 status struct free하기
}

static tid_t exec(char *cmd_line) { // TODO: exec도 file read하므로 다른 file read syscall과 동시에 작업하면 안됨
/* Make a copy of FILE_NAME.
* Otherwise there's a race between the caller and load(). */
	check_address(cmd_line); 

	char *fn_copy = palloc_get_page (1);

	if (fn_copy == NULL)
		return TID_ERROR;

	strlcpy(fn_copy, cmd_line, PGSIZE);
	return process_exec(fn_copy); // 성공시 다시 돌아오지 않음
}

static int filesize(int fd) {
	struct thread *curr = thread_current();

	if(fd < 2) {
		return -1;
	} else if( fd - 2 < curr->fd_cap ){
		struct file *f = (curr->fd_arr[fd - 2]);
		if(f == NULL)
			return;
		else {
			return file_length(f);
		}
	} else {
		return;
	}
}