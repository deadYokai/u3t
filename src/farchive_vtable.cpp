#include "farchive_vtable.hpp"
#include "anchor.hpp"
#include <Zydis/Zydis.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace
{
	constexpr int PS = static_cast<int>(sizeof(void *));

	ZydisDecoder mkdec()
	{
		ZydisDecoder d;
		ZydisDecoderInit(&d,
		                 PS == 8 ? ZYDIS_MACHINE_MODE_LONG_64
		                         : ZYDIS_MACHINE_MODE_LEGACY_32,
		                 PS == 8 ? ZYDIS_STACK_WIDTH_64 : ZYDIS_STACK_WIDTH_32);
		return d;
	}

	const uint8_t *u8(const void *p) { return static_cast<const uint8_t *>(p); }

	int forwarder_slot(const void *fn, const void *fn_end,
	                   ptrdiff_t loader_off_fa)
	{
		ZydisDecoder dec = mkdec();
		const uint8_t *p = u8(fn), *e = u8(fn_end);
		ZydisDecodedInstruction in;
		ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];

		if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec, p, e - p, &in, ops)))
			return -1;
		if (in.mnemonic != ZYDIS_MNEMONIC_MOV ||
		    ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
		    ops[1].type != ZYDIS_OPERAND_TYPE_MEMORY ||
		    ops[1].mem.base == ZYDIS_REGISTER_NONE ||
		    ops[1].mem.base == ZYDIS_REGISTER_RIP ||
		    !ops[1].mem.disp.has_displacement ||
		    ops[1].mem.disp.value != loader_off_fa)
			return -1;
		p += in.length;

		if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec, p, e - p, &in, ops)))
			return -1;
		if (in.mnemonic != ZYDIS_MNEMONIC_MOV ||
		    ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
		    ops[1].type != ZYDIS_OPERAND_TYPE_MEMORY)
			return -1;
		p += in.length;

		if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec, p, e - p, &in, ops)))
			return -1;
		if (in.mnemonic == ZYDIS_MNEMONIC_JMP &&
		    ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
		    ops[0].mem.base != ZYDIS_REGISTER_NONE &&
		    ops[0].mem.disp.has_displacement)
			return static_cast<int>(ops[0].mem.disp.value / PS);
		if (in.mnemonic == ZYDIS_MNEMONIC_MOV &&
		    ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
		    ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
		    ops[1].mem.disp.has_displacement)
			return static_cast<int>(ops[1].mem.disp.value / PS);
		return -1;
	}

	std::vector<int> preload_loader_calls(const void *begin, const void *end,
	                                      ptrdiff_t loader_off_fa)
	{
		std::vector<int> slots;
		ZydisDecoder dec = mkdec();
		const uint8_t *p = u8(begin), *e = u8(end);
		ZydisDecodedInstruction in;
		ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];

		int loader_reg = -1;
		int vtbl_reg = -1;
		auto gi = [](ZydisRegister r)
		{
			ZydisRegister enc = ZydisRegisterGetLargestEnclosing(
			    PS == 8 ? ZYDIS_MACHINE_MODE_LONG_64
			            : ZYDIS_MACHINE_MODE_LEGACY_32,
			    r);
			return (enc >= ZYDIS_REGISTER_RAX && enc <= ZYDIS_REGISTER_R15)
			           ? int(enc - ZYDIS_REGISTER_RAX)
			           : -1;
		};

		while (p < e)
		{
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec, p, e - p, &in, ops)))
				break;

			if (in.mnemonic == ZYDIS_MNEMONIC_MOV &&
			    ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
			    ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY)
			{
				int dst = gi(ops[0].reg.value);
				if (ops[1].mem.base != ZYDIS_REGISTER_NONE &&
				    ops[1].mem.base != ZYDIS_REGISTER_RIP &&
				    ops[1].mem.disp.has_displacement &&
				    ops[1].mem.disp.value == loader_off_fa)
				{
					loader_reg = dst;
					vtbl_reg = -1;
				}
				else if (ops[1].mem.base != ZYDIS_REGISTER_NONE &&
				         gi(ops[1].mem.base) == loader_reg &&
				         (!ops[1].mem.disp.has_displacement ||
				          ops[1].mem.disp.value == 0))
				{
					vtbl_reg = dst;
				}
				else if (dst == loader_reg || dst == vtbl_reg)
				{
					if (dst == loader_reg)
						loader_reg = -1;
					if (dst == vtbl_reg)
						vtbl_reg = -1;
				}
			}
			else if (in.mnemonic == ZYDIS_MNEMONIC_CALL &&
			         ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
			         ops[0].mem.base != ZYDIS_REGISTER_NONE &&
			         gi(ops[0].mem.base) == vtbl_reg &&
			         ops[0].mem.disp.has_displacement)
			{
				slots.push_back(static_cast<int>(ops[0].mem.disp.value / PS));
			}
			p += in.length;
		}
		return slots;
	}

	const uint8_t *find_ptr_in_data(const anchor::ModuleImage &img,
	                                const void *target)
	{
		const auto tv = reinterpret_cast<uintptr_t>(target);
		for (const uint8_t *q = img.base; q + PS <= img.base + img.size;
		     q += PS)
		{
			uintptr_t v = 0;
			memcpy(&v, q, PS);
			if (v == tv)
				return q;
		}
		return nullptr;
	}
}  // namespace

