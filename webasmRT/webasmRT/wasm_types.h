#pragma once

#pragma pack(push, 1)

static const uint32_t WASM_PAGE_SIZE = 64 * 1024;

template<typename T>
struct varuintT
{
	varuintT() = default;
	varuintT(T val)
		: val(val)
	{}

	operator T() const
	{
		return val;
	}

	varuintT operator|=(T other)
	{
		val |= other;
		return *this;
	}

	T val;
};

using varuint32 = varuintT<uint32_t>;
using varuint64 = varuintT<uint64_t>;

struct varint32
{
	varint32() = default;
	varint32(int32_t val)
		: val(val)
	{}

	operator int32_t() const
	{
		return val;
	}

	varint32 operator|=(int32_t other)
	{
		val |= other;
		return *this;
	}

	int32_t val;
};

typedef uint8_t block_type;

struct wasm_file_header
{
	uint32_t magic;
	uint32_t version;
};

enum class value_type : uint8_t
{
	i32 = 0x7f,
	i64 = 0x7e,
	f32 = 0x7d,
	f64 = 0x7c,
	anyfunc = 0x70,
	func = 0x60,
	empty_block = 0x40
};

enum class section_types : uint8_t
{
	Custom = 0,
	Type = 1,
	Import = 2,
	Function = 3,
	Table = 4,
	Memory = 5,
	Global = 6,
	Export = 7,
	Start = 8,
	Element = 9,
	Code = 10,
	Data = 11,
};

enum class external_kind : uint8_t
{
	Function = 0,
	Table = 1,
	Memory = 2,
	Global = 3,
};

enum class elem_type : uint8_t
{
	anyfunc = 0x70,
};

// DO NOT ADD opcodes until they are implemented
enum class opcode : uint8_t
{
	unreachable = 0x00,

	block = 0x02,
	loop = 0x03,

	br = 0x0c,
	br_if = 0x0d,
	br_table = 0x0e,

	ret = 0x0f,
	call = 0x10,
	call_indirect = 0x11,

	drop = 0x1a,
	select = 0x1b,

	get_local = 0x20,
	set_local = 0x21,
	tee_local = 0x22,
	get_global = 0x23,

	i32_load =     0x28,
	i32_load8_u =  0x2d,
	i32_load8_s =  0x2c,
	i64_load =	   0x29,
	i64_load32_s = 0x34,

	i32_store = 0x36,
	i64_store = 0x37,
	i32_store8 = 0x3a,

	grow_memory = 0x40,

	i32_const = 0x41,
	i64_const = 0x42,

	i32_eqz =  0x45,
	i32_eq =   0x46,
	i32_ne =   0x47,
	i32_lt_s = 0x48,
	i32_lt_u = 0x49,
	i32_gt_s = 0x4a,
	i32_gt_u = 0x4b,
	i32_le_s = 0x4c,
	i32_le_u = 0x4d,
	i32_ge_s = 0x4e,	// signed >=
	i32_ge_u = 0x4f,

	i64_ne = 0x52,

	i32_popcnt = 0x69,
	i32_add = 0x6a,
	i32_sub = 0x6b,
	i32_mul = 0x6c,
	i32_and = 0x71,
	i32_or = 0x72,
	i32_xor = 0x73,
	i32_shl = 0x74,
	i32_shr_s = 0x75,
	i32_shr_u = 0x76,

	i64_add = 0x7c,
	i64_div_u = 0x80,
	i64_and = 0x83,
	i64_or = 0x84,
	i64_shl = 0x86,

	i32_wrap_i64 = 0xa7,
	i64_extend_u32 = 0xad,

	end = 0x0b,
};


struct section_header
{
	section_types id;
	varuint32 payload_len;
};
static_assert(sizeof(section_header) == 5, "Incorrect packing");

struct resizable_limits
{
	bool fMaxSet;
	uint32_t initial_size;
	uint32_t maximum_size;
};

struct table_type
{
	elem_type elem_type;
	resizable_limits limits;
};

struct export_entry
{
	std::string strName;
	external_kind kind;
	uint32_t index;
};

struct local_entry
{
	uint32_t count;
	value_type type;
};



template<typename T>
struct free_delete
{
	void operator()(T* pT)
	{
		pT->~T();
		free(pT);
	}
};

struct FunctionTypeEntry
{
private:
	FunctionTypeEntry() = default;

public:
	using unique_pfne_ptr = std::unique_ptr <FunctionTypeEntry, free_delete<FunctionTypeEntry>>;
	static unique_pfne_ptr CreateFunctionEntry(uint32_t cparams)
	{
		FunctionTypeEntry *pfne = (FunctionTypeEntry*)malloc(sizeof(FunctionTypeEntry) + (sizeof(value_type) * cparams));	// this will allocate 1 extra value_type... who cares
		pfne->cparams = cparams;
		return unique_pfne_ptr(pfne);
	}

	bool fHasReturnValue;
	value_type return_type;
	uint32_t cparams;
	value_type rgparam_type[1];
};

struct FunctionCodeEntry
{
private:
	FunctionCodeEntry() = default;

public:
	using unique_pfne_ptr = std::unique_ptr<FunctionCodeEntry, free_delete<FunctionCodeEntry>>;
	static unique_pfne_ptr CreateFunctionCodeEntry(uint32_t clocals)
	{
		FunctionCodeEntry *pfnce = (FunctionCodeEntry*)malloc(sizeof(FunctionCodeEntry) + (sizeof(local_entry) * clocals));		// this will allocate 1 extra local_entry... who cares
		new (pfnce) FunctionCodeEntry();	// ensure the vec is initialized
		pfnce->clocalVars = clocals;
		return unique_pfne_ptr(pfnce);
	}

	uint32_t clocalVars;
	std::vector<uint8_t> vecbytecode;
	local_entry rglocals[1];
};

#pragma pack(pop)