	;# callee-saved registers - System V AMD64 ABI
	push rbx
	push rbp
	push r12
	push r13
	push r14
	push r15

	;# function arguments
	mov rbx, rcx                ;# loop counter
	push rdi                    ;# RegisterFile& registerFile
	mov rcx, rdi
	mov rbp, qword ptr [rsi]    ;# "mx", "ma"
	mov rdi, qword ptr [rsi+8]  ;# uint8_t* dataset
	mov rsi, rdx                ;# uint8_t* scratchpad
