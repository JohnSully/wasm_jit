// webasmRT.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <inttypes.h>
#include "wasm_types.h"
#include "Exceptions.h"
#include "safe_access.h"
#include "JitWriter.h"
#include "ExpressionService.h"

std::vector<FunctionTypeEntry::unique_pfne_ptr> g_vecfn_types;
std::vector<uint32_t> g_vecfn_entries;
std::vector<table_type> g_vectbl;
std::vector<resizable_limits> g_vecmem_types;
std::vector<int> g_vecimports;
std::vector<export_entry> g_vecexports;
std::vector<FunctionCodeEntry::unique_pfne_ptr> g_vecfn_code;
std::vector<uint8_t> g_vecmem;

void load_fn_type(const uint8_t **prgbPayload, size_t *pcbData)
{
	value_type form = safe_read_buffer<value_type>(prgbPayload, pcbData);
	Verify(form == value_type::func);

	varuint32 paramCount = safe_read_buffer<varuint32>(prgbPayload, pcbData);
	
	auto spfne = FunctionTypeEntry::CreateFunctionEntry(paramCount);
	
	for (uint32_t iparam = 0; iparam < paramCount; ++iparam)
	{
		spfne->rgparam_type[iparam] = safe_read_buffer<value_type>(prgbPayload, pcbData);
	}

	spfne->fHasReturnValue = safe_read_buffer<uint8_t>(prgbPayload, pcbData) == 1;
	if (spfne->fHasReturnValue)
		spfne->return_type = safe_read_buffer<value_type>(prgbPayload, pcbData);

	g_vecfn_types.emplace_back(std::move(spfne));
}

void load_fn_types(const uint8_t *rgbPayload, size_t cbData)
{
	varuint32 var32cfn = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
	uint32_t cfn = var32cfn;

	while (cfn > 0)
	{
		load_fn_type(&rgbPayload, &cbData);
		cfn--;
	}
	Verify(cbData == 0);
}

void load_fn_decls(const uint8_t *rgbPayload, size_t cbData)
{
	varuint32 var32cfn = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
	uint32_t cfn = var32cfn;
	g_vecfn_entries.reserve(g_vecfn_entries.size() + cfn);
	while (cfn > 0)
	{
		g_vecfn_entries.push_back(safe_read_buffer<varuint32>(&rgbPayload, &cbData));
		Verify(g_vecfn_entries.back() < g_vecfn_types.size());
		--cfn;
	}
	Verify(cbData == 0);
}

resizable_limits load_resizeable_limits(const uint8_t **prgbPayload, size_t *pcbData)
{
	resizable_limits limits;
	limits.fMaxSet = !!(safe_read_buffer<uint8_t>(prgbPayload, pcbData) & 1);
	limits.initial_size = safe_read_buffer<varuint32>(prgbPayload, pcbData);
	if (limits.fMaxSet)
		limits.maximum_size = safe_read_buffer<varuint32>(prgbPayload, pcbData);
	return limits;
}

local_entry load_local_entry(const uint8_t **prgbPayload, size_t *pcbData)
{
	local_entry le;
	le.count = safe_read_buffer<varuint32>(prgbPayload, pcbData);
	le.type = safe_read_buffer<value_type>(prgbPayload, pcbData);
	return le;
}

void load_tables(const uint8_t *rgbPayload, size_t cbData)
{
	varuint32 var32ctbl = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
	uint32_t ctbl = var32ctbl;
	g_vectbl.reserve(ctbl);
	while (ctbl > 0)
	{
		table_type tbl;
		tbl.elem_type = safe_read_buffer<elem_type>(&rgbPayload, &cbData);
		tbl.limits = load_resizeable_limits(&rgbPayload, &cbData);
		--ctbl;
	}
	Verify(cbData == 0);
}

void load_memory(const uint8_t *rgbPayload, size_t cbData)
{
	varuint32 var32cmemt = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
	uint32_t cmemt = var32cmemt;
	g_vecmem_types.reserve(cmemt);
	while (cmemt > 0)
	{
		g_vecmem_types.emplace_back(load_resizeable_limits(&rgbPayload, &cbData));
		--cmemt;
	}
	Verify(cbData == 0);
}

