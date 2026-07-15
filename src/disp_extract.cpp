#include "disp_extract.hpp"
#include <Zydis/Zydis.h>
#include <algorithm>
#include <cstdint>

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

	static int gpr_idx(ZydisRegister reg);

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
			    in.operand_count_visible >= 2 &&
			    ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
			    ops[1].mem.base != ZYDIS_REGISTER_NONE &&
			    ops[1].mem.base != ZYDIS_REGISTER_RIP &&
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
					if (ops[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
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

		bool have_store = false;
		const uint8_t *store_ip = nullptr;
		uint32_t store_len = 0;
		int32_t store_disp = 0;
		int insns_since_store = 0;

		while (p < e)
		{
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec.d, p, e - p, &in, ops)))
				break;

			if (in.mnemonic == ZYDIS_MNEMONIC_MOV &&
			    in.operand_count_visible >= 2 &&
			    ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
			    ops[0].mem.base == ZYDIS_REGISTER_RIP &&
			    ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER)
			{
				have_store = true;
				store_ip = p;
				store_len = in.length;
				store_disp = static_cast<int32_t>(ops[0].mem.disp.value);
				insns_since_store = 0;
			}
			else if (have_store)
			{
				++insns_since_store;
				if (insns_since_store > call_window)
					have_store = false;
			}

			if (have_store && in.mnemonic == ZYDIS_MNEMONIC_CALL &&
			    in.operand_count_visible == 1 &&
			    ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
			    ops[0].mem.base != ZYDIS_REGISTER_NONE &&
			    ops[0].mem.base != ZYDIS_REGISTER_RIP &&
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
			if ((in.mnemonic == ZYDIS_MNEMONIC_MOV ||
			     in.mnemonic == ZYDIS_MNEMONIC_LEA) &&
			    in.operand_count_visible >= 2 &&
			    ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
			    ops[1].mem.base == ZYDIS_REGISTER_RIP &&
			    ops[1].mem.disp.has_displacement)
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
			if ((in.mnemonic == ZYDIS_MNEMONIC_MOV ||
			     in.mnemonic == ZYDIS_MNEMONIC_LEA) &&
			    in.operand_count_visible >= 2 &&
			    ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
			    ops[1].mem.base == ZYDIS_REGISTER_RIP &&
			    ops[1].mem.disp.has_displacement)
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
				    ops2[1].type == ZYDIS_OPERAND_TYPE_REGISTER &&
				    ops2[1].reg.value == ZYDIS_REGISTER_RSP)
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
			    in.operand_count_visible == 1 &&
			    ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
			    ops[0].mem.base != ZYDIS_REGISTER_NONE &&
			    ops[0].mem.base != ZYDIS_REGISTER_RIP &&
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
				if (ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER &&
				    (ops[i].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE))
				{
					int gi = gpr_idx(ops[i].reg.value);
					if (gi >= 0)
						reg_global[gi] = nullptr;
				}

			if (in.mnemonic == ZYDIS_MNEMONIC_MOV &&
			    in.operand_count_visible >= 2 &&
			    ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
			    ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
			    ops[1].mem.base == ZYDIS_REGISTER_RIP &&
			    ops[1].mem.disp.has_displacement)
			{
				int gi = gpr_idx(ops[0].reg.value);
				if (gi >= 0)
				{
					const uint8_t *next = p + in.length;
					reg_global[gi] = reinterpret_cast<void **>(
					    const_cast<uint8_t *>(next) +
					    static_cast<int32_t>(ops[1].mem.disp.value));
				}
			}
			else if (in.operand_count_visible >= 2 &&
			         ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
			         ops[0].mem.base != ZYDIS_REGISTER_NONE &&
			         ops[0].mem.base != ZYDIS_REGISTER_RIP &&
			         ops[0].mem.index != ZYDIS_REGISTER_NONE &&
			         ops[0].mem.scale == scale)
			{
				int gi = gpr_idx(ops[0].mem.base);
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

	static int gpr_idx(ZydisRegister reg)
	{
		ZydisRegister enc = ZydisRegisterGetLargestEnclosing(
		    sizeof(void *) == 8 ? ZYDIS_MACHINE_MODE_LONG_64
		                        : ZYDIS_MACHINE_MODE_LEGACY_32,
		    reg);
		if (enc >= ZYDIS_REGISTER_RAX && enc <= ZYDIS_REGISTER_R15)
			return static_cast<int>(enc - ZYDIS_REGISTER_RAX);
		return -1;
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
				if (ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER &&
				    (ops[i].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE))
				{
					int gi = gpr_idx(ops[i].reg.value);
					if (gi >= 0)
						load_disp[gi] = NONE;
				}

			if (in.mnemonic == ZYDIS_MNEMONIC_MOV &&
			    in.operand_count_visible >= 2 &&
			    ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
			    ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
			    ops[1].mem.base != ZYDIS_REGISTER_NONE &&
			    ops[1].mem.base != ZYDIS_REGISTER_RIP &&
			    ops[1].mem.index == ZYDIS_REGISTER_NONE &&
			    ops[1].mem.disp.has_displacement)
			{
				int gi = gpr_idx(ops[0].reg.value);
				if (gi >= 0)
					load_disp[gi] = ops[1].mem.disp.value;
			}

			if (in.mnemonic == ZYDIS_MNEMONIC_IMUL &&
			    ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER)
			{
				for (int i = 1; i < in.operand_count_visible; ++i)
					if (ops[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
					    ops[i].imm.value.s == stride)
					{
						idx_reg = ops[0].reg.value;
						break;
					}
			}
			else if (idx_reg != ZYDIS_REGISTER_NONE &&
			         (in.mnemonic == ZYDIS_MNEMONIC_ADD ||
			          in.mnemonic == ZYDIS_MNEMONIC_LEA) &&
			         ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
			         gpr_idx(ops[0].reg.value) == gpr_idx(idx_reg) &&
			         in.operand_count_visible >= 2)
			{
				if (ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
				    ops[1].mem.base != ZYDIS_REGISTER_NONE &&
				    ops[1].mem.base != ZYDIS_REGISTER_RIP &&
				    ops[1].mem.disp.has_displacement)
				{
					out_disp = ops[1].mem.disp.value;
					return true;
				}
				if (in.mnemonic == ZYDIS_MNEMONIC_ADD &&
				    ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER)
				{
					int s = gpr_idx(ops[1].reg.value);
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
				if (ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER &&
				    (ops[i].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE))
				{
					int gi = gpr_idx(ops[i].reg.value);
					if (gi >= 0)
					{
						field_disp[gi] = NONE;
						vt_field_disp[gi] = NONE;
					}
				}

			if (in.mnemonic == ZYDIS_MNEMONIC_MOV &&
			    in.operand_count_visible >= 2 &&
			    ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
			    ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY)
			{
				const int dst = gpr_idx(ops[0].reg.value);
				if (dst < 0)
				{
					p += in.length;
					continue;
				}

				if (ops[1].mem.base != ZYDIS_REGISTER_NONE &&
				    ops[1].mem.base != ZYDIS_REGISTER_RIP &&
				    ops[1].mem.index == ZYDIS_REGISTER_NONE)
				{
					const int base = gpr_idx(ops[1].mem.base);
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
			         in.operand_count_visible == 1 &&
			         ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
			         ops[0].mem.base != ZYDIS_REGISTER_NONE &&
			         ops[0].mem.base != ZYDIS_REGISTER_RIP &&
			         ops[0].mem.disp.has_displacement)
			{
				const int vbase = gpr_idx(ops[0].mem.base);
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
		int since_lea = 99;
		while (p < e)
		{
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec.d, p, e - p, &in, ops)))
				break;
			if (in.mnemonic == ZYDIS_MNEMONIC_LEA &&
			    in.operand_count_visible >= 2 &&
			    ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
			    ops[1].mem.base != ZYDIS_REGISTER_NONE &&
			    ops[1].mem.base != ZYDIS_REGISTER_RIP &&
			    ops[1].mem.index == ZYDIS_REGISTER_NONE &&
			    ops[1].mem.disp.has_displacement &&
			    ops[1].mem.disp.value == field_disp)
			{
				since_lea = 0;
			}
			else if (in.mnemonic == ZYDIS_MNEMONIC_CALL &&
			         in.operand_count_visible >= 1 &&
			         ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
			         since_lea <= 3)
			{
				const uint8_t *next = p + in.length;
				out.push_back(const_cast<uint8_t *>(next) +
				              static_cast<int32_t>(ops[0].imm.value.s));
				since_lea = 99;
			}
			else if (since_lea < 99)
				++since_lea;
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
		int since_imm = window + 1;
		while (p < e)
		{
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec.d, p, e - p, &in, ops)))
				break;

			if (in.mnemonic == ZYDIS_MNEMONIC_CALL &&
			    in.operand_count_visible >= 1 &&
			    ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
			    since_imm <= window)
				return true;

			bool imm_load = false;
			if (in.mnemonic == ZYDIS_MNEMONIC_PUSH ||
			    in.mnemonic == ZYDIS_MNEMONIC_MOV)
				for (int i = 0; i < in.operand_count_visible; ++i)
					if (ops[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
					    ops[i].imm.value.u == static_cast<uint64_t>(imm))
					{
						imm_load = true;
						break;
					}

			if (imm_load)
				since_imm = 0;
			else if (since_imm <= window)
				++since_imm;
			p += in.length;
		}
		return false;
	}

	void *find_split_name_setup(const void *begin, const void *end)
	{
		//   x86: PUSH 0x400 ; LEA rA,[ESP+disp] ; PUSH rA ; PUSH rB ; CALL
		//   x64: LEA rX,[RSP+disp] ; MOV r32,0x400 ; ... ; CALL
		Dec dec;
		const uint8_t *p = u8(begin), *e = u8(end);
		ZydisDecodedInstruction in;
		ZydisDecodedOperand op[ZYDIS_MAX_OPERAND_COUNT];

		const int W = 6;
		int lea_ago = W + 1, imm_ago = W + 1, this_ago = W + 1;

		while (p < e)
		{
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec.d, p, e - p, &in, op)))
			{
				++p;
				lea_ago = imm_ago = this_ago = W + 1;
				continue;
			}

			if (in.mnemonic == ZYDIS_MNEMONIC_CALL &&
			    in.operand_count_visible >= 1 &&
			    op[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
			{
				if (sizeof(void *) == 8)  // x64
				{
					if (imm_ago <= W && this_ago <= W)
						return const_cast<uint8_t *>(p);
				}
				else  // x86
				{
					if (lea_ago <= W && imm_ago <= W && this_ago <= W)
						return const_cast<uint8_t *>(p);
				}
			}

			if (in.mnemonic == ZYDIS_MNEMONIC_CALL ||
			    in.mnemonic == ZYDIS_MNEMONIC_RET ||
			    in.mnemonic == ZYDIS_MNEMONIC_JMP)
			{
				lea_ago = imm_ago = this_ago = W + 1;
				p += in.length;
				continue;
			}

			if (lea_ago <= W)
				++lea_ago;
			if (imm_ago <= W)
				++imm_ago;
			if (this_ago <= W)
				++this_ago;

			if (sizeof(void *) == 8)  // x64
			{
				// MOV r32/64, 0x400
				if (in.mnemonic == ZYDIS_MNEMONIC_MOV &&
				    op[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
				    op[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
				    op[1].imm.value.u == 0x400)
				{
					imm_ago = 0;
				}
				// MOV RCX, RSI
				else if (in.mnemonic == ZYDIS_MNEMONIC_MOV &&
				         op[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
				         op[1].type == ZYDIS_OPERAND_TYPE_REGISTER &&
				         op[0].reg.value == ZYDIS_REGISTER_RCX &&
				         op[1].reg.value == ZYDIS_REGISTER_RSI)
				{
					this_ago = 0;
				}
			}
			else  // x86
			{
				// PUSH 0x400  (size)
				if (in.mnemonic == ZYDIS_MNEMONIC_PUSH &&
				    op[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
				    op[0].imm.value.u == 0x400)
				{
					imm_ago = 0;
				}
				// LEA r32, [ESP + disp]
				else if (in.mnemonic == ZYDIS_MNEMONIC_LEA &&
				         op[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
				         op[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
				         op[1].mem.base == ZYDIS_REGISTER_ESP &&
				         op[1].mem.index == ZYDIS_REGISTER_NONE)
				{
					lea_ago = 0;
				}
				// PUSH r32
				else if (in.mnemonic == ZYDIS_MNEMONIC_PUSH &&
				         op[0].type == ZYDIS_OPERAND_TYPE_REGISTER)
				{
					this_ago = 0;
				}
			}

			p += in.length;
		}
		return nullptr;
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

		while (p < e)
		{
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec.d, p, e - p, &in, op)))
				break;

			if (in.mnemonic == ZYDIS_MNEMONIC_CALL ||
			    in.mnemonic == ZYDIS_MNEMONIC_RET ||
			    in.mnemonic == ZYDIS_MNEMONIC_JMP)
			{
				if (have_pair && in.mnemonic == ZYDIS_MNEMONIC_JMP)
					return true;

				have_lo = have_pair = false;
				base = -1;
				sreg = ZYDIS_REGISTER_NONE;
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
				const int b = gpr_idx(op[0].mem.base);

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
					if (b >= 0 && op[1].type == ZYDIS_OPERAND_TYPE_REGISTER)
					{
						if (disp == 0)
						{
							have_lo = true;
							base = b;
							sreg = op[1].reg.value;
							consumed = true;
						}
						else if (disp == 4 && have_lo && b == base &&
						         op[1].reg.value == sreg)
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
			    in.operand_count_visible >= 1 &&
			    op[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
			{
				const uint8_t *next = p + in.length;
				last_call = const_cast<uint8_t *>(next) +
				            static_cast<int32_t>(op[0].imm.value.s);
			}
			else if (in.mnemonic == ZYDIS_MNEMONIC_MOV &&
			         in.operand_count_visible >= 2 &&
			         op[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
			         op[1].type == ZYDIS_OPERAND_TYPE_REGISTER)
			{
				void **g = nullptr;
				if (op[0].mem.base == ZYDIS_REGISTER_RIP &&
				    op[0].mem.disp.has_displacement)  // x64 rip-relative
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
}  // namespace dx
