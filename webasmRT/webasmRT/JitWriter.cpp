#include "stdafx.h"
#include "wasm_types.h"
#include "Exceptions.h"
#include "safe_access.h"
#include "JitWriter.h"
#include <Windows.h>

extern std::vector<FunctionTypeEntry::unique_pfne_ptr> g_vecfn_types;
extern std::vector<uint32_t> g_vecfn_entries;
extern std::vector<table_type> g_vectbl;
extern std::vector<resizable_limits> g_vecmem_types;
extern std::vector<export_entry> g_vecexports;
extern std::vector<FunctionCodeEntry::unique_pfne_ptr> g_vecfn_code;
extern std::vector<uint8_t> g_vecmem;
extern std::vector<int> g_vecimports;
extern std::vector<uint32_t> g_vecIndirectFnTable;
struct GlobalVar
{
	uint64_t val;
	value_type type;
	bool fMutable;
};
extern std::vector<GlobalVar> g_vecglbls;
extern "C" void WasmToC();
extern "C" void CallIndirectShim();
extern "C" void BranchTable();

JitWriter::JitWriter(uint8_t *pexecPlane, size_t cbExec, size_t cfn, size_t cglbls)
	: m_pexecPlane(pexecPlane), m_pexecPlaneCur(pexecPlane), m_pexecPlaneMax(pexecPlane + cbExec), m_cfn(cfn)
{
	uint8_t *pvZeroStart = m_pexecPlaneCur;
	m_pexecPlaneCur += sizeof(void*) * cfn;	// allocate the function table, ensuring its within 32-bits of all our code
	
	m_pfnCallIndirectShim = (void**)m_pexecPlaneCur;
	m_pfnBranchTable = ((void**)m_pexecPlaneCur) + 1;
	m_pexecPlaneCur += sizeof(*m_pfnCallIndirectShim) * 2;

	m_pexecPlaneCur += (4096 - reinterpret_cast<uint64_t>(m_pexecPlaneCur)) % 4096;
	m_pGlobalsStart = (uint64_t*)m_pexecPlaneCur;
	m_pexecPlaneCur += (sizeof(uint64_t) * cglbls);
	m_pexecPlaneCur += (4096 - reinterpret_cast<uint64_t>(m_pexecPlaneCur)) % 4096;
	m_pcodeStart = m_pexecPlaneCur;
	memset(pvZeroStart, 0, m_pexecPlaneCur - pvZeroStart);	// these areas should be initialized to zero

	for (size_t iimportfn = 0; iimportfn < g_vecimports.size(); ++iimportfn)
		reinterpret_cast<void**>(m_pexecPlane)[iimportfn] = WasmToC;
	*m_pfnCallIndirectShim = CallIndirectShim;
	*m_pfnBranchTable = BranchTable;
}

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


