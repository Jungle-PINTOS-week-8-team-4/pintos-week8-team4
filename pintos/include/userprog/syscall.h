#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <stdbool.h>
#include <stddef.h>

void syscall_init (void);

/* 사용자 주소가 현재 프로세스에서 안전하게 접근 가능한지 검사한다. */
bool syscall_is_valid_user_address (const void *address);

/* 사용자 버퍼 전체가 매핑되어 있는지 검사한다.
   WRITABLE이 true이면 커널이 버퍼에 쓸 수 있도록 쓰기 권한도 확인한다. */
bool syscall_is_valid_user_buffer (const void *buffer, size_t size,
		bool writable);

/* 널 문자까지 읽을 수 있도록 사용자 문자열의 모든 페이지를 검사한다. */
bool syscall_is_valid_user_string (const char *string);

#endif /* userprog/syscall.h */