bool resolve_farchive_slots(FArchiveSlots &out, void *preload,
                            ptrdiff_t farchive_off, ptrdiff_t loader_off)
{
	anchor::ModuleImage img = anchor::image_of(nullptr);
	if (!img.ok || !preload)
		return false;

	const ptrdiff_t loader_off_fa = loader_off - farchive_off;

	{
		auto calls = preload_loader_calls(
		    preload, anchor::function_end(img, preload), loader_off_fa);
		std::vector<int> distinct;
		for (int s : calls)
			if (std::find(distinct.begin(), distinct.end(), s) ==
			    distinct.end())
				distinct.push_back(s);
		if (distinct.size() >= 3)
		{
			out.Tell = distinct[0];
			out.Seek = distinct[1];
			out.Precache = distinct[2];
		}
	}

	const uint8_t *vt_preload = find_ptr_in_data(img, preload);
	if (vt_preload)
	{
		const uint8_t *lo = vt_preload - 24 * PS;
		if (lo < img.base)
			lo = img.base;
		const uint8_t *hi = vt_preload + 24 * PS;
		if (hi > img.base + img.size - PS)
			hi = img.base + img.size - PS;

		int min_fwd = 1 << 30;
		int max_slot_seen = 0;
		for (const uint8_t *q = lo; q <= hi; q += PS)
		{
			uintptr_t fp = 0;
			memcpy(&fp, q, PS);
			auto *fn = reinterpret_cast<const void *>(fp);
			if (fn < img.text || fn >= img.text + img.text_size)
				continue;
			int k = forwarder_slot(fn, u8(fn) + 0x20, loader_off_fa);
			if (k > 0)
			{
				min_fwd = (std::min)(min_fwd, k);
				max_slot_seen = (std::max)(max_slot_seen, k);
			}
		}
		if (min_fwd != (1 << 30))
			out.Serialize = min_fwd;
		out.total = (std::max)(max_slot_seen + 1, 32);
	}

	if (out.Serialize == 1 && out.Tell > 0 && out.Seek == out.Tell + 3 &&
	    out.Precache == out.Tell + 6)
	{
		out.validated = true;
		out.SerializeName = 6;
		out.TotalSize = out.Tell + 1;  // 11
		out.GetError = 21;
		if (out.total < 32)
			out.total = 32;
		return true;
	}

	return out.Serialize > 0 && out.Tell > 0 && out.Seek > 0 &&
	       out.Precache > 0;
}
