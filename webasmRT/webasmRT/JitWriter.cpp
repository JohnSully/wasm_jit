#include "stdafx.h"
#include "wasm_types.h"
#include "Exceptions.h"
#include "safe_access.h"
#include "JitWriter.h"

extern std::vector<FunctionTypeEntry::unique_pfne_ptr> g_vecfn_types;
extern std::vector<uint32_t> g_vecfn_entries;
extern std::vector<table_type> g_vectbl;
extern std::vector<resizable_limits> g_vecmem_types;
extern std::vector<export_entry> g_vecexports;
extern std::vector<FunctionCodeEntry::unique_pfne_ptr> g_vecfn_code;
extern std::vector<uint8_t> g_vecmem;
extern std::vector<int> g_vecimports;

void JitWriter::SafePushCode(const void *pv, size_t cb)
{
	if (m_pexecPlaneCur + cb > m_pexecPlaneMax)
		throw RuntimeException("No room to compile function");
	memcpy(m_pexecPlaneCur, pv, cb);
	m_pexecPlaneCur += cb;
}

void JitWriter::_PushExpandStack()
{
	// RAX -> stack
	// mov [rdi], rax		{ 0x48, 0x89, 0x07 }
	// add rdi, 8			{ 0x48, 0x83, 0xC7, 0x08 }
	static const uint8_t rgcode[] = { 0x48, 0x89, 0x07, 0x48, 0x83, 0xC7, 0x08 };
	SafePushCode(rgcode, _countof(rgcode));
}

// NOTE: Must not affect flags
void JitWriter::_PopContractStack()
{
	// mov rax, [rdi - 8]	{ 0x48, 0x8b, 0x47, 0xf8 }
	// lea rdi, [rdi - 8]	{ 0x48, 0x8D, 0x7F, 0xF8 }
	static const uint8_t rgcode[] = { 0x48, 0x8B, 0x47, 0xF8, 0x48, 0x8D, 0x7F, 0xF8 };
	SafePushCode(rgcode, _countof(rgcode));
}

void JitWriter::_LoadMem32(uint32_t offset)
{
	// mov eax, [rsi+rax+offset]	{ 0x8B, 0x84, 0x06, [4 byte offset] }
	static const uint8_t rgcode[] = { 0x8B, 0x84, 0x06 };
	SafePushCode(rgcode, _countof(rgcode));
	SafePushCode(&offset, sizeof(offset));
}

void JitWriter::_SetMem32(uint32_t offset)
{
	_PopSecondParam();
	// mov [rsi+rcx+offset], eax	{ 0x89, 0x84, 0x06, [4 byte offset] }
	static const uint8_t rgcode[] = { 0x89, 0x84, 0x06 };
	SafePushCode(rgcode, _countof(rgcode));
	SafePushCode(&offset, sizeof(offset));
}

void JitWriter::_PopSecondParam(bool fSwapParams)
{
	if (fSwapParams)
	{
		// mov rcx, rax
		// mov rax, [rdi - 8]
		// sub rdi, 8
		static const uint8_t rgcode[] = { 0x48, 0x89, 0xC1, 0x48, 0x8B, 0x47, 0xF8, 0x48, 0x83, 0xEF, 0x08 };
		SafePushCode(rgcode, _countof(rgcode));
	}
	else
	{
		// mov rcx, [rdi - 8]
		// sub rdi, 8
		static const uint8_t rgcode[] = { 0x48, 0x8B, 0x4F, 0xF8, 0x48, 0x83, 0xEF, 0x08 };
		SafePushCode(rgcode, _countof(rgcode));
	}
}

void JitWriter::LoadMem32(uint32_t offset)
{
	_LoadMem32(offset);
}

void JitWriter::LoadMem64(uint32_t offset)
{
	// mov rax, [rsi+rax+offset]
	static const uint8_t rgcode[] = { 0x48, 0x8B, 0x84, 0x06 };
	SafePushCode(rgcode);
	SafePushCode(&offset, sizeof(offset));
}

void JitWriter::LoadMem8(uint32_t offset, bool fSigned)
{
	if (fSigned)
	{
		// movsx rax, byte ptr [rsi + rax + offset]
		static const uint8_t rgcode[] = { 0x0F, 0xBE, 0x84, 0x06 };
		SafePushCode(rgcode, _countof(rgcode));
	}
	else
	{
		// movzx eax, byte ptr [rsi + rax + offset]
		static const uint8_t rgcode[] = { 0x0F, 0xB6, 0x84, 0x06 };
		SafePushCode(rgcode, _countof(rgcode));
	}
	SafePushCode(&offset, sizeof(offset));
}

