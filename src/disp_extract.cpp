#include "disp_extract.hpp"
#include <Zydis/Zydis.h>
#include <algorithm>
#include <cstdint>

#include "disp_extract_arch.hpp"

#include "asm_pat.hpp"

namespace dx
{
	namespace
	{
		struct Dec
		{
			ZydisDecoder d;

			Dec()
			{
				ZydisDecoderInit(&d,
				                 sizeof(void *) == 8
				                     ? ZYDIS_MACHINE_MODE_LONG_64
				                     : ZYDIS_MACHINE_MODE_LEGACY_32,
				                 sizeof(void *) == 8 ? ZYDIS_STACK_WIDTH_64
				                                     : ZYDIS_STACK_WIDTH_32);
			}
		};

		inline const uint8_t *u8(const void *p)
		{
			return reinterpret_cast<const uint8_t *>(p);
		}
	}  // namespace

	bool first_neg_lea(const void *begin, const void *end, int64_t &out_disp,
	                   int max_insns)
	{
		Dec dec;
		const uint8_t *p = u8(begin), *e = u8(end);
		ZydisDecodedInstruction in;
		ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
		for (int i = 0; i < max_insns && p < e; ++i)
		{
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec.d, p, e - p, &in, ops)))
				break;
			if (in.mnemonic == ZYDIS_MNEMONIC_LEA &&
			    in.operand_count_visible >= 2 && is_mem_reg(ops[1]) &&
			    ops[1].mem.disp.has_displacement && ops[1].mem.disp.value < 0)
			{
				out_disp = -ops[1].mem.disp.value;
				return true;
			}
			p += in.length;
		}
		return false;
	}

	bool first_imul_imm(const void *begin, const void *end, int64_t &out_imm)
	{
		Dec dec;
		const uint8_t *p = u8(begin), *e = u8(end);
		ZydisDecodedInstruction in;
		ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
		while (p < e)
		{
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec.d, p, e - p, &in, ops)))
				break;
			if (in.mnemonic == ZYDIS_MNEMONIC_IMUL)
			{
				for (int i = 0; i < in.operand_count_visible; ++i)
					if (is_imm(ops[i]))
					{
						out_imm = ops[i].imm.value.s;
						return true;
					}
			}
			p += in.length;
		}
		return false;
	}

	bool rip_store_then_vcall(const void *begin, const void *end,
	                          void **&out_global, int &out_slot,
	                          int call_window)
	{
		Dec dec;
		const uint8_t *p = u8(begin), *e = u8(end);
		ZydisDecodedInstruction in;
		ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];

		AsmAgo since_store(call_window);
		const uint8_t *store_ip = nullptr;
		uint32_t store_len = 0;
		int32_t store_disp = 0;

		while (p < e)
		{
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec.d, p, e - p, &in, ops)))
				break;

			if (in.mnemonic == ZYDIS_MNEMONIC_MOV &&
			    in.operand_count_visible >= 2 && is_mem_rip(ops[0]) &&
			    is_reg(ops[1]))
			{
				store_ip = p;
				store_len = in.length;
				store_disp = static_cast<int32_t>(ops[0].mem.disp.value);
				since_store.hit();
			}
			else
			{
				since_store.tick();
			}

			if (since_store.seen() && in.mnemonic == ZYDIS_MNEMONIC_CALL &&
			    in.operand_count_visible == 1 && is_mem_reg(ops[0]) &&
			    ops[0].mem.disp.has_displacement)
			{
				const int64_t d = ops[0].mem.disp.value;
				if (d >= 0 && (d % 8) == 0 && d < 64 * 8)
				{
					out_slot = static_cast<int>(d / 8);
					const uint8_t *next = store_ip + store_len;
					out_global = reinterpret_cast<void **>(
					    const_cast<uint8_t *>(next) + store_disp);
					return true;
				}
			}
			p += in.length;
		}
		return false;
	}

	bool nth_rip_global(const void *begin, const void *end, int n,
	                    void **&out_global)
	{
		Dec dec;
		const uint8_t *p = u8(begin), *e = u8(end);
		ZydisDecodedInstruction in;
		ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
		int seen = 0;
		while (p < e)
		{
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec.d, p, e - p, &in, ops)))
				break;
			if (is_mnemonic(in, {ZYDIS_MNEMONIC_MOV, ZYDIS_MNEMONIC_LEA}) &&
			    in.operand_count_visible >= 2 && is_mem_rip(ops[1]))
			{
				if (seen++ == n)
				{
					const uint8_t *next = p + in.length;
					out_global = reinterpret_cast<void **>(
					    const_cast<uint8_t *>(next) +
					    static_cast<int32_t>(ops[1].mem.disp.value));
					return true;
				}
			}
			p += in.length;
		}
		return false;
	}

	bool first_rip_global_noncookie(const void *begin, const void *end,
	                                void **&out_global)
	{
		Dec dec;
		const uint8_t *p = u8(begin), *e = u8(end);
		ZydisDecodedInstruction in;
		ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
		while (p < e)
		{
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec.d, p, e - p, &in, ops)))
				break;
			if (is_mnemonic(in, {ZYDIS_MNEMONIC_MOV, ZYDIS_MNEMONIC_LEA}) &&
			    in.operand_count_visible >= 2 && is_mem_rip(ops[1]))
			{
				const uint8_t *next = p + in.length;
				void **g = reinterpret_cast<void **>(
				    const_cast<uint8_t *>(next) +
				    static_cast<int32_t>(ops[1].mem.disp.value));

				ZydisDecodedInstruction in2;
				ZydisDecodedOperand ops2[ZYDIS_MAX_OPERAND_COUNT];
				bool is_cookie = false;
				if (next < e &&
				    ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec.d, next, e - next,
				                                        &in2, ops2)) &&
				    in2.mnemonic == ZYDIS_MNEMONIC_XOR &&
				    in2.operand_count_visible >= 2 &&
				    is_reg(ops2[1], ZYDIS_REGISTER_RSP))
					is_cookie = true;

				if (!is_cookie)
				{
					out_global = g;
					return true;
				}
			}
			p += in.length;
		}
		return false;
	}

	std::vector<int> indirect_call_slots(const void *begin, const void *end)
	{
		std::vector<int> out;
		Dec dec;
		const uint8_t *p = u8(begin), *e = u8(end);
		ZydisDecodedInstruction in;
		ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
		while (p < e)
		{
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec.d, p, e - p, &in, ops)))
				break;
			if (in.mnemonic == ZYDIS_MNEMONIC_CALL &&
			    in.operand_count_visible == 1 && is_mem_reg(ops[0]) &&
			    ops[0].mem.disp.has_displacement)
			{
				const int64_t d = ops[0].mem.disp.value;
				if (d >= 0 && (d % 8) == 0 && d < 64 * 8)
					out.push_back(static_cast<int>(d / 8));
			}
			p += in.length;
		}
		return out;
	}

	bool indexed_store_global(const void *begin, const void *end,
	                          void **&out_array_data, int scale)
	{
		Dec dec;
		const uint8_t *p = u8(begin), *e = u8(end);
		ZydisDecodedInstruction in;
		ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];

		void **reg_global[16];
		for (int i = 0; i < 16; ++i)
			reg_global[i] = nullptr;

		while (p < e)
		{
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec.d, p, e - p, &in, ops)))
				break;

			for (int i = 0; i < in.operand_count; ++i)
				if (is_reg(ops[i]) &&
				    (ops[i].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE))
				{
					int gi = dxa::gpr_idx(ops[i].reg.value);
					if (gi >= 0)
						reg_global[gi] = nullptr;
				}

			if (in.mnemonic == ZYDIS_MNEMONIC_MOV &&
			    in.operand_count_visible >= 2 && is_reg(ops[0]) &&
			    is_mem_rip(ops[1]))
			{
				int gi = dxa::gpr_idx(ops[0].reg.value);
				if (gi >= 0)
				{
					const uint8_t *next = p + in.length;
					reg_global[gi] = reinterpret_cast<void **>(
					    const_cast<uint8_t *>(next) +
					    static_cast<int32_t>(ops[1].mem.disp.value));
				}
			}
			else if (in.operand_count_visible >= 2 && is_mem_reg(ops[0]) &&
			         ops[0].mem.index != ZYDIS_REGISTER_NONE &&
			         ops[0].mem.scale == scale)
			{
				int gi = dxa::gpr_idx(ops[0].mem.base);
				if (gi >= 0 && reg_global[gi])
				{
					out_array_data = reg_global[gi];
					return true;
				}
			}
			p += in.length;
		}
		return false;
	}

	bool array_base_disp_for_stride(const void *begin, const void *end,
	                                int64_t stride, int64_t &out_disp)
	{
		Dec dec;
		const uint8_t *p = u8(begin), *e = u8(end);
		ZydisDecodedInstruction in;
		ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
		ZydisRegister idx_reg = ZYDIS_REGISTER_NONE;

		constexpr int64_t NONE = INT64_MIN;
		int64_t load_disp[16];
		for (int i = 0; i < 16; ++i)
			load_disp[i] = NONE;

		while (p < e)
		{
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec.d, p, e - p, &in, ops)))
				break;

			for (int i = 0; i < in.operand_count; ++i)
				if (is_reg(ops[i]) &&
				    (ops[i].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE))
				{
					int gi = dxa::gpr_idx(ops[i].reg.value);
					if (gi >= 0)
						load_disp[gi] = NONE;
				}

			if (in.mnemonic == ZYDIS_MNEMONIC_MOV &&
			    in.operand_count_visible >= 2 && is_reg(ops[0]) &&
			    is_mem_reg(ops[1]) && ops[1].mem.index == ZYDIS_REGISTER_NONE &&
			    ops[1].mem.disp.has_displacement)
			{
				int gi = dxa::gpr_idx(ops[0].reg.value);
				if (gi >= 0)
					load_disp[gi] = ops[1].mem.disp.value;
			}

			if (in.mnemonic == ZYDIS_MNEMONIC_IMUL && is_reg(ops[0]))
			{
				for (int i = 1; i < in.operand_count_visible; ++i)
					if (is_imm(ops[i]) && ops[i].imm.value.s == stride)
					{
						idx_reg = ops[0].reg.value;
						break;
					}
			}
			else if (idx_reg != ZYDIS_REGISTER_NONE &&
			         is_mnemonic(in,
			                     {ZYDIS_MNEMONIC_ADD, ZYDIS_MNEMONIC_LEA}) &&
			         is_reg(ops[0]) &&
			         dxa::gpr_idx(ops[0].reg.value) == dxa::gpr_idx(idx_reg) &&
			         in.operand_count_visible >= 2)
			{
				if (is_mem_reg(ops[1]) && ops[1].mem.disp.has_displacement)
				{
					out_disp = ops[1].mem.disp.value;
					return true;
				}
				if (in.mnemonic == ZYDIS_MNEMONIC_ADD && is_reg(ops[1]))
				{
					int s = dxa::gpr_idx(ops[1].reg.value);
					if (s >= 0 && load_disp[s] != NONE)
					{
						out_disp = load_disp[s];
						return true;
					}
				}
			}
			p += in.length;
		}
		return false;
	}

	bool field_disp_before_vcall(const void *begin, const void *end, int slot,
	                             int64_t &out_disp)
	{
		Dec dec;
		const uint8_t *p = u8(begin), *e = u8(end);
		ZydisDecodedInstruction in;
		ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];

		constexpr int64_t NONE = INT64_MIN;
		int64_t field_disp[16];
		int64_t vt_field_disp[16];
		for (int i = 0; i < 16; ++i)
			field_disp[i] = vt_field_disp[i] = NONE;

		while (p < e)
		{
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec.d, p, e - p, &in, ops)))
				break;

			for (int i = 0; i < in.operand_count; ++i)
				if (is_reg(ops[i]) &&
				    (ops[i].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE))
				{
					int gi = dxa::gpr_idx(ops[i].reg.value);
					if (gi >= 0)
					{
						field_disp[gi] = NONE;
						vt_field_disp[gi] = NONE;
					}
				}

			if (in.mnemonic == ZYDIS_MNEMONIC_MOV &&
			    in.operand_count_visible >= 2 && is_reg(ops[0]) &&
			    is_mem(ops[1]))
			{
				const int dst = dxa::gpr_idx(ops[0].reg.value);
				if (dst < 0)
				{
					p += in.length;
					continue;
				}

				if (is_mem_reg(ops[1]) &&
				    ops[1].mem.index == ZYDIS_REGISTER_NONE)
				{
					const int base = dxa::gpr_idx(ops[1].mem.base);
					if (base >= 0 && field_disp[base] != NONE &&
					    ops[1].mem.disp.value == 0)
					{
						vt_field_disp[dst] = field_disp[base];
					}
					else if (ops[1].mem.disp.has_displacement)
					{
						field_disp[dst] = ops[1].mem.disp.value;
					}
				}
			}
			else if (in.mnemonic == ZYDIS_MNEMONIC_CALL &&
			         in.operand_count_visible == 1 && is_mem_reg(ops[0]) &&
			         ops[0].mem.disp.has_displacement)
			{
				const int vbase = dxa::gpr_idx(ops[0].mem.base);
				const int64_t d = ops[0].mem.disp.value;
				if (vbase >= 0 && vt_field_disp[vbase] != NONE && d >= 0 &&
				    (d % 8) == 0 && d / 8 == slot)
				{
					out_disp = vt_field_disp[vbase];
					return true;
				}
			}
			p += in.length;
		}
		return false;
	}

	std::vector<void *> field_lea_call_targets(const void *begin,
	                                           const void *end,
	                                           int64_t field_disp)
	{
		std::vector<void *> out;
		Dec dec;
		const uint8_t *p = u8(begin), *e = u8(end);
		ZydisDecodedInstruction in;
		ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
		AsmAgo since_lea(3);
		while (p < e)
		{
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec.d, p, e - p, &in, ops)))
				break;
			if (in.mnemonic == ZYDIS_MNEMONIC_LEA &&
			    in.operand_count_visible >= 2 && is_mem_reg(ops[1]) &&
			    ops[1].mem.index == ZYDIS_REGISTER_NONE &&
			    ops[1].mem.disp.has_displacement &&
			    ops[1].mem.disp.value == field_disp)
			{
				since_lea.hit();
			}
			else if (in.mnemonic == ZYDIS_MNEMONIC_CALL &&
			         in.operand_count_visible >= 1 && is_imm(ops[0]) &&
			         since_lea.seen())
			{
				const uint8_t *next = p + in.length;
				out.push_back(const_cast<uint8_t *>(next) +
				              static_cast<int32_t>(ops[0].imm.value.s));
				since_lea.reset();
			}
			else
				since_lea.tick();
			p += in.length;
		}
		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());
		return out;
	}

	bool imm_then_call(const void *begin, const void *end, int64_t imm,
	                   int window)
	{
		Dec dec;
		const uint8_t *p = u8(begin), *e = u8(end);
		ZydisDecodedInstruction in;
		ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
		AsmAgo since_imm(window);
		while (p < e)
		{
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec.d, p, e - p, &in, ops)))
				break;

			if (in.mnemonic == ZYDIS_MNEMONIC_CALL &&
			    in.operand_count_visible >= 1 && is_imm(ops[0]) &&
			    since_imm.seen())
				return true;

			bool imm_load = false;
			if (is_mnemonic(in, {ZYDIS_MNEMONIC_PUSH, ZYDIS_MNEMONIC_MOV}))
				for (int i = 0; i < in.operand_count_visible; ++i)
					if (is_imm(ops[i], static_cast<uint64_t>(imm)))
					{
						imm_load = true;
						break;
					}

			if (imm_load)
				since_imm.hit();
			else
				since_imm.tick();
			p += in.length;
		}
		return false;
	}

	void *find_split_name_setup(const void *begin, const void *end)
	{
		using asmpat::OnDecodeFail;
		const uint8_t *hit =
		    sizeof(void *) == 8
		        ? asmpat::asmfindpat(begin, end,
		                             {"mov REG, 0x400", "mov RCX, RSI"},
		                             {"!call", "!ret", "!jmp"}, "call IMM", 6,
		                             OnDecodeFail::SkipByte)
		        : asmpat::asmfindpat(
		              begin, end, {"push 0x400", "lea REG, [ESP]", "push REG"},
		              {"!call", "!ret", "!jmp"}, "call IMM", 6,
		              OnDecodeFail::SkipByte);
		return const_cast<uint8_t *>(hit);
	}

	bool has_fname_none_store(const void *begin, const void *end)
	{
		Dec dec;
		const uint8_t *p = u8(begin), *e = u8(end);
		ZydisDecodedInstruction in;
		ZydisDecodedOperand op[ZYDIS_MAX_OPERAND_COUNT];

		bool have_lo = false, have_pair = false;
		int base = -1;
		ZydisRegister sreg = ZYDIS_REGISTER_NONE;  // x86 zero-source register
		bool imm_lo = false;

		while (p < e)
		{
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec.d, p, e - p, &in, op)))
				break;

			if (is_flow_break(in))
			{
				if (have_pair && in.mnemonic == ZYDIS_MNEMONIC_JMP)
					return true;

				have_lo = have_pair = false;
				base = -1;
				sreg = ZYDIS_REGISTER_NONE;
				imm_lo = false;
				p += in.length;
				continue;
			}

			if (have_pair)
			{
				have_pair = false;
				have_lo = false;
				p += in.length;
				continue;
			}

			bool consumed = false;

			const bool store = in.mnemonic == ZYDIS_MNEMONIC_MOV &&
			                   in.operand_count_visible >= 2 &&
			                   op[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
			                   op[0].size == 32 &&
			                   op[0].mem.base != ZYDIS_REGISTER_NONE &&
			                   op[0].mem.base != ZYDIS_REGISTER_RIP &&
			                   op[0].mem.index == ZYDIS_REGISTER_NONE;

			if (store)
			{
				const int64_t disp =
				    op[0].mem.disp.has_displacement ? op[0].mem.disp.value : 0;
				const int b = dxa::gpr_idx(op[0].mem.base);

				if (sizeof(void *) == 8)  // x64
				{
					if (b >= 0 && op[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
					    op[1].imm.value.u == 0)
					{
						if (disp == 0)
						{
							have_lo = true;
							base = b;
							consumed = true;
						}
						else if (disp == 4 && have_lo && b == base)
						{
							have_pair = true;
							have_lo = false;
							consumed = true;
						}
					}
				}
				else  // x86
				{
					const bool is_imm_zero =
					    op[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
					    op[1].imm.value.u == 0;
					const bool is_reg =
					    op[1].type == ZYDIS_OPERAND_TYPE_REGISTER;

					if (b >= 0 && (is_imm_zero || is_reg))
					{
						if (disp == 0)
						{
							have_lo = true;
							base = b;
							imm_lo = is_imm_zero;
							sreg =
							    is_reg ? op[1].reg.value : ZYDIS_REGISTER_NONE;
							consumed = true;
						}
						else if (disp == 4 && have_lo && b == base &&
						         ((imm_lo && is_imm_zero) ||
						          (!imm_lo && is_reg &&
						           op[1].reg.value == sreg)))
						{
							have_pair = true;
							have_lo = false;
							consumed = true;
						}
					}
				}
			}

			if (!consumed)
				have_lo = false;

			p += in.length;
		}
		return false;
	}

	void *call_feeding_global_store(const void *begin, const void *end,
	                                void **global)
	{
		Dec dec;
		const uint8_t *p = u8(begin), *e = u8(end);
		ZydisDecodedInstruction in;
		ZydisDecodedOperand op[ZYDIS_MAX_OPERAND_COUNT];
		void *last_call = nullptr;

		while (p < e)
		{
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec.d, p, e - p, &in, op)))
				break;

			if (in.mnemonic == ZYDIS_MNEMONIC_CALL &&
			    in.operand_count_visible >= 1 && is_imm(op[0]))
			{
				const uint8_t *next = p + in.length;
				last_call = const_cast<uint8_t *>(next) +
				            static_cast<int32_t>(op[0].imm.value.s);
			}
			else if (in.mnemonic == ZYDIS_MNEMONIC_MOV &&
			         in.operand_count_visible >= 2 && is_mem(op[0]) &&
			         is_reg(op[1]))
			{
				void **g = nullptr;
				if (is_mem_rip(op[0]))  // x64 rip-relative
					g = reinterpret_cast<void **>(
					    const_cast<uint8_t *>(p) + in.length +
					    static_cast<int32_t>(op[0].mem.disp.value));
				else if (op[0].mem.base == ZYDIS_REGISTER_NONE &&
				         op[0].mem.index == ZYDIS_REGISTER_NONE &&
				         op[0].mem.disp.has_displacement)  // x86 absolute
					g = reinterpret_cast<void **>(static_cast<uintptr_t>(
					    static_cast<uint32_t>(op[0].mem.disp.value)));

				if (g && g == global && last_call)
					return last_call;
			}
			p += in.length;
		}
		return nullptr;
	}

	int x64_argnum_liveness(const void *begin, const void *end, int max_insns)
	{
		if (!begin || !end)
			return 0;

		ZydisDecoder dec;
		ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64,
		                 ZYDIS_STACK_WIDTH_64);

		static constexpr int kArgReg[4] = {1 /*RCX*/, 2 /*RDX*/, 8 /*R8*/,
		                                   9 /*R9*/};

		bool live[4] = {};
		bool dead[4] = {};

		const uint8_t *p = u8(begin), *e = u8(end);
		ZydisDecodedInstruction in;
		ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];

		for (int i = 0; i < max_insns && p < e; ++i)
		{
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec, p, e - p, &in, ops)))
				break;

			if (is_mnemonic(in, {ZYDIS_MNEMONIC_CALL, ZYDIS_MNEMONIC_RET,
			                     ZYDIS_MNEMONIC_JMP}))
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
					if (op.type == ZYDIS_OPERAND_TYPE_REGISTER)
					{
						if (!(op.actions & ZYDIS_OPERAND_ACTION_MASK_READ))
							continue;
						const int gi = dxa::gpr_idx(op.reg.value);
						for (int a = 0; a < 4; ++a)
							if (gi == kArgReg[a] && !dead[a])
								live[a] = true;
					}
					else if (op.type == ZYDIS_OPERAND_TYPE_MEMORY)
					{
						for (ZydisRegister r : {op.mem.base, op.mem.index})
						{
							const int gi = dxa::gpr_idx(r);
							for (int a = 0; a < 4; ++a)
								if (gi == kArgReg[a] && !dead[a])
									live[a] = true;
						}
					}
				}
			}

			for (int oi = 0; oi < in.operand_count; ++oi)
			{
				const auto &op = ops[oi];
				if (op.type != ZYDIS_OPERAND_TYPE_REGISTER)
					continue;
				if (!(op.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE))
					continue;
				const int gi = dxa::gpr_idx(op.reg.value);
				for (int a = 0; a < 4; ++a)
					if (gi == kArgReg[a])
						dead[a] = true;
			}

			p += in.length;
		}

		int argnum = 0;
		for (int a = 0; a < 4; ++a)
			if (live[a])
				argnum = a + 1;
		return argnum;
	}
}  // namespace dx
