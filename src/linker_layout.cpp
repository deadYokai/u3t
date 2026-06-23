#define WIN32_LEAN_AND_MEAN
#include "linker_layout.hpp"
#include "logs.hpp"
#include "utypes/ULinker.hpp"
#include <Zydis/Zydis.h>
#include <cstring>
#include <windows.h>

static constexpr size_t kScanWindow = 1024;
static constexpr size_t kCallWindow = 8;
static constexpr uint32_t kMaxVtSlot = 64;

static bool scan_preload_serialize(void *preload_fn, int &out_vtslot,
                                   void **&out_gserial)
{
	if (!preload_fn || IsBadReadPtr(preload_fn, kScanWindow))
		return false;

	ZydisDecoder dec;
	ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

	const auto *base = static_cast<const uint8_t *>(preload_fn);
	size_t off = 0;

	struct IRec
	{
		size_t ip;
		ZydisDecodedInstruction ins;
		ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
	};

	static constexpr size_t kWin = kCallWindow + 2;
	IRec win[kWin];
	int wn = 0;

	size_t last_rip_mov_ip = 0;
	int32_t last_rip_mov_rel = 0;
	uint32_t last_rip_mov_len = 0;
	bool have_rip_mov = false;

	while (off < kScanWindow)
	{
		IRec r;
		r.ip = reinterpret_cast<size_t>(base + off);

		ZyanStatus st = ZydisDecoderDecodeFull(
		    &dec, base + off, kScanWindow - off, &r.ins, r.ops);
		if (ZYAN_FAILED(st))
			break;

		const uint32_t len = r.ins.length;
		const uint64_t ip = r.ip;

		if (r.ins.mnemonic == ZYDIS_MNEMONIC_RET)
			break;

		if (r.ins.mnemonic == ZYDIS_MNEMONIC_MOV &&
		    r.ins.operand_count_visible >= 2 &&
		    r.ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
		    r.ops[0].mem.base == ZYDIS_REGISTER_RIP &&
		    r.ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER)
		{
			have_rip_mov = true;
			last_rip_mov_ip = ip;
			last_rip_mov_rel = static_cast<int32_t>(r.ops[0].mem.disp.value);
			last_rip_mov_len = len;
		}

		if (have_rip_mov && r.ins.mnemonic == ZYDIS_MNEMONIC_CALL &&
		    r.ins.operand_count_visible == 1 &&
		    r.ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
		    r.ops[0].mem.base != ZYDIS_REGISTER_NONE &&
		    r.ops[0].mem.base != ZYDIS_REGISTER_RIP &&
		    r.ops[0].mem.disp.has_displacement)
		{
			const int64_t disp = r.ops[0].mem.disp.value;

			if (disp >= 0 && disp % 8 == 0 &&
			    disp < static_cast<int64_t>(kMaxVtSlot * 8))
			{

				const size_t dist = ip - last_rip_mov_ip;
				if (dist < kCallWindow * 15)
				{
					out_vtslot = static_cast<int>(disp / 8);

					const size_t next_ip = last_rip_mov_ip + last_rip_mov_len;
					const auto gaddr = static_cast<uintptr_t>(
					    static_cast<intptr_t>(next_ip) + last_rip_mov_rel);
					out_gserial = reinterpret_cast<void **>(gaddr);

					log_info("linker_layout: UObject::Serialize slot=%d "
					         "GSerializedObject=%p  (from Preload+0x%zx)",
					         out_vtslot, static_cast<void *>(out_gserial), off);
					return true;
				}
			}
		}

		off += len;
		(void)wn;
	}

	log_warn("linker_layout: could not locate UObject::Serialize vtable slot "
	         "in Preload — full port will call g_orig_Preload for Serialize");
	return false;
}

static LinkerLayout g_layout;

const LinkerLayout &linker_layout() { return g_layout; }

bool resolve_linker_layout(LinkerLayout &out, void * /*fname_init_unused*/,
                           void *preload_fn)
{

	out.farchive_off = kLinker_FArchiveOff;
	out.fname_slot = FArchiveVtSlot::SerializeFName;
	out.nm_data_off = kLinker_NameMap_Data;
	out.nm_count_off = kLinker_NameMap_Num;

	if (preload_fn)
	{
		scan_preload_serialize(preload_fn, out.uobj_serialize_vtslot,
		                       out.g_serialized_obj);
	}
	else
	{
		log_info("linker_layout: no preload_fn supplied — "
		         "Serialize slot scan skipped");
	}

	out.valid = true;
	g_layout = out;

	log_info("linker_layout: farchive_off=0x%tx  fname_slot=%d  "
	         "nm_data_off=0x%tx  nm_count_off=0x%tx  "
	         "serialize_slot=%d  g_serialized_obj=%p",
	         out.farchive_off, out.fname_slot, out.nm_data_off,
	         out.nm_count_off, out.uobj_serialize_vtslot,
	         static_cast<void *>(out.g_serialized_obj));
	return true;
}
