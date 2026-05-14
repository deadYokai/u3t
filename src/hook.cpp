#define WIN32_LEAN_AND_MEAN
#include "hook.hpp"
#include "logs.hpp"
#include <Zydis/Encoder.h>
#include <Zydis/Zydis.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>
#include <windows.h>

namespace hook
{

#ifdef _WIN64
	static constexpr ZydisMachineMode kMachineMode = ZYDIS_MACHINE_MODE_LONG_64;
	static constexpr ZydisStackWidth kStackWidth = ZYDIS_STACK_WIDTH_64;
#else
	static constexpr ZydisMachineMode kMachineMode =
	    ZYDIS_MACHINE_MODE_LEGACY_32;
	static constexpr ZydisStackWidth kStackWidth = ZYDIS_STACK_WIDTH_32;
#endif

	static constexpr size_t kPrologueCap = 64;
	static constexpr size_t kTrampolineCap = 128;

	static ZydisDecoder g_dec;
	static bool g_dec_ready = false;

	static ZydisDecoder &get_dec()
	{
		if (!g_dec_ready)
		{
			ZydisDecoderInit(&g_dec, kMachineMode, kStackWidth);
			g_dec_ready = true;
		}
		return g_dec;
	}

	static std::vector<void *> g_allocs;

	static uint8_t *alloc_rwx(size_t sz)
	{
		void *p = VirtualAlloc(nullptr, sz, MEM_COMMIT | MEM_RESERVE,
		                       PAGE_EXECUTE_READWRITE);
		if (!p)
			log_err("hook: VirtualAlloc(%zu) failed (err %lu)", sz,
			        GetLastError());
		return static_cast<uint8_t *>(p);
	}

	static void commit_alloc(void *p) { g_allocs.push_back(p); }

	static bool vprotect(void *p, size_t n, DWORD prot, DWORD *old)
	{
		if (VirtualProtect(p, n, prot, old))
			return true;
		log_err("hook: VirtualProtect(%p, %zu, 0x%lX) failed (err %lu)", p, n,
		        prot, GetLastError());
		return false;
	}

	static size_t emit_jmp(uint8_t *at, void *target)
	{
#ifdef _WIN64
		at[0] = 0x48;  // REX.W
		at[1] = 0xB8;  // mov rax, imm64
		memcpy(at + 2, &target, 8);

		at[10] = 0xFF;  // jmp rax
		at[11] = 0xE0;

		return 12;
#else
		auto rel =
		    static_cast<int32_t>((uintptr_t)target - ((uintptr_t)at + 5));
		at[0] = 0xE9;
		memcpy(at + 1, &rel, 4);
		return 5;
#endif
	}

	static size_t jmp_size_at([[maybe_unused]] void *at,
	                          [[maybe_unused]] void *target)
	{
#ifdef _WIN64
		return 14;
#else
		return 5;
#endif
	}

