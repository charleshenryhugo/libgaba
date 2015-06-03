
#
# @file io.s
#
# @brief sequence reader / alignment writer implementation for x86_64.
#

	.text
	.align 4

# pop functions

	# ascii
	.globl _pop_ascii
	.globl __pop_ascii
_pop_ascii:
__pop_ascii:
	movb (%rdi, %rsi), %al
	movq %rax, %rsi
	shrq $2, %rax
	shrq $1, %rsi
	xorq %rsi, %rax
	andq $3, %rax
	ret

	# 4bit encoded (1base per byte)
	.globl _pop_4bit
	.globl __pop_4bit
_pop_4bit:
__pop_4bit:
	movb (%rdi, %rsi), %al
	movq %rax, %rsi
	shrq $3, %rax
	shrq $1, %rsi
	andq $1, %rax
	subq %rsi, %rax
	andq $3, %rax
	ret

	# 2bit encoded (1base per byte)
	.globl _pop_2bit
	.globl __pop_2bit
_pop_2bit:
__pop_2bit:
	movb (%rdi, %rsi), %al
	ret

	# 4bit packed
	.globl _pop_4bit8packed
	.globl __pop_4bit8packed
_pop_4bit8packed:
__pop_4bit8packed:
	movq %rsi, %rcx
	shrq $1, %rsi
	movb (%rdi, %rsi), %al
	shlq $2, %rcx
	andq $4, %rcx
	shrq %cl, %rax
	movq %rax, %rsi
	shrq $3, %rsi
	shrq $1, %rax
	andq $1, %rsi
	subq %rsi, %rax
	andq $3, %rax	
	ret

	# 2bit packed
	.globl _pop_2bit8packed
	.globl __pop_2bit8packed
_pop_2bit8packed:
__pop_2bit8packed:
	movq %rsi, %rcx
	shrq $2, %rsi
	movb (%rdi, %rsi), %al
	shlq $1, %rcx
	andq $6, %rcx
	shrq %cl, %rax
	andq $3, %rax
	ret

# push functions

	# ascii forward
	.globl _init_ascii_f
	.globl __init_ascii_f
_init_ascii_f:
__init_ascii_f:
	addq $-1, %rsi
	movb $0, (%rdi, %rsi)
	movq %rsi, %rax
	ret

	.globl _pushm_ascii_f
	.globl __pushm_ascii_f
_pushm_ascii_f:
__pushm_ascii_f:
	addq $-1, %rsi
	movb $77, (%rdi, %rsi)
	movq %rsi, %rax
	ret

	.globl _pushx_ascii_f
	.globl __pushx_ascii_f
_pushx_ascii_f:
__pushx_ascii_f:
	addq $-1, %rsi
	movb $88, (%rdi, %rsi)
	movq %rsi, %rax
	ret

	.globl _pushi_ascii_f
	.globl __pushi_ascii_f
_pushi_ascii_f:
__pushi_ascii_f:
	addq $-1, %rsi
	movb $73, (%rdi, %rsi)
	movq %rsi, %rax
	ret

	.globl _pushd_ascii_f
	.globl __pushd_ascii_f
_pushd_ascii_f:
__pushd_ascii_f:
	addq $-1, %rsi
	movb $68, (%rdi, %rsi)
	movq %rsi, %rax
	ret

	.globl _finish_ascii_f
	.globl __finish_ascii_f
_finish_ascii_f:
__finish_ascii_f:
	movq %rsi, %rax
	ret

	# ascii reverse
	.globl _init_ascii_r
	.globl __init_ascii_r
_init_ascii_r:
__init_ascii_r:
	movq %rsi, %rax
	ret

	.globl _pushm_ascii_r
	.globl __pushm_ascii_r
_pushm_ascii_r:
__pushm_ascii_r:
	movb $77, (%rdi, %rsi)
	movq %rsi, %rax
	addq $1, %rax
	ret

	.globl _pushx_ascii_r
	.globl __pushx_ascii_r