void JitWriter::Load64Mem32(uint32_t offset, bool fSigned)
{
	if (fSigned)
	{
		// movsx rax, dword ptr [rsi + rax + offset]
		static const uint8_t rgcode[] = { 0x48, 0x63, 0x84, 0x06 };
		SafePushCode(rgcode, _countof(rgcode));
	}
	else
	{
		_LoadMem32(offset);	// implicit zero extend
	}
}

void JitWriter::StoreMem32(uint32_t offset)
{
	_SetMem32(offset);
	_PopContractStack();
}

void JitWriter::StoreMem64(uint32_t offset)
{
	_PopSecondParam();
	// mov [rsi+rcx+offset], rax	
	static const uint8_t rgcode[] = { 0x48, 0x89, 0x84, 0x0E };
	SafePushCode(rgcode);
	SafePushCode(offset);
	_PopContractStack();
}

void JitWriter::StoreMem8(uint32_t offset)
{
	_PopSecondParam();
	// mov [rsi+rcx+offset], al	
	static const uint8_t rgcode[] = { 0x88, 0x84, 0x0E };
	SafePushCode(rgcode);
	SafePushCode(offset);
	_PopContractStack();
}

void JitWriter::Sub32()
{
	_PopSecondParam();
	// sub eax, ecx
	static const uint8_t rgcode[] = { 0x29, 0xC8 };
	SafePushCode(rgcode, _countof(rgcode));
}

void JitWriter::Add32()
{
	_PopSecondParam();
	// add eax, ecx
	static const uint8_t rgcode[] = { 0x01, 0xC8 };
	SafePushCode(rgcode, _countof(rgcode));
}

void JitWriter::Add64()
{
	_PopSecondParam();
	// add rax, rcx
	static const uint8_t rgcode[] = { 0x48, 0x01, 0xC8 };
	SafePushCode(rgcode);
}

void JitWriter::Popcnt32()
{
	// mov [rdi], eax		; popcnt wants a memory operand - gross
	// popcnt eax, [rdi]
	const uint8_t rgcode[] = { 0x89, 0x07, 0xF3, 0x0F, 0xB8, 0x07 };
	SafePushCode(rgcode, _countof(rgcode));
}

void JitWriter::PushC32(uint32_t c)
{
	_PushExpandStack();
	if (c == 0)	// micro optimize zeroing
	{
		// xor eax, eax
		static const uint8_t rgcode[] = { 0x31, 0xC0 };
		SafePushCode(rgcode, _countof(rgcode));
	}
	else
	{
		// mov eax, c
		static const uint8_t rgcode[] = { 0xB8 };
		SafePushCode(rgcode, sizeof(uint8_t));
		SafePushCode(&c, sizeof(c));
	}
}

void JitWriter::PushC64(uint64_t c)
{
	_PushExpandStack();
	if (c == 0) // micro optimize zeroing
	{
		// xor rax, rax
		static const uint8_t rgcode[] = { 0x48, 0x31, 0xC0 };
		SafePushCode(rgcode, _countof(rgcode));
	}
	else
	{
		// mov rax, c
		static const uint8_t rgcode[] = { 0x48, 0xB8 };
		SafePushCode(rgcode, _countof(rgcode));
		SafePushCode(&c, sizeof(c));
	}
}

void JitWriter::Div64()
{
	// NYI: ud2
	static const uint8_t rgcode[] = { 0x0F, 0x0B };
	SafePushCode(rgcode, _countof(rgcode));
}

void JitWriter::SetLocal(uint32_t idx, bool fPop)
{
	// mov [rbx+idx], rax
	static const uint8_t rgcode[] = { 0x48, 0x89, 0x83 };
	SafePushCode(rgcode, _countof(rgcode));
	idx *= sizeof(uint64_t);
	SafePushCode(&idx, sizeof(idx));
	if (fPop)
		_PopContractStack();
}

void JitWriter::GetLocal(uint32_t idx)
{
	_PushExpandStack();
	// mov rax, [rbx+idx]
	static const uint8_t rgcode[] = { 0x48, 0x8B, 0x83 };
	SafePushCode(rgcode, _countof(rgcode));
	SafePushCode(&idx, sizeof(idx));
}

