// webasmRT.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <Windows.h>
#include <inttypes.h>
#include "WasmContext.h"

int main(int argc, char *argv[])
{
	for (int iarg = 1; iarg < argc; ++iarg)
	{
		printf("arg %d: %s\n", iarg, argv[iarg]);
	}
	if (argc < 2)
		return EXIT_FAILURE;
	FILE *pf = fopen(argv[1], "rb");
	if (pf == nullptr)
		return EXIT_FAILURE;

	WasmContext ctxt;
	ctxt.LoadModule(pf);
	fclose(pf);
	
	ctxt.CallFunction("main");

    return EXIT_SUCCESS;
}

