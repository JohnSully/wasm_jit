#include "stdafx.h"
#include "ExpressionService.h"
#include "safe_access.h"
#include "Exceptions.h"

size_t ExpressionService::CbEatExpression(const uint8_t *rgb, size_t cb, _Out_ Variant *pvariantOut)
{
	const size_t cbStart = cb;
	opcode op = safe_read_buffer<opcode>(&rgb, &cb);
	switch (op)
	{
	case opcode::i32_const:
		pvariantOut->type = value_type::i32;
		pvariantOut->val = safe_read_buffer<varuint32>(&rgb, &cb);
		break;

	default:
		Verify(false);
	}

	op = safe_read_buffer<opcode>(&rgb, &cb);
	Verify(op == opcode::end);

	return cbStart - cb;
}