void JitWriter::EnterBlock()
{
	_PushExpandStack();	// backup rax
	// push rdi
	static const uint8_t rgcode[] = { 0x57 };
	SafePushCode(rgcode, _countof(rgcode));
}

void JitWriter::LeaveBlock(bool fHasReturn)
{
	// pop rdi
	static const uint8_t rgcode[] = { 0x5F };
	SafePushCode(rgcode, _countof(rgcode));
	if (!fHasReturn)
		_PopContractStack();
}

void JitWriter::Eqz32()
{
	//	test eax, eax		{ 0x85, 0xC0 }
	//	mov eax, 1			{ 0xb8, 0x01, 0x00, 0x00, 0x00}
	//  jz  Lz				{ 0x74, 0x02 }
	//	xor eax, eax		{ 0x31, 0xC0 }
	// Lz:
	static const uint8_t rgcode[] = { 0x85, 0xC0, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x74, 0x02, 0x31, 0xC0 };
	SafePushCode(rgcode, _countof(rgcode));
}

void JitWriter::Compare(CompareType type, bool fSigned, bool f64)
{
	_PopSecondParam();
	//		cmp rcx, rax		{ 0x48, 0x39, 0xc1 }			// yes the order is reversed... this is by the spec
	//		mov eax, 1			{ b8 01 00 00 00}
	//		jcc LTrue			{ XX, 0x02 }	2-byte forward rel jump
	//		xor eax, eax		{ 31 c0 }
	// LTrue:
	static const uint8_t rgcodeCmp64[] = { 0x48, 0x39, 0xc1 };
	static const uint8_t rgcodeCmp32[] = { 0x39, 0xC1 };
	static const uint8_t rgcodePre[] = { 0xb8, 0x01, 0x00, 0x00, 0x00 };
	static const uint8_t rgcodePost[] = { 0x02, 0x31, 0xc0 };

	if (f64)
		SafePushCode(rgcodeCmp64, _countof(rgcodeCmp64));
	else
		SafePushCode(rgcodeCmp32, _countof(rgcodeCmp32));

	SafePushCode(rgcodePre, _countof(rgcodePre));
	uint8_t cmpOp = 0xCC;	// int3 BUG
	switch (type)
	{
	case CompareType::LessThan:
		if (fSigned)
			cmpOp = 0x7C;	// JL  rel8
		else
			cmpOp = 0x72;	// JB  rel8
		break;

	case CompareType::LessThanEqual:
		if (fSigned)
			cmpOp = 0x7E;	// JLE rel8
		else
			cmpOp = 0x76;	// JBE rel8
		break;

	case CompareType::Equal:
		cmpOp = 0x74;		// JE rel8
		break;

	case CompareType::NotEqual:
		cmpOp = 0x75;		// JNE rel8
		break;

	case CompareType::GreaterThanEqual:
		if (fSigned)
			cmpOp = 0x7D;	// JGE rel8
		else
			cmpOp = 0x73;	// JAE rel8
		break;

	case CompareType::GreaterThan:
		if (fSigned)
			cmpOp = 0x7F;	// JG rel8
		else
			cmpOp = 0x74;	// JE rel8
		break;

	default:
		Verify(false);
	}
	SafePushCode(&cmpOp, sizeof(cmpOp));
	SafePushCode(rgcodePost, _countof(rgcodePost));
}

int32_t *JitWriter::JumpNIf(void *pvJmp)
{
	int32_t offset = -6;
	// test rax, rax { 0x48, 0x85, 0xC0 }
	static const uint8_t rgcodeTest[] = { 0x48, 0x85, 0xC0 };
	SafePushCode(rgcodeTest, _countof(rgcodeTest));
	_PopContractStack();		// does not affect flags

	if (pvJmp != nullptr)
	{
		offset = reinterpret_cast<uint8_t*>(pvJmp) - (reinterpret_cast<uint8_t*>(m_pexecPlaneCur) + 6);
	}
	// JZ rel32	{ 0x0F, 0x84, REL32 }
	static const uint8_t rgcodeJRel[] = { 0x0F, 0x84 };
	SafePushCode(rgcodeJRel, _countof(rgcodeJRel));
	int32_t *poffsetRet = (int32_t*)m_pexecPlaneCur;
	SafePushCode(&offset, sizeof(offset));
	return poffsetRet;
}