_pushx_ascii_r:
__pushx_ascii_r:
	movb $88, (%rdi, %rsi)
	movq %rsi, %rax
	addq $1, %rax
	ret

	.globl _pushi_ascii_r
	.globl __pushi_ascii_r
_pushi_ascii_r:
__pushi_ascii_r:
	movb $73, (%rdi, %rsi)
	movq %rsi, %rax
	addq $1, %rax
	ret

	.globl _pushd_ascii_r
	.globl __pushd_ascii_r
_pushd_ascii_r:
__pushd_ascii_r:
	movb $68, (%rdi, %rsi)
	movq %rsi, %rax
	addq $1, %rax
	ret

	.globl _finish_ascii_r
	.globl __finish_ascii_r
_finish_ascii_r:
__finish_ascii_r:
	movb $0, (%rdi, %rsi)
	movq %rsi, %rax
	addq $1, %rax
	ret

	# cigar forward
	.globl _init_cigar_f
	.globl __init_cigar_f
_init_cigar_f:
__init_cigar_f:
	addq $-1, %rsi
	movb $0, (%rdi, %rsi)
	addq $-1, %rsi
	movb $61, (%rdi, %rsi)
	movq %rsi, %rax
	addq $-4, %rsi
	movl $0, (%rdi, %rsi)
	ret

	.globl _pushm_cigar_f
	.globl __pushm_cigar_f
_pushm_cigar_f:
__pushm_cigar_f:
	cmpb $61, (%rdi, %rsi)
	je __pushm_cigar_f_incr
	movq %rsi, %rdx
	addq $-4, %rdx
	movl (%rdi, %rdx), %eax
	movq $10, %rcx
__pushm_cigar_f_loop:
	movq $0, %rdx
	divq %rcx
	addq $48, %rdx
	addq $-1, %rsi
	movb %dl, (%rdi, %rsi)
	cmpl $0, %eax
	jne __pushm_cigar_f_loop
	addq $-1, %rsi
	movb $61, (%rdi, %rsi)
	movq %rsi, %rax
	addq $-4, %rsi
	movl $1, (%rdi, %rsi)
	ret
__pushm_cigar_f_incr:
	movq %rsi, %rax
	addq $-4, %rsi
	addl $1, (%rdi, %rsi)
	ret

	.globl _pushx_cigar_f
	.globl __pushx_cigar_f
_pushx_cigar_f:
__pushx_cigar_f:
	cmpb $88, (%rdi, %rsi)
	je __pushx_cigar_f_incr
	movq %rsi, %rdx
	addq $-4, %rdx
	movl (%rdi, %rdx), %eax
	movq $10, %rcx
__pushx_cigar_f_loop:
	movq $0, %rdx
	divq %rcx
	addq $48, %rdx
	addq $-1, %rsi
	movb %dl, (%rdi, %rsi)
	cmpl $0, %eax
	jne __pushx_cigar_f_loop
	addq $-1, %rsi
	movb $88, (%rdi, %rsi)
	movq %rsi, %rax
	addq $-4, %rsi
	movl $1, (%rdi, %rsi)
	ret
__pushx_cigar_f_incr:
	movq %rsi, %rax
	addq $-4, %rsi
	addq $1, (%rdi, %rsi)
	ret

	.globl _pushi_cigar_f
	.globl __pushi_cigar_f
_pushi_cigar_f:
__pushi_cigar_f:
	cmpb $73, (%rdi, %rsi)
	je __pushi_cigar_f_incr
	movq %rsi, %rdx
	addq $-4, %rdx
	movl (%rdi, %rdx), %eax
	movq $10, %rcx
__pushi_cigar_f_loop:
	movq $0, %rdx
	divq %rcx
	addq $48, %rdx
	addq $-1, %rsi
	movb %dl, (%rdi, %rsi)
	cmpl $0, %eax
	jne __pushi_cigar_f_loop
	addq $-1, %rsi
	movb $73, (%rdi, %rsi)
	movq %rsi, %rax
	addq $-4, %rsi
	movl $1, (%rdi, %rsi)
	ret
