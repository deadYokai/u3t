#define WIN32_LEAN_AND_MEAN
#include "trampoline_hook.hpp"
#include "logs.hpp"
#include <Zydis/Zydis.h>
#include <cstring>
#include <windows.h>

static ZydisDecoder &g_dec() {
	static ZydisDecoder d = [] {
		ZydisDecoder d2;
		ZydisDecoderInit(&d2, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
		return d2;
	}();
	return d;
}

static void write_abs_jmp(uint8_t *dst, void *target) {
	dst[0] = 0xFF;
	dst[1] = 0x25;
	*reinterpret_cast<uint32_t *>(dst + 2) = 0;
	*reinterpret_cast<uint64_t *>(dst + 6) = reinterpret_cast<uint64_t>(target);
}

static void *alloc_near(void *near_addr, size_t size) {
	SYSTEM_INFO si{};
	GetSystemInfo(&si);
	const auto gran = static_cast<int64_t>(si.dwAllocationGranularity);
	const int64_t MAX = 0x7FF00000LL;
	const int64_t base = reinterpret_cast<int64_t>(near_addr);
	const int64_t lo =
	    reinterpret_cast<int64_t>(si.lpMinimumApplicationAddress);
	const int64_t hi =
	    reinterpret_cast<int64_t>(si.lpMaximumApplicationAddress);

	for (int64_t off = 0; off < MAX; off += gran * 16) {
		for (int sign : {1, -1}) {
			int64_t try_addr = base + sign * off;
			if (try_addr < lo || try_addr > hi)
				continue;
			void *p =
			    VirtualAlloc(reinterpret_cast<void *>(try_addr), size,
			                 MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
			if (p)
				return p;
		}
	}
	return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE,
	                    PAGE_EXECUTE_READWRITE);
}

bool trampoline_install(TrampolineHook *hook, void *target, void *detour) {
	if (!hook || !target || !detour)
		return false;

	ZydisDecodedInstruction insn{};
	ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT]{};
	const auto *src = static_cast<const uint8_t *>(target);

	size_t total = 0;
	bool has_rip = false;

	while (total < kJmpStubSize) {
		ZyanStatus st =
		    ZydisDecoderDecodeFull(&g_dec(), src + total, 15, &insn, ops);
		if (!ZYAN_SUCCESS(st)) {
			log_err("trampoline: decode failed at %p+%zu", target, total);
			return false;
		}

		for (uint8_t i = 0; i < insn.operand_count; ++i) {
			if (ops[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
			    ops[i].imm.is_relative) {
				log_err("trampoline: relative branch at %p+%zu — cannot hook",
				        target, total);
				return false;
			}
		}

		for (uint8_t i = 0; i < insn.operand_count; ++i) {
			if (ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY &&
			    ops[i].mem.base == ZYDIS_REGISTER_RIP) {
				has_rip = true;
				break;
			}
		}

		total += insn.length;
		if (total > kMaxStolenSize) {
			log_err("trampoline: prologue at %p is too large (%zu > %zu)",
			        target, total, kMaxStolenSize);
			return false;
		}
	}

	const size_t tramp_cap = total + kJmpStubSize;
	auto *tramp = static_cast<uint8_t *>(alloc_near(target, tramp_cap));
	if (!tramp) {
		log_err("trampoline: alloc_near failed for %p", target);
		return false;
	}

	if (has_rip) {
		size_t src_off = 0, dst_off = 0;
		while (src_off < total) {
			ZydisDecoderDecodeFull(&g_dec(), src + src_off, 15, &insn, ops);
			memcpy(tramp + dst_off, src + src_off, insn.length);

			for (uint8_t i = 0; i < insn.operand_count; ++i) {
				if (ops[i].type != ZYDIS_OPERAND_TYPE_MEMORY ||
				    ops[i].mem.base != ZYDIS_REGISTER_RIP)
					continue;

				const int64_t abs_ref =
				    reinterpret_cast<int64_t>(src + src_off + insn.length) +
				    static_cast<int64_t>(ops[i].mem.disp.value);

				const int64_t new_disp =
				    abs_ref -
				    reinterpret_cast<int64_t>(tramp + dst_off + insn.length);

				if (new_disp < INT32_MIN || new_disp > INT32_MAX) {
					log_err("trampoline: RIP fixup out of ±2 GB at %p+%zu",
					        target, src_off);
					VirtualFree(tramp, 0, MEM_RELEASE);
					return false;
				}

				*reinterpret_cast<int32_t *>(tramp + dst_off +
				                             insn.raw.disp.offset) =
				    static_cast<int32_t>(new_disp);

				log_info("trampoline: RIP fixup +%zu disp %08X -> %08X",
				         src_off, static_cast<uint32_t>(ops[i].mem.disp.value),
				         static_cast<uint32_t>(new_disp));
				break;
			}

			src_off += insn.length;
			dst_off += insn.length;
		}
		write_abs_jmp(tramp + dst_off, const_cast<uint8_t *>(src) + total);
	} else {
		memcpy(tramp, src, total);
		write_abs_jmp(tramp + total, const_cast<uint8_t *>(src) + total);
	}

	FlushInstructionCache(GetCurrentProcess(), tramp, tramp_cap);

	hook->target = target;
	hook->detour = detour;
	hook->trampoline = tramp;
	hook->stolen = total;
	memcpy(hook->orig, src, total);

	DWORD old{};
	VirtualProtect(target, total, PAGE_EXECUTE_READWRITE, &old);
	write_abs_jmp(static_cast<uint8_t *>(target), detour);
	if (total > kJmpStubSize)
		memset(static_cast<uint8_t *>(target) + kJmpStubSize, 0x90,
		       total - kJmpStubSize);
	VirtualProtect(target, total, old, &old);
	FlushInstructionCache(GetCurrentProcess(), target, total);

	hook->installed = true;
	log_info("trampoline: installed %p -> %p (tramp=%p stolen=%zu rip_fix=%d)",
	         target, detour, tramp, total, (int)has_rip);
	return true;
}

bool trampoline_remove(TrampolineHook *hook) {
	if (!hook || !hook->installed)
		return false;

	DWORD old{};
	VirtualProtect(hook->target, hook->stolen, PAGE_EXECUTE_READWRITE, &old);
	memcpy(hook->target, hook->orig, hook->stolen);
	VirtualProtect(hook->target, hook->stolen, old, &old);
	FlushInstructionCache(GetCurrentProcess(), hook->target, hook->stolen);

	VirtualFree(hook->trampoline, 0, MEM_RELEASE);
	hook->trampoline = nullptr;
	hook->installed = false;

	log_info("trampoline: removed %p", hook->target);
	return true;
}
