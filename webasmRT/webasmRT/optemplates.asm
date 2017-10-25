
_TEXT SEGMENT

ExecutionControlBlock STRUCT
	JitWriter dq ?

	pfnEntry dq ?
	operandStack dq ?
	localsStack dq ?
	memoryBase dq ?

	cFnIndirect dq ?
	rgfnIndirect dq ?
	rgFnTypeIndicies dq ?
	cFnTypeIndicies dq ?
	rgFnPtrs dq ?
	cFnPtrs dq ?

	; Outputs and Temps
	stackrestore dq ?
	retvalue dq ?
ExecutionControlBlock ENDS

; REGISTERS:
;	rax - top of param stack
;	rdi - param stack - 1
;	rsi - memory base
;	rbx	- parameter base
;	rbp - pointer to the execution control block
;	temps: rcx, rdx, r11

ExternCallFnASM PROC pctl : ptr ExecutionControlBlock
	push rdi
	push rsi
	push rbx
	push rbp
	
	mov rdi, (ExecutionControlBlock PTR [rcx]).operandStack
	mov rsi, (ExecutionControlBlock PTR [rcx]).memoryBase
	mov rbx, (ExecutionControlBlock PTR [rcx]).localsStack
	mov (ExecutionControlBlock PTR [rcx]).stackrestore, rsp
	mov rbp, rcx
	mov rax, (ExecutionControlBlock PTR [rcx]).pfnEntry
	call rax
	mov (ExecutionControlBlock PTR [rbp]).retvalue, rax
	mov (ExecutionControlBlock PTR [rbp]).operandStack, rdi
	mov (ExecutionControlBlock PTR [rbp]).localsStack, rbx

	mov eax, 1
LDone:
	pop rbp
	pop rbx
	pop rsi
	pop rdi
	ret
LTrapRet::
	xor eax, eax	; return 0 for failed execution
	jmp LDone
ExternCallFnASM ENDP

BranchTable PROC
	; jump to an argument in the table or default
	;	note: this is a candidate for more optimization but for now we will always perform a jump table
	;
	;	rax - table index
	;	rcx - table pointer
	;	edx - temp
	;
	;	Table Format:
	;		dword count
	;		dword fReturnVal
	;		--default_target--
	;		qword distance (how many blocks we're jumping)
	;		qword target_addr
	;		--table_targets---
	;		.... * count
	
	mov edx, eax		; backup the table index
	mov rax, [rdi]		; store the block return value (or garbage)

	cmp edx, [rcx]
	jb LNotDefault
	xor edx, edx
	jmp LDefault

LNotDefault:
	;else use table
	add edx, 1						; skip the default
	shl edx, 4						; convert table offset to a byte offset
LDefault:
	; at this point rdx is a byte offset into the vector table
	; Lets adjust the stack to deal with the blocks we're leaving
	mov r11d, dword ptr [rcx+8+rdx]			; r11d = table[idx].distance
	lea rsp, [rsp+r11*8]					; adjust the stack for the blocks
	pop rdi									; Restore the param stack to the right location
	; Put the top of the param stack in rax if we don't have a return value
	mov r11d, dword ptr [rcx+4]
	test r11d, r11d
	jnz LHasRetValue
	; Doesn't have a return value if we get here, so put the top param back in rax
	mov rax, [rdi]
	sub rdi, 8
LHasRetValue:
	; The universe should be setup correctly for the target block so we just need to jump
	jmp [rcx+16+rdx]
BranchTable ENDP

Trap PROC
	mov rsp, (ExecutionControlBlock PTR [rbp]).stackrestore
	jmp LTrapRet
Trap ENDP

CReentryFn PROTO
WasmToC PROC
	; Translates a wasm function call into a C function call for internal use
	; we expect the internal function # in rcx already
	; we will put the parameter base address in rdx
	; the normal JIT registers are preserved by the calling convention
	
	mov rdx, rbx	; put the parameter base address in rdx (second arg)
	mov r8, rsi
	mov r9, rbp
	; reserve space on stack

	sub rsp, 32
	call CReentryFn
	add rsp, 32
	ret
WasmToC ENDP

CompileFn PROTO
CallIndirectShim PROC
	; ecx contains the function index
	; eax contains the type
	
	; Convert the indirect index to the function index
	; First Bounds Check
	cmp rcx, (ExecutionControlBlock PTR [rbp]).cFnIndirect
	jae Trap
	; Now do the conversion
	mov rdx, (ExecutionControlBlock PTR [rbp]).rgFnIndirect
	mov ecx, [rdx + rcx * 4]

	; Now bounds check the type index
	mov rdx, (ExecutionControlBlock PTR [rbp]).rgFnTypeIndicies
	cmp rcx, (ExecutionControlBlock PTR [rbp]).cFnTypeIndicies
	jae Trap

	mov edx, [rdx + rcx*4]	; get the callee's type
	cmp edx, eax			; check if its equal to the expected type
	jne Trap				; Trap if not

	; Type is validated now lets do the function call
	cmp rcx, (ExecutionControlBlock PTR [rbp]).cFnPtrs
	jae Trap
	mov rdx, (ExecutionControlBlock PTR [rbp]).rgFnPtrs
	;jmp qword ptr [rdx + rcx*8]
	mov rax, [rdx + rcx*8]
	test rax, rax
	jz LCompileFn
	jmp rax
	ud2

LCompileFn:
	lea rax, [rdx + rcx*8]
	push rax
	sub rsp, 32
	mov edx, ecx	; second param is the function index
	mov rcx, rbp	; first param the control block
	call CompileFn  ; call the function
	add rsp, 32
	pop rax
	jmp qword ptr [rax]
	ud2
CallIndirectShim ENDP

_TEXT ENDS

END