	static size_t
	copy_and_relocate(const uint8_t *src_bytes, uintptr_t src_va, uint8_t *dst,
	                  uintptr_t dst_va, const ZydisDecodedInstruction &ins,
	                  const ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT])
	{
		for (uint8_t i = 0; i < ins.operand_count_visible; i++)
		{
			if (ops[i].type != ZYDIS_OPERAND_TYPE_MEMORY ||
			    ops[i].mem.base != ZYDIS_REGISTER_RIP)
				continue;

			auto orig_disp = static_cast<intptr_t>(ops[i].mem.disp.value);
			auto abs_addr = static_cast<uintptr_t>(
			    static_cast<intptr_t>(src_va + ins.length) + orig_disp);

			intptr_t new_disp = static_cast<intptr_t>(abs_addr) -
			                    static_cast<intptr_t>(dst_va + ins.length);

			if (new_disp < INT32_MIN || new_disp > INT32_MAX)
			{
				log_err("hook: RIP-relative fixup at %#zx: "
				        "new disp %+td out of i32 range (target too far)",
				        (size_t)src_va, new_disp);
				return 0;
			}

			ZydisEncoderRequest req;
			if (ZYAN_FAILED(ZydisEncoderDecodedInstructionToEncoderRequest(
			        &ins, ops, ins.operand_count_visible, &req)))
			{
				log_err(
				    "hook: DecodedInstructionToEncoderRequest failed at %#zx",
				    (size_t)src_va);
				return 0;
			}

			req.operands[i].mem.displacement = static_cast<ZyanI64>(new_disp);

			uint8_t tmp[ZYDIS_MAX_INSTRUCTION_LENGTH];
			ZyanUSize enc_len = sizeof(tmp);
			if (ZYAN_FAILED(ZydisEncoderEncodeInstruction(&req, tmp, &enc_len)))
			{
				log_err("hook: re-encode RIP-relative failed at %#zx",
				        (size_t)src_va);
				return 0;
			}

			if (enc_len != ins.length)
				log_warn("hook: RIP-relative re-encode changed length %u->%zu "
				         "at %#zx",
				         ins.length, (size_t)enc_len, (size_t)src_va);

			memcpy(dst, tmp, enc_len);
			return enc_len;
		}

		for (uint8_t i = 0; i < ins.operand_count_visible; i++)
		{
			if (ops[i].type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
			    !ops[i].imm.is_relative)
				continue;

			intptr_t abs_target = static_cast<intptr_t>(src_va + ins.length) +
			                      static_cast<intptr_t>(ops[i].imm.value.s);

			intptr_t new_rel =
			    abs_target - static_cast<intptr_t>(dst_va + ins.length);

			const uint8_t imm_off = ins.raw.imm[0].offset;
			const uint8_t imm_sz = ins.raw.imm[0].size / 8;  // bits → bytes

			if (imm_off + imm_sz > ins.length)
			{
				log_err("hook: malformed relative immediate at %#zx",
				        (size_t)src_va);
				return 0;
			}

			memcpy(dst, src_bytes, ins.length);

			if (imm_sz == 1)
			{
				if (new_rel < INT8_MIN || new_rel > INT8_MAX)
				{
					log_err("hook: rel8 at %#zx needs rel32 — expansion not "
					        "supported",
					        (size_t)src_va);
					return 0;
				}
				auto v = static_cast<int8_t>(new_rel);
				memcpy(dst + imm_off, &v, 1);
			}
			else if (imm_sz == 4)
			{
				if (new_rel < INT32_MIN || new_rel > INT32_MAX)
				{
					log_err("hook: rel32 at %#zx out of range after relocation",
					        (size_t)src_va);
					return 0;
				}
				auto v = static_cast<int32_t>(new_rel);
				memcpy(dst + imm_off, &v, 4);
			}
			else
			{
				log_err("hook: unsupported relative imm size %u bytes at %#zx",
				        imm_sz, (size_t)src_va);
				return 0;
			}

			return ins.length;
		}

		memcpy(dst, src_bytes, ins.length);
		return ins.length;
	}

	struct HookEntry
	{
		void *target = nullptr;
		void *detour = nullptr;
		void **original = nullptr;
		uint8_t *trampoline = nullptr;
		uint8_t saved[kPrologueCap] = {};
		size_t saved_len = 0;
		bool installed = false;
	};

	static std::vector<HookEntry> g_hooks;

	static bool install_one(HookEntry &h)
	{
		auto *tgt = static_cast<uint8_t *>(h.target);

		const size_t min_patch = jmp_size_at(tgt, h.detour);

		struct InstrRec
		{
			size_t offset;
			size_t written;
			ZydisDecodedInstruction ins;
			ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
		};

		std::array<InstrRec, 32> recs;
		int rec_n = 0;
		size_t total = 0;

		while (total < min_patch)
		{
			if (rec_n >= static_cast<int>(recs.size()))
			{
				log_err("hook: %p — prologue decode overrun (>%zu instrs)",
				        h.target, recs.size());
				return false;
			}
			auto &r = recs[rec_n];
			r.offset = total;
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(&get_dec(), tgt + total,
			                                       ZYDIS_MAX_INSTRUCTION_LENGTH,
			                                       &r.ins, r.ops)))
			{
				log_err("hook: Zydis decode failed at %p+%zu", h.target, total);
				return false;
			}
			total += r.ins.length;
			rec_n++;
		}

		if (total > kPrologueCap)
		{
			log_err("hook: %p — prologue (%zu B) exceeds kPrologueCap",
			        h.target, total);
			return false;
		}

		uint8_t *tramp = alloc_rwx(kTrampolineCap);
		if (!tramp)
			return false;

		uint8_t *tp = tramp;
		size_t tp_used = 0;

		for (int i = 0; i < rec_n; i++)
		{
			auto &r = recs[i];

			const size_t room = kTrampolineCap - tp_used;
			if (room < ZYDIS_MAX_INSTRUCTION_LENGTH + 14)
			{
				log_err("hook: %p — trampoline overflow building prologue",
				        h.target);
				VirtualFree(tramp, 0, MEM_RELEASE);
				return false;
			}

			size_t w = copy_and_relocate(
			    tgt + r.offset, reinterpret_cast<uintptr_t>(tgt + r.offset), tp,
			    reinterpret_cast<uintptr_t>(tp), r.ins, r.ops);

			if (w == 0)
			{
				VirtualFree(tramp, 0, MEM_RELEASE);
				return false;
			}
			r.written = w;
			tp += w;
			tp_used += w;
		}

		size_t jback = emit_jmp(tp, tgt + total);
		tp += jback;
		tp_used += jback;
		(void)tp;

		{
			DWORD old;
			if (!vprotect(tramp, kTrampolineCap, PAGE_EXECUTE_READ, &old))
			{
				VirtualFree(tramp, 0, MEM_RELEASE);
				return false;
			}
		}

		*h.original = tramp;
		h.trampoline = tramp;

		memcpy(h.saved, tgt, total);
		h.saved_len = total;

		{
			DWORD old;
			if (!vprotect(tgt, total, PAGE_EXECUTE_READWRITE, &old))
			{
				*h.original = nullptr;
				h.trampoline = nullptr;
				VirtualFree(tramp, 0, MEM_RELEASE);
				return false;
			}

			size_t patch = emit_jmp(tgt, h.detour);
			for (size_t i = patch; i < total; i++)
				tgt[i] = 0x90;

			DWORD dummy;
			if (!vprotect(tgt, total, old, &dummy))
				log_warn("hook: could not restore protection at %p", h.target);
		}

		FlushInstructionCache(GetCurrentProcess(), tgt, total);

		commit_alloc(tramp);
		h.installed = true;
		log_info("hook: installed %p -> %p  tramp=%p  prologue=%zuB  "
		         "tramp_used=%zuB",
		         h.target, h.detour, tramp, total, tp_used);
		return true;
	}

	static void remove_one(HookEntry &h)
	{
		if (!h.installed)
			return;
		auto *tgt = static_cast<uint8_t *>(h.target);
		DWORD old;
		if (vprotect(tgt, h.saved_len, PAGE_EXECUTE_READWRITE, &old))
		{
			memcpy(tgt, h.saved, h.saved_len);
			DWORD dummy;
			vprotect(tgt, h.saved_len, old, &dummy);
		}
		FlushInstructionCache(GetCurrentProcess(), tgt, h.saved_len);
		h.installed = false;
	}

	void add(void *target, void *detour, void **original)
	{
		if (!target || !detour || !original)
		{
			log_err("hook::add: null arg  target=%p  detour=%p  original=%p",
			        target, detour, original);
			return;
		}
		g_hooks.push_back({target, detour, original});
	}

	void install_all()
	{
		for (auto &h : g_hooks)
			install_one(h);
	}

	void remove_all()
	{
		for (auto &h : g_hooks)
			remove_one(h);
		for (auto *p : g_allocs)
			VirtualFree(p, 0, MEM_RELEASE);
		g_allocs.clear();
		g_hooks.clear();
	}

}  // namespace hook