__pushi_cigar_f_incr:
	movq %rsi, %rax
	addq $-4, %rsi
	addq $1, (%rdi, %rsi)
	ret

	.globl _pushd_cigar_f
	.globl __pushd_cigar_f
_pushd_cigar_f:
__pushd_cigar_f:
	cmpb $68, (%rdi, %rsi)
	je __pushd_cigar_f_incr
	movq %rsi, %rdx
	addq $-4, %rdx
	movl (%rdi, %rdx), %eax
	movq $10, %rcx
__pushd_cigar_f_loop:
	movq $0, %rdx
	divq %rcx
	addq $48, %rdx
	addq $-1, %rsi
	movb %dl, (%rdi, %rsi)
	cmpl $0, %eax
	jne __pushd_cigar_f_loop
	addq $-1, %rsi
	movb $68, (%rdi, %rsi)
	movq %rsi, %rax
	addq $-4, %rsi
	movl $1, (%rdi, %rsi)
	ret
__pushd_cigar_f_incr:
	movq %rsi, %rax
	addq $-4, %rsi
	addq $1, (%rdi, %rsi)
	ret

	.globl _finish_cigar_f
	.globl __finish_cigar_f
_finish_cigar_f:
__finish_cigar_f:
	movq %rsi, %rdx
	addq $-4, %rdx
	movl (%rdi, %rdx), %eax
	movq $10, %rcx
__finish_cigar_f_loop:
	movq $0, %rdx
	divq %rcx
	addq $48, %rdx
	addq $-1, %rsi
	movb %dl, (%rdi, %rsi)
	cmpl $0, %eax
	jne __finish_cigar_f_loop
	movq %rsi, %rax
	ret

	# cigar reverse
	.globl _init_cigar_r
	.globl __init_cigar_r
_init_cigar_r:
__init_cigar_r:
	movb $61, (%rdi, %rsi)
	movq %rsi, %rax
	addq $1, %rsi
	movl $0, (%rdi, %rsi)
	ret

	.globl _pushm_cigar_r
	.globl __pushm_cigar_r
_pushm_cigar_r:
__pushm_cigar_r:
	movb (%rdi, %rsi), %r8b
	cmpb $61, %r8b
	je __pushm_cigar_r_incr
	pxor %xmm0, %xmm0
	pxor %xmm1, %xmm1
	movd %r8d, %xmm0
#	shlq $56, %r8		# %r8 is used as string buffer
	movq $1, %r9		# %r9 holds the length of array in %r8
	movq %rsi, %rdx
	addq $1, %rdx
	movl (%rdi, %rdx), %eax
	movq $10, %rcx
__pushm_cigar_r_loop:
	movq $0, %rdx
	divq %rcx
	addq $48, %rdx
	movd %edx, %xmm1
#	shlq $56, %rdx
#	shlq $8, %r8
	pslldq $1, %xmm0
#	orq %rdx, %r8
	por %xmm1, %xmm0
	addq $1, %r9
	cmpl $0, %eax
	jne __pushm_cigar_r_loop
#	movq %r8, (%rdi, %rsi)
	movdqu %xmm0, (%rdi, %rsi)
	addq %r9, %rsi
	movb $61, (%rdi, %rsi)
	movq %rsi, %rax
	addq $1, %rsi
	movl $1, (%rdi, %rsi)
	ret
__pushm_cigar_r_incr:
	movq %rsi, %rax
	addq $1, %rsi
	addl $1, (%rdi, %rsi)
	ret

	.globl _pushx_cigar_r
	.globl __pushx_cigar_r
_pushx_cigar_r:
__pushx_cigar_r:
	movb (%rdi, %rsi), %r8b
	cmpb $88, %r8b
	je __pushx_cigar_r_incr
	pxor %xmm0, %xmm0
	pxor %xmm1, %xmm1
	movd %r8d, %xmm0
#	shlq $56, %r8		# %r8 is used as string buffer
	movq $1, %r9		# %r9 holds the length of array in %r8
	movq %rsi, %rdx
	addq $1, %rdx
	movl (%rdi, %rdx), %eax
	movq $10, %rcx
