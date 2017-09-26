
_TEXT SEGMENT

; REGISTERS:
;	rax - top of param stack
;	rdi - param stack - 1
;	rsi - memory base
;	rbx	- parameter base
;	temps: rcx, rdx

ExternCallFnASM PROC ;pfn:QWORD, operandStack:QWORD, localsStack:QWORD, memoryBase:QWORD 
	push rdi
	push rsi
	push rbx
	
	mov rdi, rdx
	mov rsi, r9
	mov rbx, r8
	call rcx
	
	pop rbx
	pop rsi
	pop rdi
	ret
ExternCallFnASM ENDP

_TEXT ENDS

END