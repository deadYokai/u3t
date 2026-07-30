#pragma once
#include <Zydis/Zydis.h>
#include <cstddef>

namespace dx
{
	inline ZydisDecoder native_decoder(bool x64 = (sizeof(void *) == 8))
	{
		ZydisDecoder d;
		ZydisDecoderInit(
		    &d, x64 ? ZYDIS_MACHINE_MODE_LONG_64 : ZYDIS_MACHINE_MODE_LEGACY_32,
		    x64 ? ZYDIS_STACK_WIDTH_64 : ZYDIS_STACK_WIDTH_32);
		return d;
	}

	struct Decoder
	{
		ZydisDecoder d;

		explicit Decoder(bool x64 = (sizeof(void *) == 8))
		    : d(native_decoder(x64))
		{
		}

		bool decode(const void *p, size_t avail, ZydisDecodedInstruction &in,
		            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT]) const
		{
			return ZYAN_SUCCESS(ZydisDecoderDecodeFull(
			    &d, p, static_cast<ZyanUSize>(avail), &in, ops));
		}
	};
}  // namespace dx
