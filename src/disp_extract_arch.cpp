#include "disp_extract_arch.hpp"
#include <Zydis/Zydis.h>
#include <cstdint>

namespace dxa
{
	namespace
	{
		constexpr int64_t SENT = INT64_MIN;
		const int PS = static_cast<int>(sizeof(void *));  // 4 or 8

		ZydisDecoder make_decoder()
		{
			ZydisDecoder d;
			ZydisDecoderInit(&d,
			                 PS == 8 ? ZYDIS_MACHINE_MODE_LONG_64
			                         : ZYDIS_MACHINE_MODE_LEGACY_32,
			                 PS == 8 ? ZYDIS_STACK_WIDTH_64
			                         : ZYDIS_STACK_WIDTH_32);
			return d;
		}

		int gpr_idx(ZydisRegister reg)
		{
			ZydisRegister enc = ZydisRegisterGetLargestEnclosing(
			    PS == 8 ? ZYDIS_MACHINE_MODE_LONG_64
			            : ZYDIS_MACHINE_MODE_LEGACY_32,
			    reg);
			if (enc >= ZYDIS_REGISTER_RAX && enc <= ZYDIS_REGISTER_R15)
				return static_cast<int>(enc - ZYDIS_REGISTER_RAX);
			return -1;
		}

		bool global_from_mem(const ZydisDecodedInstruction &in,
		                     const ZydisDecodedOperand &op, const uint8_t *ip,
		                     void **&out)
		{
			if (op.type != ZYDIS_OPERAND_TYPE_MEMORY)
				return false;
			if (op.mem.base == ZYDIS_REGISTER_RIP &&
			    op.mem.disp.has_displacement)
			{
				out = reinterpret_cast<void **>(
				    const_cast<uint8_t *>(ip) + in.length +
				    static_cast<int32_t>(op.mem.disp.value));
				return true;
			}
			if (op.mem.base == ZYDIS_REGISTER_NONE &&
			    op.mem.index == ZYDIS_REGISTER_NONE &&
			    op.mem.disp.has_displacement)
			{
				uintptr_t va =
				    PS == 4 ? static_cast<uintptr_t>(
				                  static_cast<uint32_t>(op.mem.disp.value))
				            : static_cast<uintptr_t>(op.mem.disp.value);
				out = reinterpret_cast<void **>(va);
				return true;
			}
			return false;
		}

		enum Mode
		{
			GSO,
			GPFC,
			FIELD_SLOT,
			INDEXED
		};

		enum
		{
			PV_NONE = 0,
			PV_GLOB,
			PV_FIELD,
			PV_OBJ
		};

		bool scan(const void *begin, const void *end, Mode mode, int slot_index,
		          int scale, void **&outg, ptrdiff_t &outv, int window)
		{
			bool is_64bit = sizeof(void *) == 8;
			ZydisDecoder dec = make_decoder();
			const uint8_t *p = static_cast<const uint8_t *>(begin);
			const uint8_t *e = static_cast<const uint8_t *>(end);
			ZydisDecodedInstruction in;
			ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];

			void *g_from[16];
			int64_t fld_off[16];
			int vtKind[16];
			void *vtGlob[16];
			int64_t vtFld[16];
			int fpKind[16];
			int64_t fpOff[16];
			void *fpGlob[16];
			int64_t fpFld[16];

			int PS = is_64bit ? 8 : 4;

			auto clear = [&](int r)
			{
				g_from[r] = nullptr;
				fld_off[r] = SENT;
				vtKind[r] = PV_NONE;
				vtGlob[r] = nullptr;
				vtFld[r] = SENT;
				fpKind[r] = PV_NONE;
				fpOff[r] = SENT;
				fpGlob[r] = nullptr;
				fpFld[r] = SENT;
			};
			for (int i = 0; i < 16; ++i)
				clear(i);
			void *pending_gso = nullptr;
			int pending_ctr = 0;

