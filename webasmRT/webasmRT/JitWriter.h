#pragma once

class JitWriter
{
public:
	JitWriter(uint8_t *pexecPlane, size_t cbExec, size_t cfn)
		: m_pexecPlane(pexecPlane), m_pexecPlaneCur(pexecPlane), m_pexecPlaneMax(pexecPlane + cbExec), m_cfn(cfn)
	{
		m_pexecPlaneCur += sizeof(void*) * cfn;	// allocate the function table, ensuring its within 32-bits of all our code
	}

	void CompileFn(uint32_t ifn);

	void ExternCallFn(uint32_t ifn, void *pvAddrMem);
private:
	void SafePushCode(const void *pv, size_t cb);
	template<typename T, size_t size>
	size_t GetArrLength(T(&)[size]) { return size; }

	template<typename T>
	void SafePushCode(T &rgcode, std::true_type) 
	{ 
		SafePushCode(rgcode, GetArrLength(rgcode) * sizeof(*rgcode));
	}

	template<typename T>
	void SafePushCode(T &val, std::false_type)
	{
		SafePushCode(&val, sizeof(val));
	}

	template<typename T>
	void SafePushCode(T &val)
	{
		SafePushCode<T>(val, std::is_array<T>());
	}

	enum class CompareType
	{
		LessThan,
		LessThanEqual,
		Equal,
		NotEqual,
		GreaterThanEqual,
		GreaterThan,
	};
	enum class LogicOperation
	{
		And,
		Or,
		Xor,
		ShiftLeft,
		ShiftRight,
		ShiftRightUnsigned,
	};

	uint32_t RelAddrPfnVector(uint32_t ifn, uint32_t opSize) const
	{
		return (m_pexecPlane + (sizeof(void*)*ifn)) - (m_pexecPlaneCur + opSize);
	}

	// Common code sequences (does not leave machine in valid state)
	void _PushExpandStack();
	void _PopContractStack();
	void _PopSecondParam(bool fSwapParams = false);
	void _LoadMem32(uint32_t offset);
	void _SetMem32(uint32_t offset);
	void _SetDbgReg(uint32_t opcode);

	// common operations (does leave machine in valid state)
	void LoadMem32(uint32_t offset);
	void LoadMem64(uint32_t offset);
	void Load64Mem32(uint32_t offset, bool fSigned);
	void LoadMem8(uint32_t offset, bool fSigned);
	void StoreMem32(uint32_t offset);
	void StoreMem64(uint32_t offset);
	void StoreMem8(uint32_t offset);
	void Sub32();
	void Add32();
	void Mul32();
	void Add64();
	void Div64();
	void Popcnt32();
	void PushC32(uint32_t c);
	void PushC64(uint64_t c);
	void SetLocal(uint32_t idx, bool fPop);
	void GetLocal(uint32_t idx);
	void Select();
	void Compare(CompareType type, bool fSigned, bool f64);
	void Eqz32();
	void LogicOp(LogicOperation op);
	void LogicOp64(LogicOperation op);
	int32_t *JumpNIf(void *addr);	// returns a pointer to the offset encoded in the instruction for later adjustment
	int32_t *Jump(void *addr);
	void CallIfn(uint32_t ifn, uint32_t clocalsCaller, uint32_t cargsCallee, bool fReturnValue);
	void FnEpilogue();
	void FnPrologue(uint32_t clocals, uint32_t cargs);

	void EnterBlock();
	void LeaveBlock(bool fHasReturn);

	uint8_t *m_pexecPlane;
	uint8_t *m_pexecPlaneCur;
	uint8_t *m_pexecPlaneMax;
	size_t m_cfn;
};