__pushx_cigar_r_loop:
	movq $0, %rdx
	divq %rcx
	addq $48, %rdx
	movd %edx, %xmm1
#	shlq $56, %rdx
#	shlq $8, %r8
	pslldq $1, %xmm0
#	orq %rdx, %r8
	por %xmm1, %xmm0
	addq $1, %r9
	cmpl $0, %eax
	jne __pushx_cigar_r_loop
#	movq %r8, (%rdi, %rsi)
	movdqu %xmm0, (%rdi, %rsi)
	addq %r9, %rsi
	movb $88, (%rdi, %rsi)
	movq %rsi, %rax
	addq $1, %rsi
	movl $1, (%rdi, %rsi)
	ret
__pushx_cigar_r_incr:
	movq %rsi, %rax
	addq $1, %rsi
	addl $1, (%rdi, %rsi)
	ret

	.globl _pushi_cigar_r
	.globl __pushi_cigar_r
_pushi_cigar_r:
__pushi_cigar_r:
	movb (%rdi, %rsi), %r8b
	cmpb $73, %r8b
	je __pushi_cigar_r_incr
	pxor %xmm0, %xmm0
	pxor %xmm1, %xmm1
	movd %r8d, %xmm0
#	shlq $56, %r8		# %r8 is used as string buffer
	movq $1, %r9		# %r9 holds the length of array in %r8
	movq %rsi, %rdx
	addq $1, %rdx
	movl (%rdi, %rdx), %eax
	movq $10, %rcx
__pushi_cigar_r_loop:
	movq $0, %rdx
	divq %rcx
	addq $48, %rdx
	movd %edx, %xmm1
#	shlq $56, %rdx
#	shlq $8, %r8
	pslldq $1, %xmm0
#	orq %rdx, %r8
	por %xmm1, %xmm0
	addq $1, %r9
	cmpl $0, %eax
	jne __pushi_cigar_r_loop
#	movq %r8, (%rdi, %rsi)
	movdqu %xmm0, (%rdi, %rsi)
	addq %r9, %rsi
	movb $73, (%rdi, %rsi)
	movq %rsi, %rax
	addq $1, %rsi
	movl $1, (%rdi, %rsi)
	ret
__pushi_cigar_r_incr:
	movq %rsi, %rax
	addq $1, %rsi
	addl $1, (%rdi, %rsi)
	ret

	.globl _pushd_cigar_r
	.globl __pushd_cigar_r
_pushd_cigar_r:
__pushd_cigar_r:
	movb (%rdi, %rsi), %r8b
	cmpb $68, %r8b
	je __pushd_cigar_r_incr
	pxor %xmm0, %xmm0
	pxor %xmm1, %xmm1
	movd %r8d, %xmm0
#	shlq $56, %r8		# %r8 is used as string buffer
	movq $1, %r9		# %r9 holds the length of array in %r8
	movq %rsi, %rdx
	addq $1, %rdx
	movl (%rdi, %rdx), %eax
	movq $10, %rcx
__pushd_cigar_r_loop:
	movq $0, %rdx
	divq %rcx
	addq $48, %rdx
	movd %edx, %xmm1
#	shlq $56, %rdx
#	shlq $8, %r8
	pslldq $1, %xmm0
#	orq %rdx, %r8
	por %xmm1, %xmm0
	addq $1, %r9
	cmpl $0, %eax
	jne __pushd_cigar_r_loop
#	movq $8, %rcx
#	subq %r9, %rcx
#	shlq $3, %rcx
#	shrq %cl, %r8
#	movq %r8, (%rdi, %rsi)
	movdqu %xmm0, (%rdi, %rsi)
	addq %r9, %rsi
	movb $68, (%rdi, %rsi)
	movq %rsi, %rax
	addq $1, %rsi
	movl $1, (%rdi, %rsi)
	ret