			while (p < e)
			{
				if (ZYAN_FAILED(
				        ZydisDecoderDecodeFull(&dec, p, e - p, &in, ops)))
					break;

				if (pending_ctr > 0 && --pending_ctr == 0)
					pending_gso = nullptr;

				for (int i = 0; i < in.operand_count; ++i)
					if (ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER &&
					    (ops[i].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE))
					{
						int gi = gpr_idx(ops[i].reg.value);
						if (gi >= 0)
							clear(gi);
					}

				if (in.mnemonic == ZYDIS_MNEMONIC_MOV &&
				    in.operand_count_visible >= 2 &&
				    ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER)
				{
					void **g = nullptr;
					if (global_from_mem(in, ops[0], p, g))
					{
						pending_gso = g;
						pending_ctr = window;
					}
				}

				if (mode == INDEXED && in.operand_count_visible >= 1 &&
				    ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
				    ops[0].mem.base != ZYDIS_REGISTER_NONE &&
				    ops[0].mem.base != ZYDIS_REGISTER_RIP &&
				    ops[0].mem.index != ZYDIS_REGISTER_NONE &&
				    ops[0].mem.scale == scale)
				{
					int b = gpr_idx(ops[0].mem.base);
					if (b >= 0 && g_from[b])
					{
						outg = reinterpret_cast<void **>(g_from[b]);
						return true;
					}
				}

				if (in.mnemonic == ZYDIS_MNEMONIC_MOV &&
				    in.operand_count_visible >= 2 &&
				    ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
				    ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY)
				{
					int dst = gpr_idx(ops[0].reg.value);
					if (dst >= 0)
					{
						void **g = nullptr;
						if (global_from_mem(in, ops[1], p, g))
						{
							g_from[dst] = reinterpret_cast<void *>(g);
						}
						else if (!is_64bit &&
						         ops[1].mem.base == ZYDIS_REGISTER_NONE &&
						         ops[1].mem.index == ZYDIS_REGISTER_NONE &&
						         ops[1].mem.disp.has_displacement)
						{
							void *abs_glob = reinterpret_cast<void *>(
							    static_cast<uintptr_t>(ops[1].mem.disp.value));
							g_from[dst] = abs_glob;
							vtKind[dst] = PV_GLOB;
							vtGlob[dst] = abs_glob;
						}
						else if (ops[1].mem.base != ZYDIS_REGISTER_NONE &&
						         ops[1].mem.base != ZYDIS_REGISTER_RIP &&
						         ops[1].mem.index == ZYDIS_REGISTER_NONE)
						{
							int b = gpr_idx(ops[1].mem.base);
							int64_t disp = ops[1].mem.disp.has_displacement
							                   ? ops[1].mem.disp.value
							                   : 0;
							if (b >= 0)
							{
								if (disp == 0)
								{
									if (g_from[b])
									{
										vtKind[dst] = PV_GLOB;
										vtGlob[dst] = g_from[b];
									}
									else if (fld_off[b] != SENT)
									{
										vtKind[dst] = PV_FIELD;
										vtFld[dst] = fld_off[b];
									}
									else
									{
										vtKind[dst] = PV_OBJ;
									}
								}
								else if (vtKind[b] != PV_NONE)
								{
									fpKind[dst] = vtKind[b];
									fpOff[dst] = disp;
									fpGlob[dst] = vtGlob[b];
									fpFld[dst] = vtFld[b];
								}
								else
								{
									fld_off[dst] = disp;
								}
							}
						}
					}
				}

				int ck = PV_NONE;
				int64_t coff = 0;
				void *cg = nullptr;
				int64_t cf = SENT;

				if (in.mnemonic == ZYDIS_MNEMONIC_CALL &&
				    in.operand_count_visible >= 1)
				{
					if (ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY)
					{
						if (ops[0].mem.base != ZYDIS_REGISTER_NONE &&
						    ops[0].mem.base != ZYDIS_REGISTER_RIP &&
						    ops[0].mem.index == ZYDIS_REGISTER_NONE &&
						    ops[0].mem.disp.has_displacement)
						{
							int b = gpr_idx(ops[0].mem.base);
							if (b >= 0 && vtKind[b] != PV_NONE)
							{
								ck = vtKind[b];
								coff = ops[0].mem.disp.value;
								cg = vtGlob[b];
								cf = vtFld[b];
							}
						}
						else if (!is_64bit &&
						         ops[0].mem.base == ZYDIS_REGISTER_NONE &&
						         ops[0].mem.index == ZYDIS_REGISTER_NONE &&
						         ops[0].mem.disp.has_displacement)
						{
							ck = PV_GLOB;
							coff = 0;
							cg = reinterpret_cast<void *>(
							    static_cast<uintptr_t>(ops[0].mem.disp.value));
							cf = SENT;
						}
					}
					else if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER)
					{
						int rt = gpr_idx(ops[0].reg.value);
						if (rt >= 0 && fpKind[rt] != PV_NONE)
						{
							ck = fpKind[rt];
							coff = fpOff[rt];
							cg = fpGlob[rt];
							cf = fpFld[rt];
						}
					}
				}