void load_exports(const uint8_t *rgbPayload, size_t cbData)
{
	varuint32 var32cexp = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
	uint32_t cexp = var32cexp;
	g_vecexports.reserve(cexp);
	
	while (cexp > 0)
	{
		export_entry entry;
		entry.strName = safe_read_buffer<std::string>(&rgbPayload, &cbData);
		entry.kind = safe_read_buffer<external_kind>(&rgbPayload, &cbData);
		entry.index = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
		g_vecexports.emplace_back(std::move(entry));
		--cexp;
	}
	Verify(cbData == 0);
}

void load_code(const uint8_t *rgbPayload, size_t cbData)
{
	varuint32 var32cfn = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
	uint32_t cfn = var32cfn;
	
	while (cfn > 0)
	{
		size_t cbBody = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
		Verify(cbBody <= cbData);
		cbData -= cbBody;
		varuint32 clocal = safe_read_buffer<varuint32>(&rgbPayload, &cbBody);
		auto spfnce = FunctionCodeEntry::CreateFunctionCodeEntry(clocal);
		for (size_t ilocal = 0; ilocal < clocal; ++ilocal)
		{
			spfnce->rglocals[ilocal] = load_local_entry(&rgbPayload, &cbBody);
		}
		spfnce->vecbytecode.resize(cbBody);
		safe_copy_buffer(spfnce->vecbytecode.data(), cbBody, &rgbPayload, &cbBody);
		Verify((opcode)spfnce->vecbytecode.back() == opcode::end);
		
		g_vecfn_code.emplace_back(std::move(spfnce));
		--cfn;
	}
	Verify(cbData == 0);
}

void load_imports(const uint8_t *rgbPayload, size_t cbData)
{
	varuint32 var32cimport = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
	uint32_t cimport = var32cimport;

	while (cimport > 0)
	{
		uint32_t module_len = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
		std::vector<char> vecrgchModule;
		vecrgchModule.resize(module_len);
		safe_copy_buffer(vecrgchModule.data(), module_len, &rgbPayload, &cbData);
		uint32_t field_len = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
		std::vector<char> vecrgchField;
		vecrgchField.resize(field_len);
		safe_copy_buffer(vecrgchField.data(), field_len, &rgbPayload, &cbData);
		external_kind kind = safe_read_buffer<external_kind>(&rgbPayload, &cbData);
		g_vecimports.push_back(0);	// for now just place hold

		switch (kind)
		{
		case external_kind::Function:
		{
			uint32_t ifnType = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
			g_vecfn_entries.push_back(ifnType);
			Verify(g_vecfn_entries.back() < g_vecfn_types.size());
			break;
		}
		default:
			Verify(false);
		}
		--cimport;
	}
}

// Note: we don't actually save this yet
// initializers for indirect function table
void load_elements(const uint8_t *rgbPayload, size_t cbData)
{
	varuint32 var32celem = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
	uint32_t celem = var32celem;

	while (celem > 0)
	{
		uint32_t idx = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
		Verify(idx == 0);	// MVP limitation
		// LoadExpression
		ExpressionService exprsvc;
		ExpressionService::Variant var;
		size_t cbExpr = exprsvc.CbEatExpression(rgbPayload, cbData, &var);
		Verify(cbExpr <= cbData);	// This would be a bug in CbEatExpr but lets double check
		cbData -= cbExpr;
		rgbPayload += cbExpr;
		uint32_t numelem = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
		for (uint32_t ielem = 0; ielem < numelem; ++ielem)
		{
			safe_read_buffer<varuint32>(&rgbPayload, &cbData);
		}

		--celem;
	}
	Verify(cbData == 0);
}

void load_data(const uint8_t *rgbPayload, size_t cbData)
{
	uint32_t csegs = safe_read_buffer<varuint32>(&rgbPayload, &cbData);

	ExpressionService exprsvc;
	while (csegs > 0)
	{
		uint32_t idxMem = safe_read_buffer<varuint32>(&rgbPayload, &cbData);
		Verify(idxMem == 0);	// MVP limitation

		ExpressionService::Variant varOffset;
		size_t cbExpr = exprsvc.CbEatExpression(rgbPayload, cbData, &varOffset);
		Verify(cbExpr <= cbData);
		rgbPayload += cbExpr;
		cbData -= cbExpr;

		uint32_t offset = varOffset.val;
		uint32_t cb = safe_read_buffer<varuint32>(&rgbPayload, &cbData);

		// Double verify to look for wrapping
		Verify(offset <= g_vecmem.size());
		Verify((offset + cb) <= g_vecmem.size());

		safe_copy_buffer(g_vecmem.data() + offset, cb, &rgbPayload, &cbData);

		--csegs;
	}
}

