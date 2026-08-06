#include "farchive_vtable.hpp"
#include "anchor.hpp"
#include <Zydis/Zydis.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "decode.hpp"

#include "asm_pat.hpp"
#include "disp_extract.hpp"
#include "disp_extract_arch.hpp"
#include "logs.hpp"

namespace
{
	constexpr int PS = static_cast<int>(sizeof(void *));

	ZydisDecoder mkdec() { return dx::native_decoder(); }

	const uint8_t *u8(const void *p) { return static_cast<const uint8_t *>(p); }

	const uint8_t *body_end_by_ret(const anchor::ModuleImage &img,
	                               const void *fn, size_t cap = 0x200)
	{
		ZydisDecoder dec = mkdec();
		const uint8_t *p = u8(fn);
		const uint8_t *e = p + cap;
		if (e > img.text + img.text_size)
			e = img.text + img.text_size;

		ZydisDecodedInstruction in;
		ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
		while (p < e)
		{
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec, p, e - p, &in, ops)))
				break;
			p += in.length;
			if (in.mnemonic == ZYDIS_MNEMONIC_RET)
				return p;
		}
		return e;
	}

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

	struct ForwarderHit
	{
		int self_slot;
		int target_slot;
	};

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
		int fn_reg = -1, fn_slot = -1;

		while (p < e)
		{
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec, p, e - p, &in, ops)))
				break;

			if (in.mnemonic == ZYDIS_MNEMONIC_MOV &&
			    ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
			    ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY)
			{
				int dst = dxa::gpr_idx(ops[0].reg.value);

				if (dst == fn_reg)
					fn_reg = -1;

				if (ops[1].mem.base != ZYDIS_REGISTER_NONE &&
				    ops[1].mem.base != ZYDIS_REGISTER_RIP &&
				    ops[1].mem.disp.has_displacement &&
				    ops[1].mem.disp.value == loader_off_fa)
				{
					loader_reg = dst;
					vtbl_reg = -1;
				}
				else if (ops[1].mem.base != ZYDIS_REGISTER_NONE &&
				         dxa::gpr_idx(ops[1].mem.base) == loader_reg &&
				         (!ops[1].mem.disp.has_displacement ||
				          ops[1].mem.disp.value == 0))
				{
					vtbl_reg = dst;
				}
				else if (ops[1].mem.base != ZYDIS_REGISTER_NONE &&
				         dxa::gpr_idx(ops[1].mem.base) == vtbl_reg &&
				         ops[1].mem.disp.has_displacement &&
				         ops[1].mem.disp.value != 0)
				{
					fn_reg = dst;
					fn_slot = static_cast<int>(ops[1].mem.disp.value / PS);
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
			         dxa::gpr_idx(ops[0].mem.base) == vtbl_reg &&
			         ops[0].mem.disp.has_displacement)
			{
				slots.push_back(static_cast<int>(ops[0].mem.disp.value / PS));
			}
			else if (in.mnemonic == ZYDIS_MNEMONIC_CALL &&
			         ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
			         dxa::gpr_idx(ops[0].reg.value) == fn_reg && fn_reg >= 0)
			{
				slots.push_back(fn_slot);
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

	int find_get_error_slot(const anchor::ModuleImage &img, void *seek_impl,
	                        int out_seek_slot, int32_t &ar_is_error_off_out)
	{
		ar_is_error_off_out = 0;
		if (!seek_impl || out_seek_slot <= 0)
			return -1;

		const uint8_t *body_end = anchor::function_end(img, seek_impl);
		if (!body_end || body_end <= u8(seek_impl))
			body_end = u8(seek_impl) + 0x400;
		if (body_end > img.text + img.text_size)
			body_end = img.text + img.text_size;

		const uint8_t *hit =
		    asmpat::asmfindpat(seek_impl, body_end, {}, {}, "mov mem, 1", 0);
		if (!hit)
			return -1;

		ZydisDecoder dec = mkdec();
		ZydisDecodedInstruction in;
		ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
		if (ZYAN_FAILED(
		        ZydisDecoderDecodeFull(&dec, hit, body_end - hit, &in, ops)) ||
		    !ops[0].mem.disp.has_displacement)
			return -1;

		const int32_t off = static_cast<int32_t>(ops[0].mem.disp.value);
		ar_is_error_off_out = off;

		const uint8_t *vt_seek = find_ptr_in_data(img, seek_impl);
		if (!vt_seek)
			return -1;
		const uint8_t *vt_base = vt_seek - out_seek_slot * PS;

		auto read_pat = asmpat::parse("mov reg, [" + std::to_string(off) + "]");

		for (int slot = 0; slot < 64; ++slot)
		{
			const uint8_t *slot_addr = vt_base + slot * PS;
			if (slot_addr < img.base || slot_addr + PS > img.base + img.size)
				continue;
			uintptr_t fp = 0;
			memcpy(&fp, slot_addr, PS);
			auto *fn = reinterpret_cast<const void *>(fp);
			if (fn < img.text || fn >= img.text + img.text_size)
				continue;

			const uint8_t *p1 = u8(fn);
			const uint8_t *pend = p1 + 16;
			if (pend > img.text + img.text_size)
				pend = img.text + img.text_size;

			ZydisDecodedInstruction in1, in2;
			ZydisDecodedOperand ops1[ZYDIS_MAX_OPERAND_COUNT];
			if (ZYAN_FAILED(
			        ZydisDecoderDecodeFull(&dec, p1, pend - p1, &in1, ops1)))
				continue;
			if (!asmpat::matches(read_pat, in1, ops1))
				continue;

			const uint8_t *p2 = p1 + in1.length;
			ZydisDecodedOperand ops2[ZYDIS_MAX_OPERAND_COUNT];
			if (ZYAN_FAILED(
			        ZydisDecoderDecodeFull(&dec, p2, pend - p2, &in2, ops2)))
				continue;
			if (in2.mnemonic != ZYDIS_MNEMONIC_RET)
				continue;

			return slot;
		}
		return -1;
	}

	bool accessor_field(const anchor::ModuleImage &img, const void *fn,
	                    int32_t &off_out, bool &plain_out)
	{
		ZydisDecoder dec = mkdec();
		const uint8_t *p = u8(fn);
		const uint8_t *e = p + 24;
		if (e > img.text + img.text_size)
			e = img.text + img.text_size;

		ZydisDecodedInstruction in;
		ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];

		if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec, p, e - p, &in, ops)))
			return false;
		if (in.mnemonic != ZYDIS_MNEMONIC_MOV || in.operand_count_visible < 2)
			return false;
		if (ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
		    ops[1].type != ZYDIS_OPERAND_TYPE_MEMORY)
			return false;
		if (ops[1].mem.base == ZYDIS_REGISTER_NONE ||
		    ops[1].mem.base == ZYDIS_REGISTER_RIP ||
		    ops[1].mem.index != ZYDIS_REGISTER_NONE)
			return false;

		off_out = static_cast<int32_t>(
		    ops[1].mem.disp.has_displacement ? ops[1].mem.disp.value : 0);
		p += in.length;

		for (int i = 0; i < 6 && p < e; ++i)
		{
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec, p, e - p, &in, ops)))
				return false;
			if (in.mnemonic == ZYDIS_MNEMONIC_RET)
			{
				plain_out = (i == 0);
				return true;
			}
			if (in.mnemonic == ZYDIS_MNEMONIC_CALL ||
			    in.mnemonic == ZYDIS_MNEMONIC_JMP)
				return false;
			p += in.length;
		}
		return false;
	}
}  // namespace

