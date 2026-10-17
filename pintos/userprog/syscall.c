#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "threads/mmu.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "threads/vaddr.h"
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
	printf ("system call!\n");
	thread_exit ();
}

/* 사용자 주소 하나를 현재 프로세스가 실제로 사용할 수 있는지 검사한다.
   pml4_get_page()는 사용자 주소가 아닌 값을 받으면 ASSERT에 걸리므로,
   NULL과 사용자 주소 범위를 먼저 확인한다. */
bool
syscall_is_valid_user_address (const void *address) {
	struct thread *current = thread_current ();
	uint64_t *pte;

	if (address == NULL || !is_user_vaddr (address) || current->pml4 == NULL)
		return false;

	if (pml4_get_page (current->pml4, address) == NULL)
		return false;

	/* 주소 범위뿐 아니라 페이지 테이블도 사용자 접근을 허용해야 한다. */
	pte = pml4e_walk (current->pml4, (uint64_t) address, false);
	return pte != NULL && is_user_pte (pte);
}

/* 크기가 SIZE인 사용자 버퍼의 모든 페이지가 매핑되어 있는지 검사한다.
   READ syscall처럼 커널이 사용자 메모리에 데이터를 쓸 때는 WRITABLE을
   true로 전달해 해당 페이지의 쓰기 권한도 확인한다. */
bool
syscall_is_valid_user_buffer (const void *buffer, size_t size, bool writable) {
	struct thread *current = thread_current ();
	uintptr_t start = (uintptr_t) buffer;
	uintptr_t end;
	uintptr_t page;
	uintptr_t last_page;

	/* 길이가 0이면 버퍼를 읽거나 쓰지 않으므로 검사할 범위가 없다. */
	if (size == 0)
		return true;

	if (buffer == NULL || !is_user_vaddr (buffer) || current->pml4 == NULL)
		return false;

	/* start + size - 1의 정수 오버플로를 막는다. */
	if (size - 1 > UINTPTR_MAX - start)
		return false;
	end = start + size - 1;
	if (!is_user_vaddr ((void *) end))
		return false;

	page = (uintptr_t) pg_round_down ((void *) start);
	last_page = (uintptr_t) pg_round_down ((void *) end);

	/* 첫 페이지부터 마지막 페이지까지 빠짐없이 확인한다.
	   버퍼가 페이지 경계를 넘는 경우도 여기서 검사된다. */
	for (;;) {
		uint64_t *pte;

		if (!syscall_is_valid_user_address ((void *) page))
			return false;

		if (writable) {
			pte = pml4e_walk (current->pml4, page, false);
			if (pte == NULL || !is_writable (pte))
				return false;
		}

		if (page == last_page)
			return true;
		page += PGSIZE;
	}
}

/* 사용자 문자열을 페이지 단위로 검사하면서 널 종료 문자를 찾는다.
   각 페이지를 검사한 뒤 pml4_get_page()가 돌려준 커널 주소로 읽어서,
   잘못된 사용자 주소를 직접 역참조하지 않도록 한다. */
bool
syscall_is_valid_user_string (const char *string) {
	struct thread *current = thread_current ();
	uintptr_t address = (uintptr_t) string;

	if (string == NULL || current->pml4 == NULL)
		return false;

	for (;;) {
		void *kernel_address;
		size_t bytes_in_page;

		if (!syscall_is_valid_user_address ((void *) address))
			return false;

		kernel_address = pml4_get_page (current->pml4, (void *) address);
		if (kernel_address == NULL)
			return false;

		/* 현재 페이지의 남은 부분만 읽고, 널 문자가 없으면 다음 페이지로 간다. */
		bytes_in_page = PGSIZE - pg_ofs ((void *) address);
		if (memchr (kernel_address, '\0', bytes_in_page) != NULL)
			return true;

		address += bytes_in_page;
	}
}
