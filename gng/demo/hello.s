	.globl _start
	.text
_start:
	movl $4, %eax
	movl $1, %ebx
	movl $msg, %ecx
	movl $msglen, %edx
	int $0x80
	movl $1, %eax
	movl $9, %ebx
	int $0x80
msg:	.ascii "assembled by GNU as and linked by GNU ld INSIDE Ginux\n"
	.set msglen, . - msg
