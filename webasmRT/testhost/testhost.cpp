#include "../webasmRT/stdafx.h"
#include "../webasmRT/WasmContext.h"
#include "../webasmRT/ExpressionService.h"
#include <assert.h>
#ifdef _MSC_VER
#include 	<process.h>
#define NOMINMAX
#include <Windows.h>
#endif
#include <algorithm>

enum class ParseMode
{
	Whitespace,
	Comment,
	Quote,
	Escape,
	Command,
};

std::unique_ptr<WasmContext> g_spctxtLast;
ExpressionService::Variant g_variantLastExec;
ExpressionService::Variant g_variantExpectedReturn;

int RunProgram(const char *szProgram, const char *szArgs)
{
	SHELLEXECUTEINFOA shellexeca = { 0 };
	shellexeca.cbSize = sizeof(SHELLEXECUTEINFOA);
	shellexeca.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOCLOSEPROCESS;
	shellexeca.lpVerb = "open";
	shellexeca.lpFile = szProgram;
	shellexeca.lpParameters = szArgs;
	shellexeca.nShow = SW_SHOW;

	ShellExecuteExA(&shellexeca);
	assert(shellexeca.hProcess != nullptr);
	WaitForSingleObject(shellexeca.hProcess, 10000);
	DWORD retV;
	GetExitCodeProcess(shellexeca.hProcess, &retV);
	return (int)retV;
}

// cch == 0 means no expression
size_t CchTryParseExpression(const char *rgchExpr, size_t cchExpr, ExpressionService::Variant *pvarOut)
{
	size_t ichCur = 0;
	while (ichCur < cchExpr)
	{
		switch (rgchExpr[ichCur])
		{
		case ')':
			return 0;
		}
		++ichCur;
	}
}

void ProcessInvoke(const char *rgch, size_t cch)
{
	size_t ichFnStart = 0;
	assert(cch > 0);
	while (rgch[ichFnStart] != '"')
	{
		assert((ichFnStart + 1) < cch);
		++ichFnStart;
	}
	++ichFnStart;	// after the quote
	size_t ichFnEnd = ichFnStart + 1;
	assert(ichFnEnd < cch);
	while (rgch[ichFnEnd] != '"')
	{
		assert((ichFnEnd + 1) < cch);
		++ichFnEnd;
	}

	std::string strFnExec(rgch + ichFnStart, rgch + ichFnEnd);

	// Gather Arguments
	std::vector<ExpressionService::Variant> vecargs;
	size_t ichArgStart = ichFnEnd + 1;
	while (ichArgStart < cch)
	{
		if (isspace(rgch[ichArgStart]))
		{
			++ichArgStart;
			continue;
		}
		if (rgch[ichArgStart] == '(')
		{
			vecargs.push_back(ExpressionService::Variant());
			ichArgStart += ExpressionService::CchEatExpression(rgch + ichArgStart, cch - ichArgStart, &vecargs.back());
			continue;
		}
		else
		{
			Verify(rgch[ichArgStart] == ')');
			break;
		}
		Verify(false);	// should never get here
	}

	printf("Invoke: %s\n", strFnExec.c_str());
	g_variantLastExec = g_spctxtLast->CallFunction(strFnExec.c_str(), vecargs.data(), numeric_cast<uint32_t>(vecargs.size()));
}