int32_t *JitWriter::Jump(void *pvJmp)
{
	int32_t offset = -5;
	
	if (pvJmp != nullptr)
	{
		offset = reinterpret_cast<uint8_t*>(pvJmp) - (reinterpret_cast<uint8_t*>(m_pexecPlaneCur) + 5);
	}
	// JMP rel32	{ 0xE9, REL32 }
	static const uint8_t rgcodeJRel[] = { 0xE9 };
	SafePushCode(rgcodeJRel, _countof(rgcodeJRel));
	int32_t *poffsetRet = (int32_t*)m_pexecPlaneCur;
	SafePushCode(&offset, sizeof(offset));
	return poffsetRet;
}

void JitWriter::CallIfn(uint32_t ifn, uint32_t clocalsCaller, uint32_t cargsCallee, bool fReturnValue)
{
	// Stage 1: Push arguments
	//	add	 rbx, (clocalsCaller * sizeof(uint64_t))
	static const uint8_t rgcodeAllocLocals[] = { 0x48, 0x81, 0xC3 };
	SafePushCode(rgcodeAllocLocals, _countof(rgcodeAllocLocals));
	uint32_t cbLocals = (clocalsCaller * sizeof(uint64_t));
	SafePushCode(&cbLocals, sizeof(cbLocals));

	// pop arguments into the newly allocated local variable region
	while (cargsCallee > 0)
	{
		SetLocal(cargsCallee - 1, true);
		--cargsCallee;
	}
	_PushExpandStack();	// put the top of the stack into RAM so we can recover it
	//  push rdi		; backup the operand stack
	static const uint8_t rgcode[] = { 0x57 };
	SafePushCode(rgcode, _countof(rgcode));

	// Stage 2, call the actual function
	// call [rip - PfnVector]
	static const uint8_t rgcodeCall[] = { 0xFF, 0x15 };
	int32_t offset = RelAddrPfnVector(ifn, 6);
	SafePushCode(rgcodeCall, _countof(rgcodeCall));
	SafePushCode(&offset, sizeof(offset));

	// Stage 3, on return cleanup the stack
	//	pop rdi
	//	sub rbx, (clocalsCaller * sizeof(uint64_t))
	static const uint8_t rgcodeCleanup[] = { 0x5F, 0x48, 0x81, 0xEB };
	SafePushCode(rgcodeCleanup, _countof(rgcodeCleanup));
	SafePushCode(&cbLocals, sizeof(cbLocals));

	// Stage 4, if there was no return value then place our last operand back in rax
	if (!fReturnValue)
	{
		_PopContractStack();
	}
}

void JitWriter::FnPrologue(uint32_t clocals, uint32_t cargs)
{
	// memset local variables to zero
	// ZeroMemory(rbx + (cargs * sizeof(uint64_t)), (clocals - cargs) * sizeof(uint64_t))
	uint32_t clocalsNoArgs = clocals - cargs;

	if (clocalsNoArgs > 0)
	{
		// xor eax, eax						; rax is clobbered so no need to save it
		// lea rdx, [rbx + localOffset]
		// mov rcx, (clocals - cargs)
		// rep stos qword ptr [rdx]

		static const uint8_t rgcodeXorEax[] = { 0x31, 0xC0 };	// xor eax, eax
		SafePushCode(rgcodeXorEax, _countof(rgcodeXorEax));

		static const uint8_t rgcodeLeaRdx[] = { 0x48, 0x8D, 0x93 };	// lea rdx, [rbx + XYZ]
		int32_t cbOffset = (cargs * sizeof(uint64_t));
		SafePushCode(rgcodeLeaRdx, _countof(rgcodeLeaRdx));
		SafePushCode(&cbOffset, sizeof(cbOffset));

		static const uint8_t rgcodeMovRcx[] = { 0x48, 0xC7, 0xC1 };	// mov rcx, XYZ
		SafePushCode(rgcodeMovRcx, _countof(rgcodeMovRcx));

		SafePushCode(&clocalsNoArgs, sizeof(clocalsNoArgs));

		static const uint8_t rgcodeRepStos[] = { 0xF3, 0x48, 0xAB };	// rep stos qword ptr [rdx]
		SafePushCode(rgcodeRepStos, _countof(rgcodeRepStos));
	}
}