__pushd_cigar_r_incr:
	movq %rsi, %rax
	addq $1, %rsi
	addl $1, (%rdi, %rsi)
	ret

	.globl _finish_cigar_r
	.globl __finish_cigar_r
_finish_cigar_r:
__finish_cigar_r:
	movb (%rdi, %rsi), %r8b
	pxor %xmm0, %xmm0
	pxor %xmm1, %xmm1
	movd %r8d, %xmm0
#	shlq $56, %r8		# %r8 is used as string buffer
	movq $1, %r9		# %r9 holds the length of array in %r8
	movq %rsi, %rdx
	addq $1, %rdx
	movl (%rdi, %rdx), %eax
	movq $10, %rcx
__finish_cigar_r_loop:
	movq $0, %rdx
	divq %rcx
	addq $48, %rdx
	movd %edx, %xmm1
#	shlq $56, %rdx
#	shlq $8, %r8
	pslldq $1, %xmm0
#	orq %rdx, %r8
	por %xmm1, %xmm0
	addq $1, %r9
	cmpl $0, %eax
	jne __finish_cigar_r_loop
#	movq %r8, (%rdi, %rsi)
	movdqu %xmm0, (%rdi, %rsi)
	addq %r9, %rsi
	movb $0, (%rdi, %rsi)
	movq %rsi, %rax
	addq $1, %rax
	ret

	# direction string forward
	.globl _init_dir_f
	.globl __init_dir_f
_init_dir_f:
__init_dir_f:
	addq $-1, %rsi
	movb $0, (%rdi, %rsi)
	movq %rsi, %rax
	ret

	.globl _pushm_dir_f
	.globl __pushm_dir_f
_pushm_dir_f:
__pushm_dir_f:
	addq $-2, %rsi
	movw $1, (%rdi, %rsi)
	movq %rsi, %rax
	ret

	.globl _pushx_dir_f
	.globl __pushx_dir_f
_pushx_dir_f:
__pushx_dir_f:
	addq $-2, %rsi
	movw $1, (%rdi, %rsi)
	movq %rsi, %rax
	ret

	.globl _pushi_dir_f
	.globl __pushi_dir_f
_pushi_dir_f:
__pushi_dir_f:
	addq $-1, %rsi
	movb $1, (%rdi, %rsi)
	movq %rsi, %rax
	ret

	.globl _pushd_dir_f
	.globl __pushd_dir_f
_pushd_dir_f:
__pushd_dir_f:
	addq $-1, %rsi
	movb $0, (%rdi, %rsi)
	movq %rsi, %rax
	ret

	.globl _finish_dir_f
	.globl __finish_dir_f
_finish_dir_f:
__finish_dir_f:
	movq %rsi, %rax
	ret


	# direction string reverse
	.globl _init_dir_r
	.globl __init_dir_r
_init_dir_r:
__init_dir_r:
	movq %rsi, %rax
	ret

	.globl _pushm_dir_r
	.globl __pushm_dir_r
_pushm_dir_r:
__pushm_dir_r:
	movw $1, (%rdi, %rsi)
	movq %rsi, %rax
	addq $2, %rax
	ret

	.globl _pushx_dir_r
	.globl __pushx_dir_r
_pushx_dir_r:
__pushx_dir_r:
	movw $1, (%rdi, %rsi)
	movq %rsi, %rax
	addq $2, %rax
	ret

	.globl _pushi_dir_r
	.globl __pushi_dir_r
_pushi_dir_r:
__pushi_dir_r:
	movb $1, (%rdi, %rsi)
	movq %rsi, %rax
	addq $1, %rax
	ret

	.globl _pushd_dir_r
	.globl __pushd_dir_r
_pushd_dir_r:
__pushd_dir_r:
	movb $0, (%rdi, %rsi)
	movq %rsi, %rax
	addq $1, %rax
	ret

	.globl _finish_dir_r
	.globl __finish_dir_r
_finish_dir_r:
__finish_dir_r:
	movb $0, (%rsi, %rax)
	movq %rsi, %rax
	addq $1, %rax
	ret

#
# end of io.s
#