void ProcessCommand(const std::string &str, FILE *pf, off_t offsetStart, off_t offsetEnd)
{
	char rgchT[1024];
	off_t offsetCur = ftell(pf);
	fseek(pf, offsetStart, SEEK_SET);

	if (str == "module")
	{
		// compile the module and set the current WasmContext

		char szPathWast[MAX_PATH];
		char szPathWasm[MAX_PATH];
		
		GetTempPathA(MAX_PATH, szPathWast);
		strcpy(szPathWasm, szPathWast);
		strcat_s(szPathWast, "temp.wast");
		strcat_s(szPathWasm, "temp.wasm");

		// copy the module portion
		size_t cbWast = offsetEnd - offsetStart;
		FILE *pfWast = fopen(szPathWast, "w+");
		if (pfWast == nullptr)
			throw "failed to open temp file";

		for (size_t cbWritten = 0; cbWritten < cbWast; )
		{
			size_t cb = std::min<size_t>(1024, cbWast - cbWritten);
			size_t cbT = fread(rgchT, 1, cb, pf);
			if (cbT == 0)
				throw "failed to read wast";
			size_t cbWrote = fwrite(rgchT, 1, cbT, pfWast);
			assert(cbWrote == cbT);
			cbWritten += cbT;
		}
		fclose(pfWast);

		// compile the module portion
		char szParams[1024] = { '\0' };
		strcat_s(szParams, szPathWast);
		strcat_s(szParams, " --no-check -o ");
		strcat_s(szParams, szPathWasm);
		int res = RunProgram("wat2wasm", szParams);
		Verify(res == EXIT_SUCCESS, "Failed to compile test module");

		g_spctxtLast = std::make_unique<WasmContext>();
		FILE *pfWasm = fopen(szPathWasm, "rb");
		g_spctxtLast->LoadModule(pfWasm);
		fclose(pfWasm);
	}
	else if (str == "invoke")
	{
		std::vector<char> vecch;
		vecch.resize(offsetEnd - offsetStart);
		fread(vecch.data(), 1, vecch.size(), pf);
		ProcessInvoke(vecch.data(), vecch.size());
		g_variantExpectedReturn.type = value_type::none;
		g_variantExpectedReturn.val = 0;
	}
	else if (str == "assert_return")
	{
		Verify(g_variantExpectedReturn == g_variantLastExec);
	}
	else if (str == "assert_trap")
	{
		//assert(false);
	}
	else if (str == "assert_invalid" || str == "assert_malformed")
	{
		//Unsupported tests
	}
	else
	{
		// If its not a verb we expect we are assuming its a return value expression
		std::vector<char> vecch;
		vecch.resize(offsetEnd - offsetStart);
		fread(vecch.data(), 1, vecch.size(), pf);
		ExpressionService::CchEatExpression(vecch.data(), vecch.size(), &g_variantExpectedReturn);
	}

	fseek(pf, offsetCur, SEEK_SET);
}

int main(int argc, char *argv[])
{
	if (argc != 2)
	{
		fprintf(stderr, "Expected test file.\n");
		return EXIT_FAILURE;
	}
	FILE *pf = fopen(argv[1], "rb");
	if (pf == nullptr)
	{
		fprintf(stderr, "Failed to open test file.\n");
		return EXIT_FAILURE;
	}

	char rgch[1024];
	size_t cch;
	std::stack<ParseMode> stackMode;
	stackMode.push(ParseMode::Whitespace);
	bool fEscapeLast = false;
	int cblock = 0;
	std::stack<off_t> stackoffsetBlockStart;
	bool fNestCmd = false;

	std::stack<std::string> stackstrCmd;
	while ((cch = fread(rgch, 1, 1024, pf)) > 0)
	{
		const char *pch = rgch;
		const char *pchMax = rgch + cch;

		while (pch < pchMax)
		{
			ParseMode mode = stackMode.top();

			if (mode == ParseMode::Command)
			{
				if ((*pch >= 'a' && *pch <= 'z') || (*pch >= 'A' && *pch <= 'Z') || (*pch >= '0' && *pch <= '9') || *pch == '_')
				{
					stackstrCmd.top().append(pch, pch + 1);
				}
				else
				{
					fNestCmd = stackstrCmd.top() != "module" && stackstrCmd.top() != "assert_invalid" && stackstrCmd.top() != "assert_malformed" && stackstrCmd.top() != "assert_trap";
					stackMode.pop();
					mode = stackMode.top();
				}
			}
			
			if (mode == ParseMode::Quote)
			{
				if (*pch == '"' && !fEscapeLast)
					stackMode.pop();
			}
			else if (mode == ParseMode::Comment)
			{
				if (*pch == '\n')
				{
					stackMode.pop();
					mode = stackMode.top();
				}
			}
			else if (mode == ParseMode::Whitespace)
			{
				switch (*pch)
				{
				case '"':
					stackMode.push(ParseMode::Quote);
					break;

				case ';':
					stackMode.push(ParseMode::Comment);
					break;

				case '(':
					if (cblock == 0 || fNestCmd)
					{
						stackstrCmd.push(std::string());
						stackoffsetBlockStart.push(ftell(pf) - (pchMax - pch));
						stackMode.push(ParseMode::Command);
					}
					++cblock;
					break;

				case ')':
					if (cblock == 1 || fNestCmd)
					{
						off_t offsetCur = ftell(pf) - (pchMax - (pch+1));
						ProcessCommand(stackstrCmd.top(), pf, stackoffsetBlockStart.top(), offsetCur);
						stackstrCmd.pop();
						stackoffsetBlockStart.pop();
						if (cblock == 1)
							fNestCmd = false;
					}
					--cblock;
					break;
				}
			}
			fEscapeLast = *pch == '\\';
			++pch;
		}
	}
	assert(stackstrCmd.empty());
	assert(cblock == 0);
	stackMode.pop();
	assert(stackMode.empty());
	fclose(pf);
	return EXIT_SUCCESS;
}