				if (ck != PV_NONE)
				{
					if (mode == GSO && pending_gso)
					{
						outg = reinterpret_cast<void **>(pending_gso);
						outv = static_cast<ptrdiff_t>(coff);
						return true;
					}
					if (mode == GPFC && ck == PV_GLOB)
					{
						outg = reinterpret_cast<void **>(cg);
						return true;
					}
					if (mode == FIELD_SLOT && ck == PV_FIELD &&
					    coff == static_cast<int64_t>(slot_index) * PS)
					{
						outv = static_cast<ptrdiff_t>(cf);
						return true;
					}
				}

				p += in.length;
			}
			return false;
		}
	}  // namespace

	bool serialized_object_and_serialize(const void *b, const void *e,
	                                     void **&outg, ptrdiff_t &outv,
	                                     int window)
	{
		return scan(b, e, GSO, 0, 0, outg, outv, window);
	}

	bool gpackagefilecache(const void *b, const void *e, void **&outg)
	{
		ptrdiff_t dummy = 0;
		return scan(b, e, GPFC, 0, 0, outg, dummy, 10);
	}

	bool field_off_for_vslot(const void *b, const void *e, int slot_index,
	                         ptrdiff_t &out_off)
	{
		void **dummy = nullptr;
		return scan(b, e, FIELD_SLOT, slot_index, 0, dummy, out_off, 0);
	}

	bool indexed_store_global(const void *b, const void *e, void **&outg,
	                          int scale)
	{
		ptrdiff_t dummy = 0;
		return scan(b, e, INDEXED, 0, scale, outg, dummy, 0);
	}

	bool load_and_indexed_store_to_global(const void *b, const void *e,
	                                      void **global_ptr, int scale)
	{
		if (!global_ptr)
			return false;

		ZydisDecoder dec = make_decoder();
		const uint8_t *p = static_cast<const uint8_t *>(b);
		const uint8_t *end = static_cast<const uint8_t *>(e);
		ZydisDecodedInstruction in;
		ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];

		bool holds_data_ptr[16] = {};

		while (p < end)
		{
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec, p, end - p, &in, ops)))
				break;

			for (int i = 0; i < in.operand_count; ++i)
			{
				if (ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER &&
				    (ops[i].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE))
				{
					int gi = gpr_idx(ops[i].reg.value);
					if (gi >= 0)
						holds_data_ptr[gi] = false;
				}
			}

			if (in.mnemonic == ZYDIS_MNEMONIC_MOV &&
			    in.operand_count_visible >= 2 &&
			    ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
			    ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY)
			{
				int dst = gpr_idx(ops[0].reg.value);
				if (dst >= 0)
				{
					void **g = nullptr;
					if (global_from_mem(in, ops[1], p, g) && g == global_ptr)
					{
						holds_data_ptr[dst] = true;
					}
				}
			}

			if (in.operand_count_visible >= 2 &&
			    ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
			    ops[0].mem.base != ZYDIS_REGISTER_NONE &&
			    ops[0].mem.base != ZYDIS_REGISTER_RIP &&
			    ops[0].mem.index != ZYDIS_REGISTER_NONE &&
			    ops[0].mem.scale == scale)
			{
				int base_reg = gpr_idx(ops[0].mem.base);
				if (base_reg >= 0 && holds_data_ptr[base_reg])
					return true;
			}

			p += in.length;
		}
		return false;
	}
}  // namespace dxa
