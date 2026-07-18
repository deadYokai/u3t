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

#include "disp_extract_arch.hpp"

#ifdef _WIN64

#ifndef UWOP_PUSH_NONVOL
#define UWOP_PUSH_NONVOL 0
#define UWOP_ALLOC_LARGE 1
#define UWOP_ALLOC_SMALL 2
#define UWOP_SET_FPREG 3
#define UWOP_SAVE_NONVOL 4
#define UWOP_SAVE_NONVOL_FAR 5
#define UWOP_SAVE_XMM128 8
#define UWOP_SAVE_XMM128_FAR 9
#define UWOP_PUSH_MACHFRAME 10
#endif

struct UnwindCode
{
	uint8_t CodeOffset;
	uint8_t UnwindOpAndInfo;
};

struct UnwindInfoHdr
{
	uint8_t VersionAndFlags;
	uint8_t SizeOfProlog;
	uint8_t CountOfCodes;
	uint8_t FrameRegAndOff;
};

static constexpr int kMaxUnwindCodes = 16;

static constexpr size_t kRFSize = sizeof(RUNTIME_FUNCTION);

static size_t rf_offset_for(size_t code_sz)
{
	return (code_sz + 3) & ~size_t(3);
}

static size_t ui_offset_for(size_t code_sz)
{
	return rf_offset_for(code_sz) + kRFSize;
}

struct InstrForUnwind
{
	size_t tramp_off;
	ZydisDecodedInstruction ins;
	ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
};