void JitWriter::LogicOp(LogicOperation op)
{
	bool fSwapParams = false;
	switch (op)
	{
	case LogicOperation::ShiftLeft:
	case LogicOperation::ShiftRight:
	case LogicOperation::ShiftRightUnsigned:
		fSwapParams = true;
	}
	_PopSecondParam(fSwapParams);
	const char *rgcode = nullptr;
	switch (op)
	{
	case LogicOperation::And:
		// and eax, ecx
		rgcode = "\x21\xC8";
		break;
	case LogicOperation::Or:
		// or eax, ecx
		rgcode = "\x09\xC8";
		break;
	case LogicOperation::Xor:
		// xor eax, ecx
		rgcode = "\x31\xC8";
		break;
	case LogicOperation::ShiftLeft:
		// shl eax, cl
		rgcode = "\xD3\xE0";
		break;
	case LogicOperation::ShiftRight:
		// sar eax, cl
		rgcode = "\xD3\xF8";
		break;
	case LogicOperation::ShiftRightUnsigned:
		// shr eax, cl
		rgcode = "\xD3\xE8";
		break;
	default:
		Verify(false);
	}
	SafePushCode(rgcode, strlen(rgcode));
}

void JitWriter::LogicOp64(LogicOperation op)
{
	bool fSwapParams = false;
	switch (op)
	{
	case LogicOperation::ShiftLeft:
	case LogicOperation::ShiftRight:
	case LogicOperation::ShiftRightUnsigned:
		fSwapParams = true;
	}
	_PopSecondParam(fSwapParams);
	const char *rgcode = nullptr;
	switch (op)
	{
	case LogicOperation::And:
		// and rax, rcx
		rgcode = "\x48\x21\xC8";
		break;
	case LogicOperation::Or:
		// or rax, rcx
		rgcode = "\x48\x09\xC8";
		break;
	case LogicOperation::Xor:
		// xor rax, rcx
		rgcode = "\x48\x31\xC8";
		break;
	case LogicOperation::ShiftLeft:
		// shl rax, cl
		rgcode = "\x48\xD3\xE0";
		break;
	case LogicOperation::ShiftRightUnsigned:
		// shr rax, cl
		rgcode = "\x48\xD3\xE8";
		break;
	default:
		Verify(false);
	}
	SafePushCode(rgcode, strlen(rgcode));
}

void JitWriter::Select()
{
	//	sub rdi, 16			; pop 2 vals from stack
	//	xor rcx, rcx		; zero our index
	//	test eax, eax		; test the conditional
	//	jnz FirstArg			; jump if index should be zero
	//	add rcx, 8			; else index should be 8 (bytes)
	// FirstArg:
	//	mov rax, [rdi+rcx]	; load the value
	static const uint8_t rgcode[] = { 0x48, 0x83, 0xEF, 0x10, 0x48, 0x31, 0xC9, 0x85, 0xC0, 0x74, 0x04, 0x48, 0x83, 0xC1, 0x08, 0x48, 0x8B, 0x04, 0x0F };
	SafePushCode(rgcode, _countof(rgcode));
}

void JitWriter::FnEpilogue()
{
	// ret
	static const uint8_t rgcode[] = { 0xC3 };
	SafePushCode(rgcode, _countof(rgcode));
}

void JitWriter::_SetDbgReg(uint32_t op)
{
	// mov r12, op
	static const uint8_t rgcode[] = { 0x49, 0xC7, 0xC4 };
	SafePushCode(rgcode, _countof(rgcode));
	SafePushCode(&op, sizeof(op));
}

void JitWriter::Mul32()
{
	// mul dword ptr [rdi-8]		; note: clobbers edx
	// sub rdi, 8
	static const uint8_t rgcode[] = { 0xF7, 0x67, 0xF8, 0x48, 0x83, 0xEF, 0x08 };
	SafePushCode(rgcode);
}

