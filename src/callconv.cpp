#include "callconv.hpp"

#include <Zydis/Zydis.h>
#include <cstdint>

#include "decode.hpp"
#include "disp_extract.hpp"
#include "disp_extract_arch.hpp"
#include "util.hpp"

namespace dx
{
	namespace
	{
		inline const uint8_t *u8(const void *p)
		{
			return reinterpret_cast<const uint8_t *>(p);
		}

		constexpr int kPS = static_cast<int>(sizeof(void *));

		constexpr int kIdxCX = 1;

		int reg_argnum_liveness(const void *begin, const void *end,
		                        const int *arg_regs, int n_regs, int max_insns)
		{
			if (!begin || !end || n_regs <= 0)
				return 0;

			Decoder dec;
			bool live[4] = {};
			bool dead[4] = {};

			const uint8_t *p = u8(begin), *e = u8(end);
			ZydisDecodedInstruction in;
			ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];

			for (int i = 0; i < max_insns && p < e; ++i)
			{
				if (!dec.decode(p, e - p, in, ops))
					break;

				if (is_flow_break(in))
					break;

				const bool zeroing_idiom =
				    is_mnemonic(in, {ZYDIS_MNEMONIC_XOR, ZYDIS_MNEMONIC_SUB}) &&
				    in.operand_count_visible == 2 && is_reg(ops[0]) &&
				    is_reg(ops[1]) &&
				    dxa::gpr_idx(ops[0].reg.value) ==
				        dxa::gpr_idx(ops[1].reg.value);

				if (!zeroing_idiom)
				{
					for (int oi = 0; oi < in.operand_count; ++oi)
					{
						const auto &op = ops[oi];
						if (is_reg(op))
						{
							if (!(op.actions & ZYDIS_OPERAND_ACTION_MASK_READ))
								continue;
							const int gi = dxa::gpr_idx(op.reg.value);
							for (int a = 0; a < n_regs; ++a)
								if (gi == arg_regs[a] && !dead[a])
									live[a] = true;
						}
						else if (is_mem(op))
						{
							for (ZydisRegister r : {op.mem.base, op.mem.index})
							{
								const int gi = dxa::gpr_idx(r);
								for (int a = 0; a < n_regs; ++a)
									if (gi == arg_regs[a] && !dead[a])
										live[a] = true;
							}
						}
					}
				}

				for (int oi = 0; oi < in.operand_count; ++oi)
				{
					const auto &op = ops[oi];
					if (!is_reg(op))
						continue;
					if (!(op.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE))
						continue;
					const int gi = dxa::gpr_idx(op.reg.value);
					for (int a = 0; a < n_regs; ++a)
						if (gi == arg_regs[a])
							dead[a] = true;
				}

				p += in.length;
			}

			int argnum = 0;
			for (int a = 0; a < n_regs; ++a)
				if (live[a])
					argnum = a + 1;
			return argnum;
		}

		bool first_reg_dereferenced(const void *begin, const void *end,
		                            int reg_idx, int max_insns)
		{
			if (!begin || !end)
				return false;

			Decoder dec;
			const uint8_t *p = u8(begin), *e = u8(end);
			ZydisDecodedInstruction in;
			ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
			bool dead = false;

			for (int i = 0; i < max_insns && p < e; ++i)
			{
				if (!dec.decode(p, e - p, in, ops))
					break;

				if (is_flow_break(in))
					break;

				if (!dead)
				{
					for (int oi = 0; oi < in.operand_count; ++oi)
						if (is_mem(ops[oi]) &&
						    dxa::gpr_idx(ops[oi].mem.base) == reg_idx)
							return true;
				}

				for (int oi = 0; oi < in.operand_count; ++oi)
					if (is_reg(ops[oi]) &&
					    (ops[oi].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) &&
					    dxa::gpr_idx(ops[oi].reg.value) == reg_idx)
						dead = true;

				p += in.length;
			}
			return false;
		}
	}  // namespace

	const char *to_string(CallConv cc)
	{
		switch (cc)
		{
			case CallConv::Cdecl:
				return "__cdecl";
			case CallConv::Stdcall:
				return "__stdcall";
			case CallConv::Thiscall:
				return "__thiscall";
			case CallConv::Fastcall:
				return "__fastcall";
			case CallConv::Ms64:
				return "ms_x64 (default __fastcall on x64)";
			default:
				return "unknown";
		}
	}

	CallConvDetector::CallConvDetector(int max_prologue_insns)
	    : max_prologue_insns_(max_prologue_insns)
	{
	}

	CallConvResult CallConvDetector::detect(const void *begin,
	                                        const void *end) const
	{
		CallConvResult r;
		if (!begin)
			return r;

		const bool end_was_guessed = (end == nullptr);
		const void *scan_end = end ? end : guess_function_end(begin);

		if (kPS == 8)
		{
			r.conv = CallConv::Ms64;
			r.stack_cleanup_bytes = 0;
			r.reg_int_args =
			    x64_argnum_liveness(begin, scan_end, max_prologue_insns_);
			r.ecx_looks_like_this =
			    r.reg_int_args >= 1 &&
			    first_reg_dereferenced(begin, scan_end, kIdxCX,
			                           max_prologue_insns_);
			r.confident = r.reg_int_args > 0 || !end_was_guessed;
			return r;
		}

		const int fastcall_regs[2] = {kIdxCX, 2};

		r.reg_int_args = reg_argnum_liveness(begin, scan_end, fastcall_regs, 2,
		                                     max_prologue_insns_);
		r.stack_cleanup_bytes =
		    call_stack_bytes(nullptr, nullptr, begin, scan_end);

		switch (r.reg_int_args)
		{
			case 2:
				r.conv = CallConv::Fastcall;
				r.confident = true;
				break;
			case 1:
				r.conv = CallConv::Thiscall;
				r.ecx_looks_like_this = first_reg_dereferenced(
				    begin, scan_end, kIdxCX, max_prologue_insns_);
				r.confident = true;
				break;
			default:
				if (r.stack_cleanup_bytes > 0)
				{
					r.conv = CallConv::Stdcall;
					r.confident = true;
				}
				else if (r.stack_cleanup_bytes == 0)
				{
					r.conv = CallConv::Cdecl;
					r.confident = false;
				}
				else
				{
					r.conv = CallConv::Unknown;
					r.confident = false;
				}
				break;
		}

		return r;
	}

	CallConvResult detect_call_conv(const void *begin, const void *end,
	                                int max_prologue_insns)
	{
		return CallConvDetector(max_prologue_insns).detect(begin, end);
	}

	const void *guess_function_end(const void *begin, size_t max_scan_bytes,
	                               int padding_run)
	{
		if (!begin || padding_run <= 0)
			return begin;

		const uint8_t *p = u8(begin);
		const uint8_t *limit = p + max_scan_bytes;

		while (p + padding_run <= limit)
		{
			bool all_int3 = true;
			for (int i = 0; i < padding_run; ++i)
			{
				if (p[i] != 0xCC)
				{
					all_int3 = false;
					break;
				}
			}
			if (all_int3)
				return p;
			++p;
		}
		return limit;
	}
}  // namespace dx