static bool write_unwind_data(uint8_t *page_base, size_t tramp_code_sz,
                              size_t prolog_bytes,
                              const std::vector<InstrForUnwind> &instrs)
{
	struct UCode
	{
		uint8_t off;
		uint8_t op;
		uint8_t info;
	};

	std::vector<UCode> codes;

	for (const auto &ir : instrs)
	{
		const auto &ins = ir.ins;
		const auto *ops = ir.ops;

		if (ins.mnemonic == ZYDIS_MNEMONIC_PUSH &&
		    ins.operand_count_visible >= 1 &&
		    ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER)
		{
			int n = dxa::gpr_idx(ops[0].reg.value);
			if (n >= 0)
			{
				UCode c;
				c.off = static_cast<uint8_t>(ir.tramp_off + ins.length);
				c.op = UWOP_PUSH_NONVOL;
				c.info = static_cast<uint8_t>(n);
				codes.push_back(c);
			}
		}
		else if (ins.mnemonic == ZYDIS_MNEMONIC_SUB &&
		         ins.operand_count_visible >= 2 &&
		         ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
		         ops[0].reg.value == ZYDIS_REGISTER_RSP &&
		         ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
		{
			int64_t alloc = ops[1].imm.value.s;
			if (alloc > 0 && alloc <= 512 * 1024)
			{
				UCode c;
				c.off = static_cast<uint8_t>(ir.tramp_off + ins.length);
				if (alloc <= 128)
				{
					c.op = UWOP_ALLOC_SMALL;
					c.info = static_cast<uint8_t>((alloc - 8) / 8);
					codes.push_back(c);
				}
				else
				{
					c.op = UWOP_ALLOC_LARGE;
					c.info = 0;
					codes.push_back(c);
					UCode extra{0, static_cast<uint8_t>((alloc / 8) & 0xff),
					            static_cast<uint8_t>((alloc / 8) >> 8)};
					codes.push_back(extra);
				}
			}
		}
	}

	std::reverse(codes.begin(), codes.end());

	if (codes.size() % 2 != 0)
		codes.push_back({0, 0, 0});

	if (codes.size() > kMaxUnwindCodes)
	{
		log_err("hook: unwind codes overflow (%zu > %d)", codes.size(),
		        kMaxUnwindCodes);
		return false;
	}

	size_t rf_off = rf_offset_for(tramp_code_sz);
	size_t ui_off = ui_offset_for(tramp_code_sz);

	auto *rf = reinterpret_cast<RUNTIME_FUNCTION *>(page_base + rf_off);
	rf->BeginAddress = 0;
	rf->EndAddress = static_cast<DWORD>(tramp_code_sz);
	rf->UnwindData = static_cast<DWORD>(ui_off);

	auto *hdr = reinterpret_cast<UnwindInfoHdr *>(page_base + ui_off);
	hdr->VersionAndFlags = 1;  // version=1, flags=0
	hdr->SizeOfProlog = static_cast<uint8_t>(prolog_bytes);
	hdr->CountOfCodes = static_cast<uint8_t>(codes.size());
	hdr->FrameRegAndOff = 0;

	auto *uc = reinterpret_cast<UnwindCode *>(page_base + ui_off +
	                                          sizeof(UnwindInfoHdr));
	for (size_t i = 0; i < codes.size(); ++i)
	{
		uc[i].CodeOffset = codes[i].off;
		uc[i].UnwindOpAndInfo =
		    static_cast<uint8_t>((codes[i].info << 4) | codes[i].op);
	}

	log_info("hook: unwind data at page+%zu  rf_off=%zu  ui_off=%zu  "
	         "codes=%zu  prolog=%zuB",
	         rf_off, rf_off, ui_off, codes.size(), prolog_bytes);
	return true;
}

#endif

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
	static constexpr size_t kTrampolineCap = 256;

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
		ZydisEncoderRequest req;
		memset(&req, 0, sizeof(req));
		req.machine_mode = kMachineMode;

#ifdef _WIN64
		req.mnemonic = ZYDIS_MNEMONIC_MOV;
		req.operand_count = 2;
		req.operands[0].type = ZYDIS_OPERAND_TYPE_REGISTER;
		req.operands[0].reg.value = ZYDIS_REGISTER_RAX;
		req.operands[1].type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
		req.operands[1].imm.u =
		    static_cast<ZyanU64>(reinterpret_cast<uintptr_t>(target));

		ZyanUSize len = ZYDIS_MAX_INSTRUCTION_LENGTH;
		if (ZYAN_FAILED(ZydisEncoderEncodeInstruction(&req, at, &len)))
		{
			log_err("hook: encode 'mov rax, imm64' failed at %p", at);
			return 0;
		}
		size_t written = static_cast<size_t>(len);

		memset(&req, 0, sizeof(req));
		req.machine_mode = kMachineMode;
		req.mnemonic = ZYDIS_MNEMONIC_JMP;
		req.operand_count = 1;
		req.operands[0].type = ZYDIS_OPERAND_TYPE_REGISTER;
		req.operands[0].reg.value = ZYDIS_REGISTER_RAX;

		len = ZYDIS_MAX_INSTRUCTION_LENGTH;
		if (ZYAN_FAILED(
		        ZydisEncoderEncodeInstruction(&req, at + written, &len)))
		{
			log_err("hook: encode 'jmp rax' failed at %p", at);
			return 0;
		}
		written += static_cast<size_t>(len);
		return written;
#else
		req.mnemonic = ZYDIS_MNEMONIC_JMP;
		req.branch_width = ZYDIS_BRANCH_WIDTH_32;
		req.operand_count = 1;
		req.operands[0].type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
		req.operands[0].imm.u =
		    static_cast<ZyanU64>(reinterpret_cast<uintptr_t>(target));

		ZyanUSize len = ZYDIS_MAX_INSTRUCTION_LENGTH;
		if (ZYAN_FAILED(ZydisEncoderEncodeInstructionAbsolute(
		        &req, at, &len,
		        static_cast<ZyanU64>(reinterpret_cast<uintptr_t>(at)))))
		{
			log_err("hook: encode 'jmp rel32' failed at %p", at);
			return 0;
		}
		return static_cast<size_t>(len);
#endif
	}

	static size_t emit_jmp_indirect(uint8_t *at, void *target)
	{
#ifdef _WIN64
		constexpr size_t kInsnLen = 6;

		ZydisEncoderRequest req;
		memset(&req, 0, sizeof(req));
		req.machine_mode = kMachineMode;
		req.mnemonic = ZYDIS_MNEMONIC_JMP;
		req.operand_count = 1;
		req.operands[0].type = ZYDIS_OPERAND_TYPE_MEMORY;
		req.operands[0].mem.base = ZYDIS_REGISTER_RIP;
		req.operands[0].mem.index = ZYDIS_REGISTER_NONE;
		req.operands[0].mem.scale = 0;
		req.operands[0].mem.size = 8;
		req.operands[0].mem.displacement =
		    static_cast<ZyanI64>(reinterpret_cast<uintptr_t>(at) + kInsnLen);

		ZyanUSize len = ZYDIS_MAX_INSTRUCTION_LENGTH;
		if (ZYAN_FAILED(ZydisEncoderEncodeInstructionAbsolute(
		        &req, at, &len,
		        static_cast<ZyanU64>(reinterpret_cast<uintptr_t>(at)))))
		{
			log_err("hook: encode 'jmp [rip+0]' failed at %p", at);
			return 0;
		}
		if (len != kInsnLen)
		{
			log_err("hook: 'jmp [rip+0]' encoded to unexpected length "
			        "%zu (expected %zu) at %p",
			        static_cast<size_t>(len), kInsnLen, at);
			return 0;
		}

		memcpy(at + kInsnLen, &target, 8);
		return kInsnLen + 8;
#else
		// x86: no RAX capture in prologues, ordinary JMP is fine.
		return emit_jmp(at, target);
#endif
	}

	static size_t jmp_size_at([[maybe_unused]] void *at, void *target)
	{
		uint8_t scratch[32] = {};
		size_t n = emit_jmp(scratch, target);
		if (n == 0)
			log_err("hook: jmp_size_at probe failed for target %p", target);
		return n;
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
#ifdef _WIN64
		RUNTIME_FUNCTION *rf = nullptr;
#endif
		uint8_t saved[kPrologueCap] = {};
		size_t saved_len = 0;
		bool installed = false;
	};

	static std::vector<HookEntry> g_hooks;

	static bool install_one(HookEntry &h)
	{
		if (h.installed)
			return true;
		auto *tgt = static_cast<uint8_t *>(h.target);

		const size_t min_patch = jmp_size_at(tgt, h.detour);

		if (min_patch == 0)
		{
			log_err("hook: %p — could not determine redirect-jmp size",
			        h.target);
			return false;
		}

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

#ifdef _WIN64
		std::vector<InstrForUnwind> unwind_instrs;
#endif

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

#ifdef _WIN64
			InstrForUnwind ifu;
			ifu.tramp_off = tp_used;
			ifu.ins = r.ins;
			memcpy(ifu.ops, r.ops, sizeof(r.ops));
			unwind_instrs.push_back(ifu);
#endif

			r.written = w;
			tp += w;
			tp_used += w;
		}

		size_t jback = emit_jmp_indirect(tp, tgt + total);
		tp += jback;
		tp_used += jback;
		(void)tp;

		const size_t tramp_code_sz = tp_used;

