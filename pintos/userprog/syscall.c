#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

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

void
syscall_init (void) {
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

	uint64_t syscall = f->R.rax;

	switch (syscall) {
		case SYS_EXIT:
			exit (f->R.rdi);
            break;
		case SYS_WRITE:
			f->R.rax = write (f->R.rdi, (const void *) f->R.rsi, f->R.rdx);
            break;
		case SYS_HALT:
			halt();
			break;
		case SYS_FORK:
			f->R.rax = process_fork((const char *)f->R.rdi, f);
			break;
		case SYS_EXEC:
			f->R.rax = process_exec ((void *) f->R.rdi);
			break;
		case SYS_CREATE:
			f->R.rax = create ((const char *)f->R.rdi, (unsigned)f->R.rsi);
			break;
		case SYS_REMOVE:
			f->R.rax = remove ((const char *)f->R.rdi);
			break;
		case SYS_OPEN:
			f->R.rax = open((const char *)f->R.rdi);
			break;
		case SYS_FILESIZE:
			break;
		case SYS_READ:
			f->R.rax = read (f->R.rdi, (void *)f->R.rsi, f->R.rdx);
			break;
		case SYS_SEEK:
			break;
		case SYS_TELL:
			break;
		case SYS_CLOSE:
			close ((int)f->R.rdi);
			break;
		case SYS_WAIT:
			f->R.rax = process_wait ((tid_t) f->R.rdi);
			break;
		default:
            exit (-1);
            break;
	}
	

	// printf ("system call!\n");
	// thread_exit ();
}
int write (int fd, const void *buffer, unsigned size) {
    //버퍼 포인터 유효성 검사. NULL이거나 유저 메모리 영역이 아니면 프로세스 종료
    if (buffer == NULL || !is_user_vaddr (buffer))
        exit (-1);
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
        if (fd < 2 || fd >= curr->fd_cap || curr->fd_arr[fd] == NULL) {
            return -1;
        }

        // 장부에서 실제 파일 객체 가져오기
        struct file *fileobj = curr->fd_arr[fd];

        //파일에 쓰기 수행
        off_t file_written = file_write (fileobj, buffer, size);

        return file_written;
    }
}