void JitWriter::LoadMem(uint32_t offset, bool f64Dst /* else 32 */, uint32_t cbSrc, bool fSignExtend)
{
	// add eax, offset	; note this implicitly ands with 0xffffffff ensuring that when referenced as rax it will always be a positive number between 0 and 2^32 - 1
	static const uint8_t rgcodeAdd[] = { 0x05 };
	SafePushCode(rgcodeAdd);
	SafePushCode(offset);

	const char *szCode = nullptr;
	if (fSignExtend)
	{
		if (f64Dst)
		{
			switch (cbSrc)
			{
			default:
				Verify(false);
				break;

			case 1:
				// movsx rax, byte ptr [rsi+rax]
				szCode = "\x48\x0F\xBE\x04\x06";
				break;

			case 2:
				// movsx rax, word ptr [rsi+rax]
				szCode = "\x48\x0F\xBF\x04\x06";
				break;

			case 4:
				// movsx rax, dword ptr [rsi+rax]
				szCode = "\x48\x63\x04\x06";
				break;
			}
		}
		else
		{
			switch (cbSrc)
			{
			default:
				Verify(false);
				break;

			case 1:
				// movsx eax, byte ptr [rsi + rax]
				szCode = "\x0F\xBE\x04\x06";
				break;

			case 2:
				// movsx eax, word ptr [rsi + rax]
				szCode = "\x0F\xBF\x04\x06";
			}
		}
	}
	else
	{
		switch (cbSrc)
		{
		case 1:
			// movzx eax, byte ptr [rsi+rax]
			szCode = "\x0F\xB6\x04\x06";
			break;

		case 2:
			// movzx eax, word ptr [rsi + rax]
			szCode = "\x0F\xB7\x04\x06";
			break;

		case 4:
			// mov eax, dword ptr [rsi + rax]
			szCode = "\x8B\x04\x06";
			break;

		case 8:
			Verify(f64Dst);
			// mov rax, qword ptr [rsi + rax]
			szCode = "\x48\x8B\x04\x06";
			break;

		default:
			Verify(false);
		}
	}
	Verify(szCode != nullptr);
	SafePushCode(szCode, strlen(szCode));
}
void JitWriter::StoreMem(uint32_t offset, uint32_t cbDst)
{
	_PopSecondParam();
	// add ecx, offset	; note this implicitly ands with 0xffffffff ensuring that when referenced as rax it will always be a positive number between 0 and 2^32 - 1
	static const uint8_t rgcodeAdd[] = { 0x81, 0xC1 };
	SafePushCode(rgcodeAdd);
	SafePushCode(offset);

	const char *szCode = nullptr;
	switch (cbDst)
	{
	default:
		Verify(false);
		break;

	case 1:
		// mov [rsi + rcx], al
		szCode = "\x88\x04\x0E";
		break;

	case 2:
		// mov [rsi + rcx], ax
		szCode = "\x66\x89\x04\x0E";
		break;

	case 4:
		// mov [rsi + rcx], eax
		szCode = "\x89\x04\x0E";
		break;

	case 8:
		// mov [rsi + rcx], rax
		szCode = "\x48\x89\x04\x0E";
		break;
	}
	Verify(szCode != nullptr);
	SafePushCode(szCode, strlen(szCode));
}