#ifdef _WIN64
		const size_t unwind_space_needed =
		    kRFSize + sizeof(UnwindInfoHdr) +
		    (kMaxUnwindCodes * sizeof(UnwindCode));
		const size_t unwind_start = rf_offset_for(tramp_code_sz);

		if (unwind_start + unwind_space_needed > kTrampolineCap)
		{
			log_err("hook: %p — no room for unwind data in trampoline page "
			        "(code=%zu, needed=%zu, cap=%zu)",
			        h.target, tramp_code_sz, unwind_start + unwind_space_needed,
			        kTrampolineCap);
		}
		else
		{
			if (!write_unwind_data(tramp, tramp_code_sz, total, unwind_instrs))
			{
				log_warn("hook: %p — unwind data write failed "
				         "(trampoline unprotected for exception unwind)",
				         h.target);
			}
			else
			{
				h.rf = reinterpret_cast<RUNTIME_FUNCTION *>(
				    tramp + rf_offset_for(tramp_code_sz));

				if (!RtlAddFunctionTable(h.rf, 1,
				                         reinterpret_cast<DWORD64>(tramp)))
				{
					log_warn("hook: %p — RtlAddFunctionTable failed (err %lu)",
					         h.target, GetLastError());
					h.rf = nullptr;
				}
				else
				{
					log_info("hook: %p — RtlAddFunctionTable OK  rf=%p",
					         h.target, static_cast<void *>(h.rf));
				}
			}
		}
#endif

		{
			DWORD old;
			if (!vprotect(tramp, kTrampolineCap, PAGE_EXECUTE_READ, &old))
			{
#ifdef _WIN64
				if (h.rf)
					RtlDeleteFunctionTable(h.rf);
#endif

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
#ifdef _WIN64
		if (h.rf)
		{
			RtlDeleteFunctionTable(h.rf);
			h.rf = nullptr;
		}
#endif

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