void InitializeMemory()
{
	// find out memory export
	int idxMem = -1;
	for (auto &exp : g_vecexports)
	{
		if (exp.strName == "memory")
		{
			Verify(idxMem == -1, "Only one memory export may be defined");
			idxMem = exp.index;
		}
	}

	Verify(idxMem >= 0 && idxMem < g_vecmem_types.size(), "Invalid memory export");
	g_vecmem.resize(g_vecmem_types[idxMem].initial_size * WASM_PAGE_SIZE);
}


bool load_section(FILE *pf)
{
	section_header header;
	std::vector<uint8_t> vecrgchName;
	std::vector<uint8_t> vecpayload;
	try
	{
		fread_struct(&header.id, pf);
		fread_struct(&header.payload_len, pf);
		if ((int)header.id == 0)
		{
			varuint32 name_len;
			fread_struct(&name_len, pf);
			vecrgchName.resize(name_len);
			fread_struct(vecrgchName.data(), pf, name_len);
		}
		vecpayload.resize(header.payload_len);
		fread_struct(vecpayload.data(), pf, header.payload_len);
	}
	catch (int)
	{
		if (feof(pf))
			return false;	// valid to end the file at a section boundary
		throw;
	}

	switch (header.id)
	{
	case section_types::Custom:
		break;	//ignore custom sections
	case section_types::Type:
		load_fn_types(vecpayload.data(), vecpayload.size());
		break;
	case section_types::Import:
		load_imports(vecpayload.data(), vecpayload.size());
		break;
	case section_types::Function:
		load_fn_decls(vecpayload.data(), vecpayload.size());
		break;
	case section_types::Table:
		load_tables(vecpayload.data(), vecpayload.size());
		break;
	case section_types::Memory:
		load_memory(vecpayload.data(), vecpayload.size());
		break;
	case section_types::Export:
		load_exports(vecpayload.data(), vecpayload.size());
		InitializeMemory();
		break;
	case section_types::Element:
		load_elements(vecpayload.data(), vecpayload.size());
		break;
	case section_types::Code:
		load_code(vecpayload.data(), vecpayload.size());
		break;
	case section_types::Data:
		load_data(vecpayload.data(), vecpayload.size());
		break;

	default:
		throw std::string("unknown section");
	}
	return true;
}