void JitWriter::Sub32()
{
	_PopSecondParam(true);
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

void JitWriter::Sub64()
{
	_PopSecondParam();
	// sub rax, rcx
	static const uint8_t rgcode[] = { 0x48, 0x29, 0xC8 };
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

void JitWriter::Ud2()
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
	idx *= sizeof(uint64_t);
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

void JitWriter::Eqz64()
{
	//	test rax, rax		{ 0x48, 0x85, 0xC0 }
	//	mov eax, 1			{ 0xb8, 0x01, 0x00, 0x00, 0x00}
	//  jz  Lz				{ 0x74, 0x02 }
	//	xor eax, eax		{ 0x31, 0xC0 }
	// Lz:
	static const uint8_t rgcode[] = { 0x48, 0x85, 0xC0, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x74, 0x02, 0x31, 0xC0 };
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

void JitWriter::CallIfn(uint32_t ifn, uint32_t clocalsCaller, uint32_t cargsCallee, bool fReturnValue, bool fIndirect)
{
	// Stage 1: Push arguments
	//	add	 rbx, (clocalsCaller * sizeof(uint64_t))
	static const uint8_t rgcodeAllocLocals[] = { 0x48, 0x81, 0xC3 };
	SafePushCode(rgcodeAllocLocals, _countof(rgcodeAllocLocals));
	uint32_t cbLocals = (clocalsCaller * sizeof(uint64_t));
	SafePushCode(&cbLocals, sizeof(cbLocals));

	if (fIndirect)
	{
		// the top of stack is the function index
		// back it up into rcx (mov rcx, rax)
		static const uint8_t rgT[] = { 0x48, 0x89, 0xC1 };
		SafePushCode(rgT);
		_PopContractStack();
	}

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

	if (fIndirect)
	{
		// ifn is the type in this case
		// mov eax, ifn
		SafePushCode(uint8_t(0xB8));
		SafePushCode(uint32_t(ifn));

		// call [m_pfnCallIndirectShim]
		static const uint8_t rgcodeCallIndirect[] = { uint8_t(0xFF), uint8_t(0x15) };
		SafePushCode(rgcodeCallIndirect);
		ptrdiff_t diffFn = reinterpret_cast<ptrdiff_t>(m_pfnCallIndirectShim) - reinterpret_cast<ptrdiff_t>(m_pexecPlaneCur + 4);
		Verify(static_cast<int32_t>(diffFn) == diffFn);
		SafePushCode(int32_t(diffFn));
	}
	else
	{
		if (ifn < g_vecimports.size())
		{
			// mov ecx ifn ; so we know the function
			static const uint8_t rgcodeFnNum[] = { 0xB9 };
			SafePushCode(rgcodeFnNum);
			SafePushCode(ifn);
		}

		// Stage 2, call the actual function
		// call [rip - PfnVector]
		static const uint8_t rgcodeCall[] = { 0xFF, 0x15 };
		int32_t offset = RelAddrPfnVector(ifn, 6);
		SafePushCode(rgcodeCall, _countof(rgcodeCall));
		SafePushCode(&offset, sizeof(offset));
	}

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
	//	jnz FirstArg		; jump if index should be zero
	//	add rcx, 8			; else index should be 8 (bytes)
	// FirstArg:
	//	mov rax, [rdi+rcx]	; load the value
	static const uint8_t rgcode[] = { 0x48, 0x83, 0xEF, 0x10, 0x48, 0x31, 0xC9, 0x85, 0xC0, 0x75, 0x04, 0x48, 0x83, 0xC1, 0x08, 0x48, 0x8B, 0x04, 0x0F };
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


void JitWriter::Mul64()
{
	// mul qword ptr [rdi-8]		; note: clobbers rdx
	// sub rdi, 8
	static const uint8_t rgcode[] = { 0x48, 0xF7, 0x67, 0xF8, 0x48, 0x83, 0xEF, 0x08 };
	SafePushCode(rgcode);
}


void JitWriter::BranchTableParse(const uint8_t **ppoperand, size_t *pcbOperand, const std::vector<std::pair<value_type, void*>> &stackBlockTypeAddr, std::vector<std::vector<int32_t*>> &stackVecFixups, std::vector<std::vector<void**>> &stackVecFixupsAbsolute)
{
	uint32_t target_count = safe_read_buffer<varuint32>(ppoperand, pcbOperand);
	std::vector<uint32_t> vectargets;
	vectargets.reserve(target_count);
	for (uint32_t itarget = 0; itarget < target_count; ++itarget)
	{
		vectargets.push_back(safe_read_buffer<varuint32>(ppoperand, pcbOperand));
	}
	uint32_t default_target = safe_read_buffer<varuint32>(ppoperand, pcbOperand);

	auto &pairBlockDft = *(stackBlockTypeAddr.rbegin() + default_target);
	bool fRetVal = (pairBlockDft.first != value_type::empty_block);

	// Setup the parameters and call thr BranchTable helper
	//	rcx - table pointer
	//
	//	lea rcx, [rip+distance_to_branch_table]		{ 0x48, 0x8D, 0x0D, rel32 }
	//	jmp [m_pfnBranchTable]						{ 0xFF, 0x25, rel32 }
	int32_t branchtable_offset = 6;
	static const uint8_t rgcodeLea[] = { 0x48, 0x8D, 0x0D };
	SafePushCode(rgcodeLea);
	SafePushCode(branchtable_offset);
	
	static const uint8_t rgcodeJmp[] = { 0xFF, 0x25 };
	SafePushCode(rgcodeJmp);
	ptrdiff_t diffFn = reinterpret_cast<ptrdiff_t>(m_pfnBranchTable) - reinterpret_cast<ptrdiff_t>(m_pexecPlaneCur + 4);
	Verify(diffFn == static_cast<int32_t>(diffFn));
	SafePushCode(static_cast<int32_t>(diffFn));

	// Now write the branch table
	/*
	;	Table Format:
	;		dword count
	;		dword fReturnVal
	;		--default_target--
	;		qword distance (how many blocks we're jumping)
	;		qword target_addr
	;		--table_targets---
	;		.... * count
	*/
	// Header
	SafePushCode(target_count);
	SafePushCode(int32_t(fRetVal));
	// default target
	SafePushCode(uint64_t(default_target));
	SafePushCode(pairBlockDft.second);
	if (pairBlockDft.second == nullptr)
		(stackVecFixupsAbsolute.rbegin() + default_target)->push_back(reinterpret_cast<void**>(m_pexecPlaneCur) - 1);
	// table targets
	for (uint32_t itarget = 0; itarget < target_count; ++itarget)
	{
		uint32_t target = vectargets[itarget];
		SafePushCode(target);
		auto &pairBlock = *(stackBlockTypeAddr.rbegin() + target);
		SafePushCode(pairBlock.second);
		if (pairBlock.second == nullptr)
			(stackVecFixupsAbsolute.rbegin() + default_target)->push_back(reinterpret_cast<void**>(m_pexecPlaneCur) - 1);
	}
}

void JitWriter::GetGlobal(uint32_t idx)
{
	auto &glbl = g_vecglbls.at(idx);

	if (glbl.fMutable)
	{
		_PushExpandStack();
		const char *szCode = nullptr;
		switch (glbl.type)
		{
		case value_type::i32:
			// mov eax, [m_pGlobalsStart + idx*8] (rip relative)
			szCode = "\x8B\x05";
			break;

		default:
			Verify(false);
		}
		SafePushCode(szCode, strlen(szCode));
		// now push the address offset
		int32_t offset = reinterpret_cast<uint8_t*>(m_pGlobalsStart + idx) - (m_pexecPlaneCur + 4);
		SafePushCode(offset);
	}
	else
	{
		switch (glbl.type)
		{
		case value_type::i32:
			PushC32((uint32_t)glbl.val);
			break;
		default:
			Verify(false);
		}
	}
}

void JitWriter::SetGlobal(uint32_t idx)
{
	auto &glbl = g_vecglbls.at(idx);

	Verify(glbl.fMutable);
	const char *szCode = nullptr;
	switch (glbl.type)
	{
	case value_type::i32:
		szCode = "\x89\x05";
		break;

	default:
		Verify(false);
	}
	SafePushCode(szCode, strlen(szCode));
	int32_t offset = reinterpret_cast<uint8_t*>(m_pGlobalsStart + idx) - (m_pexecPlaneCur + 4);
	SafePushCode(offset);
	_PopContractStack();
}

void JitWriter::ExtendSigned32_64()
{
	// mov [rdi], eax
	// movsxd rax, dword ptr [rdi]
	static const uint8_t rgcode[] = { 0x89, 0x07, 0x48, 0x63, 0x07 };
	SafePushCode(rgcode);
}

void JitWriter::CompileFn(uint32_t ifn)
{
	size_t cfnImports = 0;
	Verify(ifn >= g_vecimports.size(), "Attempt to compile an import");
	FunctionCodeEntry *pfnc = g_vecfn_code[ifn - g_vecimports.size()].get();
	const uint8_t *pop = pfnc->vecbytecode.data();
	size_t cb = pfnc->vecbytecode.size();
	std::vector<std::pair<value_type, void*>> stackBlockTypeAddr;
	std::vector<std::vector<int32_t*>> stackVecFixupsRelative;
	std::vector<std::vector<void**>> stackVecFixupsAbsolute;

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

	const char *szFnName = nullptr;
	for (size_t iexport = 0; iexport < g_vecexports.size(); ++iexport)
	{
		if (g_vecexports[iexport].kind == external_kind::Function)
		{
			if (g_vecexports[iexport].index == ifn)
			{
				szFnName = g_vecexports[iexport].strName.c_str();
				break;
			}
		}
	}

	if (szFnName != nullptr)
		printf("Function %s (%d):\n", szFnName, ifn);
	else
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
			stackVecFixupsRelative.push_back(std::vector<int32_t*>());
			stackVecFixupsAbsolute.push_back(std::vector<void**>());
			EnterBlock();
			break;
		}

		case opcode::loop:
		{
			value_type type = safe_read_buffer<value_type>(&pop, &cb);
			printf("loop\n");
			stackBlockTypeAddr.push_back(std::make_pair(type, m_pexecPlaneCur));
			stackVecFixupsRelative.push_back(std::vector<int32_t*>());
			stackVecFixupsAbsolute.push_back(std::vector<void**>());
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
			for (uint32_t idepth = 0; idepth < depth; ++idepth)
			{
				LeaveBlock(true);
			}
			LeaveBlock(pairBlock.first != value_type::empty_block);

			int32_t *pdeltaFix = Jump(pairBlock.second);
			if (pairBlock.second == nullptr)
			{
				(stackVecFixupsRelative.rbegin() + depth)->push_back(pdeltaFix);
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
			for (uint32_t idepth = 0; idepth < depth; ++idepth)
			{
				LeaveBlock(true);
			}
			LeaveBlock(pairBlock.first != value_type::empty_block);
			
			int32_t *pdeltaFix = Jump(pairBlock.second);
			if (pairBlock.second == nullptr)
			{
				(stackVecFixupsRelative.rbegin() + depth)->push_back(pdeltaFix);
			}
			*pdeltaNoJmp = m_pexecPlaneCur - (reinterpret_cast<uint8_t*>(pdeltaNoJmp) + sizeof(*pdeltaNoJmp));
			break;
		}
		case opcode::br_table:
		{
			printf("br_table\n");
			BranchTableParse(&pop, &cb, stackBlockTypeAddr, stackVecFixupsRelative, stackVecFixupsAbsolute);
			break;
		}

		case opcode::ret:
		{
			printf("return\n");
			// add rsp, (cblock * 8)
			static const uint8_t rgcode[] = { 0x48, 0x81, 0xC4 };
			int32_t cbSub = stackVecFixupsRelative.size() * 8;
			SafePushCode(rgcode);
			SafePushCode(cbSub);
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
			CallIfn(idx, clocals, ptype->cparams, ptype->fHasReturnValue, false /*fIndirect*/);
			break;
		}
		case opcode::call_indirect:
		{
			printf("call_indirect\n");
			uint32_t idx = safe_read_buffer<varuint32>(&pop, &cb);
			safe_read_buffer<char>(&pop, &cb);	// reserved
			auto ptype = g_vecfn_types.at(idx).get();
			CallIfn(idx, clocals, ptype->cparams, ptype->fHasReturnValue, true /*fIndirect*/);
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


		case opcode::i64_eqz:
			printf("i64.eqz\n");
			Eqz64();
			break;
		case opcode::i64_eq:
			printf("i64.eq\n");
			Compare(CompareType::Equal, false /*fSigned*/, true /*f64*/);
			break;
		case opcode::i64_ne:
			printf("i64.ne\n");
			Compare(CompareType::NotEqual, false /*fSigned*/, true /*f64*/);
			break;
		case opcode::i64_lt_s:
			printf("i64.lt_s\n");
			Compare(CompareType::LessThan, true /*fSigned*/, true /*f64*/);
			break;
		case opcode::i64_lt_u:
			printf("i64.lt_u\n");
			Compare(CompareType::LessThan, false /*fSigned*/, true /*f64*/);
			break;
		case opcode::i64_gt_s:
			printf("i64.gt_s\n");
			Compare(CompareType::GreaterThan, true /*fSigned*/, true /*f64*/);
			break;
		case opcode::i64_gt_u:
			printf("i64.gt_u\n");
			Compare(CompareType::GreaterThan, false /*fSigned*/, true /*f64*/);
			break;
		case opcode::i64_le_s:
			printf("i64.le_s\n");
			Compare(CompareType::LessThanEqual, true /*fSigned*/, true /*f64*/);
			break;
		case opcode::i64_le_u:
			printf("i64.le_u\n");
			Compare(CompareType::LessThanEqual, false /*fSigned*/, true /*f64*/);
			break;
		case opcode::i64_ge_s:
			printf("i64.ge_s\n");
			Compare(CompareType::GreaterThanEqual, true /*fSigned*/, true /*f64*/);
			break;
		case opcode::i64_ge_u:
			printf("i64.ge_u\n");
			Compare(CompareType::GreaterThanEqual, false /*fSigned*/, true /*f64*/);
			break;

		case opcode::i64_load32_u:
		case opcode::i32_load:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
			printf("i32.load $%X\n", offset);
			LoadMem(offset, false, 4, false);
			break;
		}

		case opcode::f64_load:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
			printf("f64.load $%X\n", offset);
			Ud2();
			break;
		}

		case opcode::i64_load8_u:
		case opcode::i32_load8_u:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
			printf("i32_load8_u $%X\n", offset);
			LoadMem(offset, false, 1, false);
			break;
		}
		case opcode::i32_load8_s:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
			printf("i32_load8_s $%X\n", offset);
			LoadMem(offset, false, 1, true);
			break;
		}

		case opcode::i64_load:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
			printf("i64.load $%X\n");
			LoadMem(offset, true, 8, false);
			break;
		}

		case opcode::i64_load8_s:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
			printf("i64.load8_s $%X\n");
			LoadMem(offset, true, 1, true);
			break;
		}

		case opcode::i32_load16_u:
		case opcode::i64_load16_u:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
			printf("i64/32.load16_u $%X\n");
			LoadMem(offset, false, 2, false);
			break;
		}

		case opcode::i64_load16_s:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
			printf("i64.load16_s $%X\n");
			LoadMem(offset, true, 2, true);
			break;
		}

		case opcode::i64_load32_s:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
			printf("i64_load32_s $%X\n", offset);
			LoadMem(offset, true, 8, false);
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
		case opcode::get_global:
		{
			uint32_t iglbl = safe_read_buffer<varuint32>(&pop, &cb);
			printf("get_global $%X\n", iglbl);
			GetGlobal(iglbl);
			break;
		}
		case opcode::set_global:
		{
			uint32_t iglbl = safe_read_buffer<varuint32>(&pop, &cb);
			printf("set_global $%X\n", iglbl);
			SetGlobal(iglbl);
			break;
		}

		case opcode::i64_store32:
		case opcode::i32_store:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
			printf("i32.store $%X\n", offset);
			StoreMem(offset, 4);
			break;
		}

		case opcode::i64_store:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
			printf("i64.store $%X\n", offset);
			StoreMem(offset, 8);
			break;
		}

		case opcode::i32_store16:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
			printf("i32.store16 $%X\n", offset);
			StoreMem(offset, 2);
			break;
		}

		case opcode::i64_store8:
		case opcode::i32_store8:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
			printf("i32.store8 $%X\n", offset);
			StoreMem(offset, 1);
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
		case opcode::i32_div_s:
			printf("i32.div_s\n");
			Ud2();
			break;
		case opcode::i32_div_u:
			printf("i32.div_u\n");
			Ud2();
			break;
		case opcode::i32_rem_s:
			printf("i32.rem_s\n");
			Ud2();
			break;
		case opcode::i32_rem_u:
			printf("i32.rem_u\n");
			Ud2();
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
		case opcode::i64_sub:
			printf("i64.sub\n");
			Sub64();
			break;
		case opcode::i64_mul:
			printf("i64.mul\n");
			Mul64();
			break;

		case opcode::i64_div_s:
			printf("i64.div_s\n");
			Ud2();
			break;
		case opcode::i64_div_u:
			printf("i64.div_u\n");
			Ud2();
			break;
		case opcode::i64_rem_s:
			printf("i64.rem_s\n");
			Ud2();
			break;
		case opcode::i64_and:
			printf("i64.and\n");
			LogicOp64(LogicOperation::And);
			break;
		case opcode::i64_or:
			printf("i64.or\n");
			LogicOp64(LogicOperation::Or);
			break;
		case opcode::i64_xor:
			printf("ix64.xor\n");
			LogicOp64(LogicOperation::Xor);
			break;
		case opcode::i64_shl:
			printf("i64.shl\n");
			LogicOp64(LogicOperation::ShiftLeft);
			break;
		case opcode::i64_shr_u:
			printf("i64.shr_u\n");
			LogicOp64(LogicOperation::ShiftRightUnsigned);
			break;

		case opcode::i32_wrap_i64:
			printf("i32.wrap/i64\n");
			break;	// NOP because accessing eax, even when set as rax has the same behavior

		case opcode::i64_extend_s_i32:
			printf("i64.extend_s_i32\n");
			ExtendSigned32_64();
			break;
		case opcode::i64_extend_u32:
			printf("i64.extend_u/i32\n");
			break;	// NOP because the upper register should be zero regardless

		case opcode::end:
			printf("end\n");
			if (stackBlockTypeAddr.empty())
				break;
			LeaveBlock(stackBlockTypeAddr.back().first != value_type::empty_block);
			// Jump targets are after the LeaveBlock because the branch already performs the work (TODO: Maybe not do that?)
			for (int32_t *poffsetFix : stackVecFixupsRelative.back())
			{
				*poffsetFix = m_pexecPlaneCur - (reinterpret_cast<uint8_t*>(poffsetFix) + sizeof(*poffsetFix));
			}
			for (void **pp : stackVecFixupsAbsolute.back())
			{
				*pp = m_pexecPlaneCur;
			}

			stackBlockTypeAddr.pop_back();
			stackVecFixupsRelative.pop_back();
			stackVecFixupsAbsolute.pop_back();
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