void JitWriter::CompileFn(uint32_t ifn)
{
	FunctionCodeEntry *pfnc = g_vecfn_code[ifn - g_vecimports.size()].get();
	const uint8_t *pop = pfnc->vecbytecode.data();
	size_t cb = pfnc->vecbytecode.size();
	std::vector<std::pair<value_type, void*>> stackBlockTypeAddr;
	std::vector<std::vector<int32_t*>> stackVecFixups;

	reinterpret_cast<void**>(m_pexecPlane)[ifn] = m_pexecPlaneCur;	// set our entry in the vector table

	size_t itype = g_vecfn_entries[ifn];
	size_t cparams = g_vecfn_types[itype]->cparams;
	size_t clocals = cparams;
	for (size_t ilocalInfo = 0; ilocalInfo < pfnc->clocalVars; ++ilocalInfo)
	{
		clocals += pfnc->rglocals[ilocalInfo].count;
	}

	std::vector<uint32_t> vecifnCompile;

	FnPrologue(clocals, cparams);
	printf("Function %d:\n", ifn);
	while (cb > 0)
	{
		cb--;	// count *pop
		++pop;
		_SetDbgReg(*(pop - 1));
		printf("%p (%X):\t", m_pexecPlaneCur, *(pop - 1));
		for (size_t itab = 0; itab < stackBlockTypeAddr.size(); ++itab)
			printf("\t");
		switch ((opcode)*(pop - 1))
		{
		case opcode::unreachable:
		{
			// ud2
			printf("unreachable\n");
			static const uint8_t rgcode[] = { 0x0F, 0x0B };
			SafePushCode(rgcode, _countof(rgcode));
			break;
		}
		case opcode::block:
		{
			value_type type = safe_read_buffer<value_type>(&pop, &cb);
			printf("block\n");
			stackBlockTypeAddr.push_back(std::make_pair(type, nullptr));	// nullptr means we need to fixup addrs
			stackVecFixups.push_back(std::vector<int32_t*>());
			EnterBlock();
			break;
		}

		case opcode::loop:
		{
			value_type type = safe_read_buffer<value_type>(&pop, &cb);
			printf("loop\n");
			stackBlockTypeAddr.push_back(std::make_pair(type, m_pexecPlaneCur));
			stackVecFixups.push_back(std::vector<int32_t*>());
			EnterBlock();
			break;
		}

		case opcode::br:
		{
			uint32_t depth = safe_read_buffer<varuint32>(&pop, &cb);
			printf("br %u\n", depth);
			Verify(depth < stackBlockTypeAddr.size());
			auto &pairBlock = *(stackBlockTypeAddr.rbegin() + depth);

			// leave intermediate blocks (lie that we have a return so we don't do useless stack operations)
			for (uint32_t idepth = 1; idepth < depth; ++idepth)
			{
				LeaveBlock(true);
			}
			LeaveBlock(pairBlock.first != value_type::empty_block);

			int32_t *pdeltaFix = Jump(pairBlock.second);
			if (pairBlock.second == nullptr)
			{
				stackVecFixups.back().push_back(pdeltaFix);
			}
			break;
		}
		case opcode::br_if:
		{
			uint32_t depth = safe_read_buffer<varuint32>(&pop, &cb);
			printf("br_if %u\n", depth);
			Verify(depth < stackBlockTypeAddr.size());
			auto &pairBlock = *(stackBlockTypeAddr.rbegin() + depth);
			
			int32_t *pdeltaNoJmp = JumpNIf(nullptr);	// skip everything if we won't jump
			// leave intermediate blocks (lie that we have a return so we don't do useless stack operations)
			for (uint32_t idepth = 1; idepth < depth; ++idepth)
			{
				LeaveBlock(true);
			}
			LeaveBlock(pairBlock.first != value_type::empty_block);
			
			int32_t *pdeltaFix = Jump(pairBlock.second);
			if (pairBlock.second == nullptr)
			{
				stackVecFixups.back().push_back(pdeltaFix);
			}
			*pdeltaNoJmp = m_pexecPlaneCur - (reinterpret_cast<uint8_t*>(pdeltaNoJmp) + sizeof(*pdeltaNoJmp));
			break;
		}
		case opcode::br_table:
		{
			Verify(false);	// NYI
			break;
		}

		case opcode::ret:
		{
			printf("return\n");
			FnEpilogue();
			break;
		}

		case opcode::call:
		{
			uint32_t idx = safe_read_buffer<varuint32>(&pop, &cb);
			printf("call %d\n", idx);
			Verify(idx < m_cfn);
			vecifnCompile.push_back(idx);
			auto ptype = g_vecfn_types.at(g_vecfn_entries.at(idx)).get();
			CallIfn(idx, clocals, ptype->cparams, ptype->fHasReturnValue);
			break;
		}
		case opcode::call_indirect:
		{
			printf("call_indirect\n");
			uint32_t idx = safe_read_buffer<varuint32>(&pop, &cb);
			safe_read_buffer<char>(&pop, &cb);	// reserved
			static const uint8_t rgcode[] = { 0x0F, 0x0B };	// NYI
			SafePushCode(rgcode, _countof(rgcode));
			break;
		}

		case opcode::drop:
		{
			printf("drop\n");
			_PopContractStack();
			break;
		}
		case opcode::select:
		{
			printf("select\n");
			Select();
			break;
		}

		case opcode::i32_const:
		{
			uint32_t val = safe_read_buffer<varint32>(&pop, &cb);
			printf("i32.const %d\n", val);
			PushC32(val);
			break;
		}
		case opcode::i64_const:
		{
			uint64_t val = safe_read_buffer<varuint64>(&pop, &cb);
			printf("i64.const %llu\n", val);
			PushC64(val);
			break;
		}

		case opcode::i32_eqz:
			printf("i32.eqz\n");
			Eqz32();
			break;
		case opcode::i32_eq:
			printf("i32.eq\n");
			Compare(CompareType::Equal, false /*fSigned*/, false /*f64*/);	// Note: signedness is irrelevant
			break;
		case opcode::i32_ne:
			printf("i32.ne\n");
			Compare(CompareType::NotEqual, false /*fSigned*/, false /*f64*/); // Note: signedness is irrelevant
			break;
		case opcode::i32_lt_s:
			printf("i32.lt_s\n");
			Compare(CompareType::LessThan, true /*fSigned*/, false /*f64*/);
			break;
		case opcode::i32_lt_u:
			printf("i32.lt_u\n");
			Compare(CompareType::LessThan, false /*fSigned*/, false /*f64*/);
			break;
		case opcode::i32_gt_s:
			printf("i32.gt_s\n");
			Compare(CompareType::GreaterThan, true /*fSigned*/, false /*f64*/);
			break;
		case opcode::i32_gt_u:
			printf("i32.gt_u\n");
			Compare(CompareType::GreaterThan, false /*fSigned*/, false /*f64*/);
			break;
		case opcode::i32_le_s:
			printf("i32_le_s\n");
			Compare(CompareType::LessThanEqual, true /*fSigned*/, false /*f64*/);
			break;
		case opcode::i32_le_u:
			printf("i32_le_u\n");
			Compare(CompareType::LessThanEqual, false /*fSigned*/, false /*f64*/);
			break;
		case opcode::i32_ge_s:
			printf("i32_ge_s\n");
			Compare(CompareType::GreaterThanEqual, true /*fSigned*/, false /*f64*/);
			break;
		case opcode::i32_ge_u:
			printf("i32_ge_u\n");
			Compare(CompareType::GreaterThanEqual, false /*fSigned*/, false /*f64*/);
			break;

		case opcode::i64_ne:
			printf("i64_ne\n");
			Compare(CompareType::NotEqual, false /*fSigned*/, true /*f64*/);
			break;

		case opcode::i32_load:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
			printf("i32_load $%X\n", offset);
			LoadMem32(offset);
			break;
		}
		case opcode::i32_load8_u:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
			printf("i32_load8_u $%X\n", offset);
			LoadMem8(offset, false /*fSigned*/);
			break;
		}
		case opcode::i32_load8_s:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
			printf("i32_load8_s $%X\n", offset);
			LoadMem8(offset, true /*fSigned*/);
			break;
		}

		case opcode::i64_load:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
			printf("i64.load $%X\n");
			LoadMem64(offset);
			break;
		}

		case opcode::i64_load32_s:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
			printf("i64_load32_s $%X\n", offset);
			Load64Mem32(offset, true /*fSigned*/);
			break;
		}

		case opcode::get_local:
		{
			uint32_t idx = safe_read_buffer<varuint32>(&pop, &cb);
			printf("get_local $%X\n", idx);
			Verify(idx < clocals);
			GetLocal(idx);
			break;
		}
		case opcode::set_local:
		{
			uint32_t idx = safe_read_buffer<varuint32>(&pop, &cb);
			printf("set_local $%X\n", idx);
			Verify(idx < clocals);
			SetLocal(idx, true /*fPop*/);
			break;
		}
		case opcode::tee_local:
		{
			uint32_t idx = safe_read_buffer<varuint32>(&pop, &cb);
			printf("tee_local $%X\n", idx);
			Verify(idx < clocals);
			SetLocal(idx, false /*fPop*/);
			break;
		}

		case opcode::i32_store:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
			printf("i32.store $%X\n", offset);
			StoreMem32(offset);
			break;
		}

		case opcode::i64_store:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
			printf("i64.store $%X\n", offset);
			StoreMem64(offset);
			break;
		}

		case opcode::i32_store8:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
			printf("i32.store8 $%X\n", offset);
			StoreMem8(offset);
			break;
		}

		case opcode::i32_popcnt:
			printf("i32.popcnt\n");
			Popcnt32();
			break;
		case opcode::i32_add:
			printf("i32.add\n");
			Add32();
			break;
		case opcode::i32_sub:
			printf("i32.sub\n");
			Sub32();
			break;
		case opcode::i32_mul:
			printf("i32.mul\n");
			Mul32();
			break;
		case opcode::i32_and:
			printf("i32.and\n");
			LogicOp(LogicOperation::And);
			break;
		case opcode::i32_or:
			printf("i32.or\n");
			LogicOp(LogicOperation::Or);
			break;
		case opcode::i32_xor:
			printf("i32.xor\n");
			LogicOp(LogicOperation::Xor);
			break;
		case opcode::i32_shl:
			printf("i32.shl\n");
			LogicOp(LogicOperation::ShiftLeft);
			break;
		case opcode::i32_shr_s:
			printf("i32.shr_s\n");
			LogicOp(LogicOperation::ShiftRight);
			break;
		case opcode::i32_shr_u:
			printf("i32.shr_u\n");
			LogicOp(LogicOperation::ShiftRightUnsigned);
			break;

		case opcode::i64_add:
			printf("i64.add\n");
			Add64();
			break;
		case opcode::i64_div_u:
			printf("i64.div_u\n");
			Div64();
			break;
		case opcode::i64_and:
			printf("i64.and\n");
			LogicOp64(LogicOperation::And);
			break;
		case opcode::i64_or:
			printf("i64.or\n");
			LogicOp64(LogicOperation::Or);
			break;
		case opcode::i64_shl:
			printf("i64.shl\n");
			LogicOp64(LogicOperation::ShiftLeft);
			break;

		case opcode::i32_wrap_i64:
			printf("i32.wrap/i64\n");
			break;	// NOP because accessing eax, even when set as rax has the same behavior

		case opcode::i64_extend_u32:
			printf("i64.extend_u/i32\n");
			break;	// NOP because the upper register should be zero regardless

		case opcode::end:
			printf("end\n");
			if (stackBlockTypeAddr.empty())
				break;
			LeaveBlock(stackBlockTypeAddr.back().first != value_type::empty_block);
			// Jump targets are after the LeaveBlock because the branch already performs the work (TODO: Maybe not do that?)
			for (int32_t *poffsetFix : stackVecFixups.back())
			{
				*poffsetFix = m_pexecPlaneCur - (reinterpret_cast<uint8_t*>(poffsetFix) + sizeof(*poffsetFix));
			}
			stackBlockTypeAddr.pop_back();
			stackVecFixups.pop_back();
			break;
			
		default:
			throw RuntimeException("Invalid opcode");

		}
	}
	FnEpilogue();

	for (uint32_t ifnCompile : vecifnCompile)
	{
		void *&pfn = reinterpret_cast<void**>(m_pexecPlane)[ifnCompile];
		if (pfn == nullptr)
		{
			CompileFn(ifnCompile);
		}
	}
	printf("\n\n");
}

extern "C" uint64_t ExternCallFnASM(void *pfn, void *operandStack, void *localsStack, void *memoryBase);

void JitWriter::ExternCallFn(uint32_t ifn, void *pvAddr)
{
	uint64_t retV;
	void *&pfn = reinterpret_cast<void**>(m_pexecPlane)[ifn];
	if (pfn == nullptr)
	{
		CompileFn(ifn);
	}
	std::vector<uint64_t> vecoperand;
	std::vector<uint64_t> veclocals;
	vecoperand.resize(4096);
	veclocals.resize(4096);
	Verify(pfn != nullptr);
	retV = ExternCallFnASM(pfn, vecoperand.data(), veclocals.data(), g_vecmem.data());
}