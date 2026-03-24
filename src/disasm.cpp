#include "disasm.hpp"
#include "logs.hpp"
#include <Zydis/Zydis.h>

static ZydisDecoder &decoder() {
	static ZydisDecoder d = [] {
		ZydisDecoder d2;
		ZydisDecoderInit(&d2, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
		return d2;
	}();
	return d;
}

int find_fpf_vtable_slot(void **vtbl, int max_slots, uintptr_t exe_base,
                         size_t exe_size, int fallback_slot) {
	ZydisDecodedInstruction insn;
	ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];

	for (int i = 0; i < max_slots; ++i) {
		auto fn = reinterpret_cast<uintptr_t>(vtbl[i]);
		if (fn < exe_base || fn >= exe_base + exe_size)
			continue;

		const auto *p = reinterpret_cast<const uint8_t *>(fn);
		size_t off = 0;
		bool has_sub = false;
		bool has_shadow = false;

		for (int j = 0; j < 8 && off < 64; ++j) {
			ZyanStatus st =
			    ZydisDecoderDecodeFull(&decoder(), p + off, 15, &insn, ops);
			if (!ZYAN_SUCCESS(st))
				break;

			if (insn.mnemonic == ZYDIS_MNEMONIC_SUB &&
			    ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
			    ops[0].reg.value == ZYDIS_REGISTER_RSP)
				has_sub = true;

			if (insn.mnemonic == ZYDIS_MNEMONIC_MOV &&
			    ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
			    ops[0].mem.base == ZYDIS_REGISTER_RSP &&
			    ops[0].mem.disp.has_displacement && ops[0].mem.disp.value > 0 &&
			    ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER)
				has_shadow = true;

			off += insn.length;
		}

		if (has_sub && has_shadow) {
			log_info("disasm: FindPackageFile candidate vtbl[%d] = %p", i,
			         reinterpret_cast<void *>(fn));
			return i;
		}
	}

	log_warn("disasm: no FindPackageFile match in %d slots, "
	         "falling back to slot %d",
	         max_slots, fallback_slot);
	return fallback_slot;
}