struct ExecutionControlBlock
{
	JitWriter *pjitWriter;
	void *pfnEntry;
	void *operandStack;
	void *localsStack;
	void *memoryBase;

	uint64_t cFnIndirect;
	uint32_t *rgfnIndirect;
	uint32_t *rgFnTypeIndicies;
	uint64_t cFnTypeIndicies;
	void *rgFnPtrs;
	uint64_t cFnPtrs;

	// Values set by the executing code
	void *stackrestore;
	uint64_t retvalue;
};
extern "C" uint64_t ExternCallFnASM(ExecutionControlBlock *pctl);


void JitWriter::ProtectForRuntime()
{
	DWORD dwT;
	Verify(VirtualProtect(m_pexecPlane, (uint8_t*)m_pGlobalsStart - m_pexecPlane, PAGE_READONLY, &dwT));
	Verify(VirtualProtect(m_pcodeStart, m_pexecPlaneCur - m_pcodeStart, PAGE_EXECUTE_READ, &dwT));
}

void JitWriter::UnprotectRuntime()
{
	DWORD dwT;
	Verify(VirtualProtect(m_pexecPlane, (uint8_t*)m_pGlobalsStart - m_pexecPlane, PAGE_READWRITE, &dwT));
	Verify(VirtualProtect(m_pcodeStart, m_pexecPlaneCur - m_pcodeStart, PAGE_READWRITE, &dwT));
}

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
	vecoperand.resize(4096*1000);
	veclocals.resize(4096*1000);
	Verify(pfn != nullptr);
	if (m_pheap == nullptr)
	{
		// Reserve 8GB of memory for our heap plane, this is 2^33 because effective addresses can compute to 33 bits (even though we actually truncate to 32 we reserve the max to prevent security flaws if we truncate incorrectly)
		m_pheap = VirtualAlloc(nullptr, 0x200000000, MEM_RESERVE, PAGE_NOACCESS);
		Verify(VirtualAlloc(m_pheap, 0x100000000, MEM_COMMIT, PAGE_READWRITE) != nullptr);
		Verify(g_vecmem.size() < 0x100000000);
		memcpy(m_pheap, g_vecmem.data(), g_vecmem.size());
	}

	ExecutionControlBlock ectl;
	ectl.pjitWriter = this;
	ectl.pfnEntry = pfn;
	ectl.operandStack = vecoperand.data();
	ectl.localsStack = veclocals.data();
	ectl.memoryBase = m_pheap;
	ectl.cFnIndirect = g_vecIndirectFnTable.size();
	ectl.rgfnIndirect = g_vecIndirectFnTable.data();
	ectl.rgFnTypeIndicies = g_vecfn_entries.data();
	ectl.cFnTypeIndicies = g_vecfn_entries.size();
	ectl.rgFnPtrs = (void*)m_pexecPlane;
	ectl.cFnPtrs = m_cfn;
	
	ProtectForRuntime();
	retV = ExternCallFnASM(&ectl);
	UnprotectRuntime();
}

extern "C" void CompileFn(ExecutionControlBlock *pectl, uint32_t ifn)
{
	pectl->pjitWriter->UnprotectRuntime();
	pectl->pjitWriter->CompileFn(ifn);
	pectl->pjitWriter->ProtectForRuntime();
}