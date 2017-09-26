#pragma once
#include "wasm_types.h"

// The expression service executes 1-off expressions as required by the loader.  This is not intended to supplement behavior of the JIT engine
class ExpressionService
{
public:
	struct Variant
	{
		uint64_t val;
		value_type type;
	};

	size_t CbEatExpression(const uint8_t *rgb, size_t cb, _Out_ Variant *pvalOut);	// consume an expression from a byte buffer and inform the caller how long it was

private:
	std::vector<uint8_t> m_vecbytecode;
};