uint64_t ExecuteFunction(uint32_t ifn, uint64_t *args = nullptr)
{
	FunctionCodeEntry *pfnc = g_vecfn_code[ifn].get();
	const uint8_t *pop = pfnc->vecbytecode.data();
	size_t cb = pfnc->vecbytecode.size();
	std::vector<uint64_t> paramStack;
	std::vector<uint64_t> locals;
	std::stack<std::tuple<block_type, size_t>> stackBlock;
	uint64_t ret = 0;

	size_t cparams = g_vecfn_types[ifn]->cparams;
	for (size_t iparam = 0; iparam < cparams; ++iparam)
	{
		locals.push_back(args[iparam]);
	}
	for (size_t ilocal = 0; ilocal < pfnc->clocalVars; ++ilocal)
	{
		locals.resize(locals.size() + pfnc->rglocals[ilocal].count);
	}

	while (cb > 0)
	{
		cb--;	// count *pop
		++pop;
		switch ((opcode)*(pop-1))
		{
		case opcode::block:
		{
			uint8_t type = safe_read_buffer<block_type>(&pop, &cb);
			stackBlock.push(std::make_tuple(type, locals.size()));
			break;
		}
		case opcode::br_if:
		{
			uint32_t depth = safe_read_buffer<varuint32>(&pop, &cb);
			if (paramStack.back())
			{
				size_t stackSize = 0;
				do
				{
					stackSize = std::get<1>(stackBlock.top());
					stackBlock.pop();
					if (depth > 0)
						--depth;
				} while (depth > 0);
				Verify(false);
			}
			paramStack.erase(paramStack.begin() + (paramStack.size() - 1));
			break;
		}

		case opcode::call:
		{
			uint32_t idx = safe_read_buffer<varuint32>(&pop, &cb);
			auto ptype = g_vecfn_types.at(idx).get();
			uint64_t ret = ExecuteFunction(idx, paramStack.data() + (paramStack.size() - ptype->cparams));
			if (ptype->fHasReturnValue)
				paramStack.push_back(ret);
			break;
		}

		case opcode::get_local:
		{
			uint32_t idx = safe_read_buffer<varuint32>(&pop, &cb);
			paramStack.push_back(locals.at(idx));
			break;
		}
		case opcode::tee_local:
		{
			uint32_t idx = safe_read_buffer<varuint32>(&pop, &cb);
			locals.at(idx) = paramStack.back();
			break;
		}

		case opcode::grow_memory:
		{
			// skip the reserved byte
			uint8_t growsize = safe_read_buffer<uint8_t>(&pop, &cb);
			size_t cb = g_vecmem.size() / WASM_PAGE_SIZE;
			g_vecmem.resize(g_vecmem.size() + (growsize * WASM_PAGE_SIZE));
			paramStack.push_back(cb);
			break;
		}

		case opcode::i32_const:
			paramStack.push_back(safe_read_buffer<varint32>(&pop, &cb));
			break;

		case opcode::i32_ge_s:	// >= (signed)
		{
			int32_t a = (int32_t)paramStack.back();
			paramStack.erase(paramStack.begin() + (paramStack.size() - 1));
			int32_t b = (int32_t)paramStack.back();
			paramStack.erase(paramStack.begin() + (paramStack.size() - 1));
			paramStack.push_back(a >= b);
			break;
		}

		case opcode::i32_load:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
			paramStack.push_back(*reinterpret_cast<uint32_t*>(g_vecmem.data() + offset));
			break;
		}

		case opcode::i64_load32_s:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
			paramStack.push_back(static_cast<int64_t>(*reinterpret_cast<int32_t*>(g_vecmem.data() + offset)));
			break;
		}

		case opcode::i32_store:
		{
			uint32_t align = safe_read_buffer<varuint32>(&pop, &cb);	// NYI alignment
			uint32_t offset = safe_read_buffer<varuint32>(&pop, &cb);
			int32_t val = (int32_t)paramStack.back();
			paramStack.erase(paramStack.begin() + (paramStack.size() - 1));
			*reinterpret_cast<uint32_t*>(g_vecmem.data() + offset) = val;
			break;
		}
		
		case opcode::i32_sub:
		{
			int32_t a = (int32_t)paramStack.back();
			paramStack.erase(paramStack.begin() + (paramStack.size() - 1));
			int32_t b = (int32_t)paramStack.back();
			paramStack.erase(paramStack.begin() + (paramStack.size() - 1));
			paramStack.push_back(a - b);
			break;
		}

		case opcode::end:
		{
			if (cb == 0)	// end of function pop's the return value
			{
				printf("return value: %ull\n", paramStack.back());
				paramStack.erase(paramStack.begin() + (paramStack.size() - 1));
			}
			break;
		}
		default:
			throw RuntimeException("Invalid opcode");
		}
	}
	return ret;
}


void CallFunction(const char *szName, JitWriter &writer)
{
	bool fExecuted = false;
	for (size_t iexport = 0; iexport < g_vecexports.size(); ++iexport)
	{
		if (g_vecexports[iexport].strName == szName)
		{
			//ExecuteFunction(g_vecexports[ifn].index);
			size_t ifn = g_vecexports[iexport].index;
			writer.ExternCallFn(ifn, g_vecmem.data());
			fExecuted = true;
			break;
		}
	} 
	Verify(fExecuted);
}

#include <Windows.h>
int main(int argc, char *argv[])
{
	if (argc < 2)
		return 0;
	FILE *pf = fopen(argv[1], "rb");
	if (pf == nullptr)
		return 0;

	wasm_file_header header;
	fread_struct(&header, pf);

	if (header.magic != 0x6d736100U)
	{
		perror("Invalid webasm magic");
		return 0;
	}
	if (header.version != 1)
	{
		perror("unknown version");
		return 0;
	}
	while (load_section(pf));
	Verify(feof(pf));
	fclose(pf);

	size_t cbExecPlane = 128 * 4096;
	uint8_t *rgexec = (uint8_t*)VirtualAlloc(nullptr, cbExecPlane, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	memset(rgexec, 0, cbExecPlane);
	JitWriter writer(rgexec, cbExecPlane, g_vecfn_entries.size());

	CallFunction("main", writer);
    return 0;
}

