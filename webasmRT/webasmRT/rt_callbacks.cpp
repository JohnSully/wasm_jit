#include "stdafx.h"
#include "Exceptions.h"
#include "BuiltinFunctions.h"
#include <io.h>

extern std::vector<int> g_vecimports;

bool FEqualProto(const BuiltinExport &builtinexport, const FunctionTypeEntry &fnt)
{
	// check return
	switch (builtinexport.retT)
	{
	case value_type::none:
		if (fnt.fHasReturnValue)
			return false;
		break;

	default:
		if (builtinexport.retT != fnt.return_type)
			return false;
	}

	auto itrParamT = builtinexport.vecarg.begin();
	for (size_t iparam = 0; iparam < fnt.cparams; ++iparam)
	{
		if (itrParamT == builtinexport.vecarg.end())
			return false;
		if (*itrParamT != fnt.rgparam_type[iparam])
			return false;
		++itrParamT;
	}
	return (itrParamT == builtinexport.vecarg.end());
}

uint64_t wasm_close_fd(uint64_t *pvArgs, uint8_t *pvMemBase)
{
	return 0;
}

// size_t wasm_write_fd(int fd, void *pv, size_t cb)
uint64_t wasm_write_fd(uint64_t *rgArgs, uint8_t *pvMemBase)
{
	int fd = (int)rgArgs[0];
	const uint8_t *pv = pvMemBase + rgArgs[1];
	size_t cb = rgArgs[2];

	return _write(fd, pv, cb);
}

// long long wasm_llseek_fd(int fd, long long offset, int whence)
uint64_t wasm_llseek_fd(uint64_t *pvArgs, uint8_t *pvMemBase)
{
	return 0;
}

BuiltinExport BuiltinMap[] = {
	{ "wasm_close_fd", value_type::i32, { value_type::i32 }, wasm_close_fd },
	{ "wasm_write_fd", value_type::i32, { value_type::i32, value_type::i32, value_type::i32}, wasm_write_fd },
	{ "wasm_llseek_fd", value_type::i32, { value_type::i32, value_type::i64, value_type::i32 }, wasm_llseek_fd }
};

int IBuiltinFromName(const std::string &strName)
{
	for (int ifn = 0; ifn < _countof(BuiltinMap); ++ifn)
	{
		if (strName == BuiltinMap[ifn].szName)
			return ifn;
	}
	Verify(false);
}

extern "C" uint64_t CReentryFn(int ifn, uint64_t *pvArgs, uint8_t *pvMemBase)
{
	Verify(ifn >= 0 && ifn < g_vecimports.size());
	ifn = g_vecimports[ifn];
	Verify(ifn >= 0 && ifn < _countof(BuiltinMap));
	BuiltinMap[ifn].pfn(pvArgs, pvMemBase);
	return 0;
}