bool resolve_farchive_slots(FArchiveSlots &out, void *preload,
                            const std::vector<void *> &fname_ops,
                            void *seek_impl, ptrdiff_t farchive_off,
                            ptrdiff_t loader_off)
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

	std::vector<ForwarderHit> fwd_hits;
	int fname_op_self_slot = -1;
	int fname_pat_slot = -1;

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

			const int self_slot = static_cast<int>((q - lo) / PS);

			if (fname_op_self_slot < 0 &&
			    std::find(fname_ops.begin(), fname_ops.end(),
			              const_cast<void *>(fn)) != fname_ops.end())
			{
				fname_op_self_slot = self_slot;
				log_info("farchive: operator<<(FName)=%p via vtable "
				         "referrer (Preload%+d)",
				         fn, (int)((q - vt_preload) / PS));
			}

			if (fname_pat_slot < 0)
			{
				const ptrdiff_t rel = (q - vt_preload) / PS;
				if (rel >= 1 && rel <= 8)
				{
					const uint8_t *fe = body_end_by_ret(img, fn, 0x200);
					if (fe > u8(fn) && dx::has_fname_none_compare(fn, fe))
						fname_pat_slot = self_slot;
				}
			}

			int k = forwarder_slot(fn, u8(fn) + 0x20, loader_off_fa);
			if (k > 0)
			{
				fwd_hits.push_back({self_slot, k});
				min_fwd = (std::min)(min_fwd, k);
				max_slot_seen = (std::max)(max_slot_seen, k);
			}
		}

		if (min_fwd != (1 << 30))
			out.Serialize = min_fwd;
		out.total = (std::max)(max_slot_seen + 1, 32);

		if (fname_op_self_slot < 0 && fname_pat_slot >= 0)
		{
			fname_op_self_slot = fname_pat_slot;
			log_info("farchive: operator<<(FName) via NAME_None pattern "
			         "(slot %d)",
			         fname_pat_slot);
		}
	}

	int slot_bias = 0;
	bool have_bias = false;
	{
		bool have_a = false, have_b = false;
		int bias_a = 0, bias_b = 0;
		for (const auto &h : fwd_hits)
		{
			if (!have_a && h.target_slot == out.Serialize)
			{
				bias_a = out.Serialize - h.self_slot;
				have_a = true;
			}
			if (!have_b && out.Tell > 0 && h.target_slot == out.Tell)
			{
				bias_b = out.Tell - h.self_slot;
				have_b = true;
			}
		}
		if (have_a && have_b)
		{
			have_bias = bias_a == bias_b;
			slot_bias = bias_a;
			if (!have_bias)
				log_warn(
				    "resolve: vtable slot bias disagrees between Serialize "
				    "and Tell anchors (%d vs %d) — skipping runtime "
				    "GetError derivation",
				    bias_a, bias_b);
		}
		else if (have_a || have_b)
		{
			slot_bias = have_a ? bias_a : bias_b;
			have_bias = true;
		}
	}

	if (out.Tell > 0)
	{
		int tell_self = -1;
		for (const auto &h : fwd_hits)
			if (h.target_slot == out.Tell)
			{
				tell_self = h.self_slot;
				break;
			}
		if (tell_self >= 0)
			for (const auto &h : fwd_hits)
				if (h.self_slot == tell_self + 1)
				{
					out.TotalSize = h.target_slot;
					break;
				}
		if (out.TotalSize <= 0)
			log_warn("resolve: TotalSize not derived at runtime (no "
			         "forwarder found immediately after Tell's vtable slot)");
	}

	bool serialize_name_direct = false;
	if (fname_op_self_slot >= 0 && have_bias)
	{
		out.SerializeName = fname_op_self_slot + slot_bias;
		serialize_name_direct = true;
	}
	else
	{
		log_warn("resolve: SerializeName not derived at runtime — no direct "
		         "operator<<(FName&)");
	}

	int32_t ar_is_error_off = 0;
	int ge = find_get_error_slot(img, seek_impl, out.Seek, ar_is_error_off);
	if (ge > 0)
		out.GetError = ge;

	const bool core_ok = out.Serialize == 1 && out.Tell > 0 &&
	                     out.Seek == out.Tell + 3 &&
	                     out.Precache == out.Tell + 6;

	if (out.TotalSize <= 0 && out.Tell > 0)
	{
		log_warn("resolve: falling back to TotalSize=Tell+1 (UNVERIFIED)");
		out.TotalSize = out.Tell + 1;
	}

	if (out.GetError <= 0 && vt_preload && have_bias && ar_is_error_off != 0)
	{
		const uint8_t *lo = vt_preload - 24 * PS;
		if (lo < img.base)
			lo = img.base;
		const uint8_t *hi = vt_preload + 24 * PS;
		if (hi > img.base + img.size - PS)
			hi = img.base + img.size - PS;

		int plain_slot = -1, close_slot = -1;

		for (const uint8_t *q = lo; q <= hi; q += PS)
		{
			uintptr_t fp = 0;
			memcpy(&fp, q, PS);
			auto *fn = reinterpret_cast<const void *>(fp);
			if (fn < img.text || fn >= img.text + img.text_size)
				continue;

			int32_t off = 0;
			bool plain = false;
			if (!accessor_field(img, fn, off, plain))
				continue;
			if (off != ar_is_error_off)
				continue;

			const int slot = static_cast<int>((q - lo) / PS) + slot_bias;
			if (plain && plain_slot < 0)
				plain_slot = slot;
			else if (!plain && close_slot < 0)
				close_slot = slot;
		}

		if (plain_slot > 0)
		{
			out.GetError = plain_slot;
			log_info("farchive: GetError=%d via ArIsError@0x%X accessor in "
			         "the Preload-anchored vtable (Close=%d)",
			         plain_slot, (unsigned)ar_is_error_off, close_slot);
		}
	}

	if (out.GetError <= 0)
	{
		log_err("resolve: GetError not derived");
	}

	if (out.SerializeName <= 0)
	{
		log_err("resolve: falling back to SerializeName=6 (UNVERIFIED)");
		out.SerializeName = 6;
	}

	if (out.GetError <= 0)
	{
		log_err("resolve: falling back to GetError=21 (UNVERIFIED, known "
		        "wrong on at least one build — check the warnings above)");
		out.GetError = 21;
	}

	out.validated = core_ok;
	if (out.total < 32)
		out.total = 32;

	log_info("resolve: farchive slots Serialize=%d SerializeName=%d(direct=%d) "
	         "Tell=%d Seek=%d TotalSize=%d Precache=%d GetError=%d total=%d "
	         "core_ok=%d slot_bias=%d",
	         out.Serialize, out.SerializeName, (int)serialize_name_direct,
	         out.Tell, out.Seek, out.TotalSize, out.Precache, out.GetError,
	         out.total, (int)core_ok, slot_bias);

	return core_ok || (out.Serialize > 0 && out.Tell > 0 && out.Seek > 0 &&
	                   out.Precache > 0);
}
