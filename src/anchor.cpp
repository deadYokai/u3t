#define WIN32_LEAN_AND_MEAN
#include "anchor.hpp"
#include "disp_extract.hpp"
#include "logs.hpp"
#include "util.hpp"

#include <Zydis/Zydis.h>
#include <algorithm>
#include <cstring>
#include <string>

#include "decode.hpp"

namespace anchor
{
	namespace
	{
		constexpr bool kNative64 = (sizeof(void *) == 8);

		void init_decoder(ZydisDecoder &dec, bool x64)
		{
			dec = dx::native_decoder(x64);
		}

		uint8_t *rel32_target(const uint8_t *p, bool accept_jmp)
		{
			if (p[0] != 0xE8 && !(accept_jmp && p[0] == 0xE9))
				return nullptr;
			int32_t rel;
			memcpy(&rel, p + 1, 4);
			return const_cast<uint8_t *>(p) + 5 + rel;
		}

		template <class F>
		void for_each_rel32(const uint8_t *entry, const uint8_t *end,
		                    bool accept_jmp, bool skip_operand, F &&visit)
		{
			if (!entry || !end)
				return;
			for (const uint8_t *p = entry; p + 5 <= end; ++p)
			{
				uint8_t *tgt = rel32_target(p, accept_jmp);
				if (!tgt)
					continue;
				if (visit(const_cast<uint8_t *>(p), tgt))
					return;
				if (skip_operand)
					p += 4;
			}
		}

		void dedupe(std::vector<void *> &v)
		{
			std::sort(v.begin(), v.end());
			v.erase(std::unique(v.begin(), v.end()), v.end());
		}

		bool readable(const void *p, size_t n)
		{
			if (!p)
				return false;
			const uint8_t *cur = static_cast<const uint8_t *>(p);
			const uint8_t *end = cur + n;
			while (cur < end)
			{
				MEMORY_BASIC_INFORMATION mbi{};
				if (!VirtualQuery(cur, &mbi, sizeof(mbi)))
					return false;
				if (mbi.State != MEM_COMMIT)
					return false;
				if ((mbi.Protect & PAGE_GUARD) ||
				    (mbi.Protect & 0xFF) == PAGE_NOACCESS)
					return false;
				cur = static_cast<const uint8_t *>(mbi.BaseAddress) +
				      mbi.RegionSize;
			}
			return true;
		}

		bool decode_at(const ModuleImage &img, const void *site,
		               ZydisDecodedInstruction &ins, ZydisDecodedOperand *ops)
		{
			if (!site)
				return false;

			size_t avail = ZYDIS_MAX_INSTRUCTION_LENGTH;
			const uint8_t *p = static_cast<const uint8_t *>(site);
			if (img.ok && p >= img.base && p < img.base + img.size)
			{
				size_t left = static_cast<size_t>(img.base + img.size - p);
				if (left < avail)
					avail = left;
			}
			else if (!readable(p, avail))
			{
				while (avail && !readable(p, avail))
					--avail;
			}
			if (!avail)
				return false;

			ZydisDecoder dec;
			init_decoder(dec, img.x64);
			return ZYAN_SUCCESS(
			    ZydisDecoderDecodeFull(&dec, p, avail, &ins, ops));
		}
	}  // namespace

	struct XrefEntry
	{
		uint32_t target;
		uint32_t site;
	};

	std::vector<XrefEntry> g_xrefs;
	std::vector<uint32_t> g_entries;
	static bool g_entries_built = false;
	const uint8_t *g_xref_base = nullptr;
	bool g_xref_built = false;

	bool operand_target(const ModuleImage &img, const uint8_t *p, uint8_t len,
	                    const ZydisDecodedOperand &op, uintptr_t &out)
	{
		if (op.type == ZYDIS_OPERAND_TYPE_MEMORY &&
		    op.mem.base == ZYDIS_REGISTER_RIP && op.mem.disp.has_displacement)
		{
			out = reinterpret_cast<uintptr_t>(p + len) +
			      static_cast<int32_t>(op.mem.disp.value);
			return true;
		}
		if (op.type == ZYDIS_OPERAND_TYPE_MEMORY &&
		    op.mem.base == ZYDIS_REGISTER_NONE &&
		    op.mem.index == ZYDIS_REGISTER_NONE && op.mem.disp.has_displacement)
		{
			out = img.x64 ? static_cast<uintptr_t>(op.mem.disp.value)
			              : static_cast<uintptr_t>(
			                    static_cast<uint32_t>(op.mem.disp.value));
			return true;
		}
		if (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && !op.imm.is_relative)
		{
			out = static_cast<uintptr_t>(op.imm.value.u);
			return true;
		}
		return false;
	}

	void build_xref_index(const ModuleImage &img)
	{
		g_xrefs.clear();
		g_entries.clear();
		g_xref_base = img.base;
		g_xref_built = true;

		const uintptr_t lo = reinterpret_cast<uintptr_t>(img.base);
		const uintptr_t hi = lo + img.size;

		ZydisDecoder dec;
		init_decoder(dec, img.x64);
		g_xrefs.reserve(img.text_size / 64);
		g_entries.reserve(img.text_size / 128);

		uint8_t *p = img.text;
		uint8_t *end = img.text + img.text_size;
		while (p < end)
		{
			ZydisDecodedInstruction in;
			ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec, p, end - p, &in, ops)))
			{
				++p;
				continue;
			}
			for (uint8_t i = 0; i < in.operand_count_visible; ++i)
			{
				uintptr_t tgt;
				if (!operand_target(img, p, in.length, ops[i], tgt))
					continue;
				if (tgt < lo || tgt >= hi)
					continue;
				g_xrefs.push_back({static_cast<uint32_t>(tgt - lo),
				                   static_cast<uint32_t>(p - img.base)});
			}
			p += in.length;
		}

		std::sort(g_xrefs.begin(), g_xrefs.end(),
		          [](const XrefEntry &a, const XrefEntry &b)
		          {
			          return a.target != b.target ? a.target < b.target
			                                      : a.site < b.site;
		          });
		g_xrefs.erase(
		    std::unique(g_xrefs.begin(), g_xrefs.end(),
		                [](const XrefEntry &a, const XrefEntry &b)
		                { return a.target == b.target && a.site == b.site; }),
		    g_xrefs.end());

		std::sort(g_entries.begin(), g_entries.end());
		g_entries.erase(std::unique(g_entries.begin(), g_entries.end()),
		                g_entries.end());

		log_info("anchor: xref index built (%zu refs over %zu bytes)",
		         g_xrefs.size(), img.text_size);
		log_info("anchor: %zu direct-call entry point(s) indexed",
		         g_entries.size());
	}

	void *only(const std::vector<void *> &v, const char *what)
	{
		if (v.empty())
		{
			return nullptr;
		}
		return v.front();
	}

	static bool first_ret_pops(ZydisDecoder &dec, const uint8_t *entry,
	                           const uint8_t *limit, uint64_t want_pop)
	{
		const uint8_t *q = entry;
		while (q < limit)
		{
			ZydisDecodedInstruction in;
			ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
			if (ZYAN_FAILED(ZydisDecoderDecodeFull(
			        &dec, q, static_cast<ZyanUSize>(limit - q), &in, ops)))
				return false;

			if (in.mnemonic == ZYDIS_MNEMONIC_RET)
			{
				const uint64_t popped =
				    (in.operand_count_visible >= 1 && is_imm(ops[0]))
				        ? ops[0].imm.value.u
				        : 0;
				return popped == want_pop;
			}
			if (in.mnemonic == ZYDIS_MNEMONIC_JMP ||
			    in.mnemonic == ZYDIS_MNEMONIC_INT3)
				return false;

			q += in.length;
		}
		return false;
	}

	static bool callee_argnum_matches(ZydisDecoder &dec, const uint8_t *entry,
	                                  const uint8_t *limit, int argnum)
	{
		if (kNative64)
			return dx::x64_argnum_liveness(entry, limit) == argnum;

		const uint64_t want_pop = static_cast<uint64_t>(argnum) *
		                          static_cast<uint64_t>(sizeof(void *));
		return first_ret_pops(dec, entry, limit, want_pop);
	}

	static IMAGE_NT_HEADERS *nt_of(uint8_t *base)
	{
		auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
			return nullptr;
		auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
		return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
	}

	ModuleImage image_of(HMODULE mod)
	{
		ModuleImage img;
		if (!mod)
			mod = GetModuleHandleW(nullptr);
		auto *base = reinterpret_cast<uint8_t *>(mod);
		IMAGE_NT_HEADERS *nt = nt_of(base);
		if (!nt)
			return img;

		img.base = base;
		img.size = nt->OptionalHeader.SizeOfImage;

		auto *sec = IMAGE_FIRST_SECTION(nt);
		for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
		{
			const bool exec =
			    (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
			if (exec && !img.text)
			{
				img.text = base + sec[i].VirtualAddress;
				img.text_size = sec[i].Misc.VirtualSize;
			}
		}
		img.ok = img.text != nullptr;
		return img;
	}

	static std::vector<uint32_t> g_starts;
	static std::vector<uint32_t> g_code_ptrs;
	static const uint8_t *g_starts_base = nullptr;
	static bool g_starts_built = false;

	static bool is_pad_byte(uint8_t b) { return b == 0xCC || b == 0x90; }

	static bool is_seh_prologue(const uint8_t *p)
	{
		if (p[0] != 0x6A || p[1] != 0xFF)
			return false;
		if (p[2] != 0x68)
			return false;
		if (p[7] != 0x64 || p[8] != 0xA1 || p[9] != 0x00 || p[10] != 0x00 ||
		    p[11] != 0x00 || p[12] != 0x00)
			return false;
		if (p[13] != 0x50)
			return false;
		if ((p[14] == 0x83 && p[15] == 0xEC) ||
		    (p[14] == 0x81 && p[15] == 0xEC))
			return true;
		return false;
	}

	static bool prologue_look(const ModuleImage &img, const uint8_t *p)
	{
		if (!p)
			return false;

		if (p[0] == 0xE9 || (p[0] == 0xFF && p[1] == 0x25))
			return true;

		if (!img.x64)
		{
			if (p[0] == 0x6A && p[1] == 0xFF &&  // push -1
			    p[2] == 0x68 &&                  // push imm32
			    p[7] == 0x64 && p[8] == 0xA1 &&  // mov eax, fs:[0]
			    p[9] == 0x00 && p[10] == 0x00 && p[11] == 0x00 &&
			    p[12] == 0x00 && p[13] == 0x50 &&     // push eax
			    ((p[14] == 0x83 && p[15] == 0xEC) ||  // sub esp, imm8
			     (p[14] == 0x81 && p[15] == 0xEC)))   // sub esp, imm32
			{
				return true;
			}
			if (p[0] == 0x55)  // push ebp
				return true;
			if (p[0] == 0x8B && p[1] == 0xFF)  // mov edi,edi
				return true;
			if (p[0] == 0x6A && p[2] == 0x68)  // SEH prologue
				return true;
			if (p[0] == 0x68 && p[5] == 0x64)  // push handler
				return true;
			if (p[0] == 0x83 && p[1] == 0xEC)  // sub esp,i8
				return true;
			if (p[0] == 0x81 && p[1] == 0xEC)  // sub esp,i32
				return true;
			if (p[0] == 0x51 || p[0] == 0x53 || p[0] == 0x56 ||
			    p[0] == 0x57)  // push reg
				return true;
			if (p[0] == 0x8B &&
			    (p[1] == 0x44 || p[1] == 0x4C || p[1] == 0x54 ||
			     p[1] == 0x5C) &&
			    p[2] == 0x24)  // mov r,[esp+x]
				return true;
			if (p[0] == 0xA1 || p[0] == 0x33)  // mov eax,m / xor
				return true;
			if (p[0] == 0xF6 &&
			    p[1] == 0x05)  // test byte ptr [mem32], imm8  (init-guard, e.g.
			                   // FName::StaticInit)
				return true;
			if (p[0] == 0x80 && p[1] == 0x3D)  // cmp  byte ptr [mem32], imm8
				return true;
			if (p[0] == 0x83 && p[1] == 0x3D)  // cmp  dword ptr [mem32], imm8
				return true;
			return false;
		}

		if (p[0] == 0x55 || p[0] == 0x53 || p[0] == 0x56 || p[0] == 0x57)
			return true;
		if (p[0] == 0x40 && (p[1] == 0x53 || p[1] == 0x55 || p[1] == 0x56 ||
		                     p[1] == 0x57))  // push w/ REX
			return true;
		if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x89 && p[3] == 0x24)
			return true;                                   // mov [rsp+x],r
		if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xEC)  // sub rsp,i8
			return true;
		if (p[0] == 0x48 && p[1] == 0x81 && p[2] == 0xEC)  // sub rsp,i32
			return true;
		if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0xC4)  // mov rax,rsp
			return true;
		if (p[0] == 0x4C && p[1] == 0x8B && p[2] == 0xDC)  // mov r11,rsp
			return true;
		if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0xEC)  // mov rbp,rsp
			return true;
		return false;
	}

	static bool boundary_before(const ModuleImage &img, const uint8_t *p)
	{
		if (p <= img.text)
			return true;
		if (is_pad_byte(p[-1]))
			return true;
		if (p[-1] == 0xC3)  // ret
			return true;
		if (p - 3 >= img.text && p[-3] == 0xC2)  // ret imm16
			return true;
		if (p - 2 >= img.text && p[-2] == 0xEB)  // jmp rel8
			return true;
		if (p - 5 >= img.text && p[-5] == 0xE9)  // jmp rel32
			return true;
		return false;
	}

	static bool spans_cleanly(const ModuleImage &img, const uint8_t *start,
	                          const uint8_t *interior)
	{
		if (!start || start > interior)
			return false;
		if (start == interior)
			return true;

		ZydisDecoder dec;
		init_decoder(dec, img.x64);

		const uint8_t *p = start;
		bool prev_ret = false;
		while (p < interior)
		{
			if (*p == 0xCC)
				return false;
			if (p[0] == 0x90 && p + 1 < interior && p[1] == 0x90)
				return false;
			if (prev_ret && (prologue_look(img, p) ||
			                 (reinterpret_cast<uintptr_t>(p) & 0xF) == 0))
				return false;

			ZydisDecodedInstruction in;
			if (ZYAN_FAILED(ZydisDecoderDecodeInstruction(
			        &dec, nullptr, p, static_cast<ZyanUSize>(interior - p),
			        &in)))
				return false;

			prev_ret = (in.mnemonic == ZYDIS_MNEMONIC_RET);
			p += in.length;
		}
		return p == interior;
	}

	static void collect_code_pointers(const ModuleImage &img,
	                                  std::vector<uint32_t> &out)
	{
		const uintptr_t tlo = reinterpret_cast<uintptr_t>(img.text);
		const uintptr_t thi = tlo + img.text_size;
		const uint8_t *img_end = img.base + img.size;

		IMAGE_NT_HEADERS *nt = nt_of(img.base);
		if (!nt)
			return;

		const IMAGE_DATA_DIRECTORY rd =
		    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];

		size_t added = 0;
		if (rd.VirtualAddress && rd.Size)
		{
			const uint8_t *cur = img.base + rd.VirtualAddress;
			const uint8_t *end = cur + rd.Size;
			while (cur + sizeof(IMAGE_BASE_RELOCATION) <= end)
			{
				auto *blk =
				    reinterpret_cast<const IMAGE_BASE_RELOCATION *>(cur);
				if (blk->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) ||
				    blk->SizeOfBlock > static_cast<size_t>(end - cur))
					break;

				auto *ent = reinterpret_cast<const uint16_t *>(blk + 1);
				const size_t n =
				    (blk->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / 2;

				for (size_t i = 0; i < n; ++i)
				{
					const uint16_t type = ent[i] >> 12;
					const uint16_t off = ent[i] & 0x0FFF;
					const uint8_t *slot = img.base + blk->VirtualAddress + off;

					uintptr_t v = 0;
					if (type == IMAGE_REL_BASED_HIGHLOW)
					{
						if (slot + 4 > img_end)
							continue;
						uint32_t u;
						memcpy(&u, slot, 4);
						v = u;
					}
					else if (type == IMAGE_REL_BASED_DIR64)
					{
						if (slot + 8 > img_end)
							continue;
						uint64_t u;
						memcpy(&u, slot, 8);
						v = static_cast<uintptr_t>(u);
					}
					else
						continue;

					if (v >= tlo && v < thi)
					{
						out.push_back(static_cast<uint32_t>(
						    v - reinterpret_cast<uintptr_t>(img.base)));
						++added;
					}
				}
				cur += blk->SizeOfBlock;
			}
		}
		if (added)
			return;

		const size_t ps = img.x64 ? 8u : 4u;
		for (const uint8_t *p = img.base; p + ps <= img_end; p += ps)
		{
			if (p >= img.text && p < img.text + img.text_size)
				continue;
			uintptr_t v = 0;
			memcpy(&v, p, ps);
			if (v >= tlo && v < thi)
				out.push_back(static_cast<uint32_t>(
				    v - reinterpret_cast<uintptr_t>(img.base)));
		}
	}

	static void build_start_index(const ModuleImage &img)
	{
		g_starts.clear();
		g_code_ptrs.clear();
		g_starts_base = img.base;
		g_starts_built = true;

		const uint8_t *tb = img.text;
		const uint8_t *te = img.text + img.text_size;

		for (const uint8_t *p = tb; p + 5 <= te; ++p)
		{
			if (*p != 0xE8 && *p != 0xE9)
				continue;
			int32_t rel;
			memcpy(&rel, p + 1, 4);
			const uint8_t *t = p + 5 + rel;
			if (t >= tb && t < te)
				g_starts.push_back(static_cast<uint32_t>(t - img.base));
		}

		collect_code_pointers(img, g_code_ptrs);
		std::sort(g_code_ptrs.begin(), g_code_ptrs.end());
		g_code_ptrs.erase(std::unique(g_code_ptrs.begin(), g_code_ptrs.end()),
		                  g_code_ptrs.end());
		g_starts.insert(g_starts.end(), g_code_ptrs.begin(), g_code_ptrs.end());

		for (const uint8_t *p = tb; p < te; ++p)
		{
			if (*p != 0xCC && !(p[0] == 0x90 && p + 1 < te && p[1] == 0x90))
				continue;
			const uint8_t *q = p;
			while (q < te && is_pad_byte(*q))
				++q;
			if (q < te)
				g_starts.push_back(static_cast<uint32_t>(q - img.base));
			p = q;
		}

		std::sort(g_starts.begin(), g_starts.end());
		g_starts.erase(std::unique(g_starts.begin(), g_starts.end()),
		               g_starts.end());

		log_info("anchor: start index built (%zu candidates, %zu from data "
		         "pointers)",
		         g_starts.size(), g_code_ptrs.size());
	}

	void reset_start_index()
	{
		g_starts.clear();
		g_starts.shrink_to_fit();
		g_code_ptrs.clear();
		g_code_ptrs.shrink_to_fit();
		g_starts_built = false;
		g_starts_base = nullptr;
	}

	template <typename CH>
	static std::vector<const void *> find_lit(const ModuleImage &img,
	                                          const CH *needle)
	{
		std::vector<const void *> out;
		if (!img.ok || !needle)
			return out;
		const size_t n = std::char_traits<CH>::length(needle);
		const size_t bytes = (n + 1) * sizeof(CH);
		if (bytes == 0 || bytes > img.size)
			return out;

		const uint8_t *end = img.base + img.size - bytes;
		for (const uint8_t *p = img.base; p <= end; p += sizeof(CH))
		{
			if (memcmp(p, needle, bytes) == 0)
				out.push_back(p);
		}
		return out;
	}

	std::vector<const void *> find_wstr_all(const ModuleImage &img,
	                                        const wchar_t *needle)
	{
		return find_lit<wchar_t>(img, needle);
	}

	std::vector<const void *> find_cstr_all(const ModuleImage &img,
	                                        const char *needle)
	{
		return find_lit<char>(img, needle);
	}

	std::vector<void *> find_refs(const ModuleImage &img, const void *data)
	{
		std::vector<void *> out;
		if (!img.ok)
			return out;
		if (!g_xref_built || g_xref_base != img.base)
			build_xref_index(img);

		const uintptr_t lo = reinterpret_cast<uintptr_t>(img.base);
		const uintptr_t t = reinterpret_cast<uintptr_t>(data);
		if (t < lo || t >= lo + img.size)
			return out;
		const uint32_t rva = static_cast<uint32_t>(t - lo);

		auto it = std::lower_bound(g_xrefs.begin(), g_xrefs.end(), rva,
		                           [](const XrefEntry &e, uint32_t v)
		                           { return e.target < v; });
		for (; it != g_xrefs.end() && it->target == rva; ++it)
			out.push_back(img.base + it->site);
		return out;
	}

	void reset_xref_index()
	{
		g_xrefs.clear();
		g_xrefs.shrink_to_fit();
		g_entries.clear();
		g_entries.shrink_to_fit();
		g_xref_built = false;
		g_xref_base = nullptr;
	}

	static IMAGE_DATA_DIRECTORY exception_dir(const ModuleImage &img)
	{
		IMAGE_NT_HEADERS *nt = nt_of(img.base);
		return nt->OptionalHeader
		    .DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
	}

	static const IMAGE_RUNTIME_FUNCTION_ENTRY *
	chain_to_primary(const ModuleImage &img,
	                 const IMAGE_RUNTIME_FUNCTION_ENTRY *rf)
	{
		for (int hop = 0; hop < 8 && rf; ++hop)
		{
			const uint8_t *ui = img.base + rf->UnwindData;
			const uint8_t flags = (ui[0] >> 3) & 0x1F;
			if (!(flags & 0x4))
				return rf;

			const uint8_t count_of_codes = ui[2];
			size_t codes_bytes = static_cast<size_t>(count_of_codes) * 2;
			if (codes_bytes % 4 != 0)
				codes_bytes += 2;
			rf = reinterpret_cast<const IMAGE_RUNTIME_FUNCTION_ENTRY *>(
			    ui + 4 + codes_bytes);
		}
		return rf;
	}

	static void build_entries(const ModuleImage &img)
	{
		g_entries.clear();
		const uint8_t *tb = img.text, *te = img.text + img.text_size;
		for (const uint8_t *p = tb; p + 5 <= te; ++p)
		{
			if (*p != 0xE8)
				continue;
			int32_t rel;
			memcpy(&rel, p + 1, 4);
			const uint8_t *t = p + 5 + rel;
			if (t >= tb && t < te)
				g_entries.push_back(static_cast<uint32_t>(t - img.base));
		}
		std::sort(g_entries.begin(), g_entries.end());
		g_entries.erase(std::unique(g_entries.begin(), g_entries.end()),
		                g_entries.end());
		g_entries_built = true;
		log_info("anchor: entry index built (%zu call targets)",
		         g_entries.size());
	}

	static void *unwind_entry(const ModuleImage &img, const void *interior)
	{
		if (!img.x64)
			return nullptr;
		IMAGE_DATA_DIRECTORY d = exception_dir(img);
		if (!d.VirtualAddress || !d.Size)
			return nullptr;
		const auto rva = static_cast<uint32_t>(
		    reinterpret_cast<const uint8_t *>(interior) - img.base);
		auto *rf = reinterpret_cast<IMAGE_RUNTIME_FUNCTION_ENTRY *>(
		    img.base + d.VirtualAddress);
		size_t lo = 0, hi = d.Size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);
		while (lo < hi)
		{
			size_t mid = (lo + hi) / 2;
			if (rva < rf[mid].BeginAddress)
				hi = mid;
			else if (rva >= rf[mid].EndAddress)
				lo = mid + 1;
			else
			{
				const IMAGE_RUNTIME_FUNCTION_ENTRY *primary =
				    chain_to_primary(img, &rf[mid]);
				return img.base +
				       (primary ? primary->BeginAddress : rf[mid].BeginAddress);
			}
		}
		return nullptr;
	}

	void *function_entry(const ModuleImage &img, const void *interior)
	{
		if (!img.ok || !interior)
			return nullptr;

		const uint8_t *itr = reinterpret_cast<const uint8_t *>(interior);
		if (itr < img.text || itr >= img.text + img.text_size)
			return nullptr;

		if (void *e = unwind_entry(img, interior))
			return e;

		return function_start_heuristic(img, interior, 0x4000);
	}

	void *function_start_heuristic(const ModuleImage &img, const void *interior,
	                               size_t max_back)
	{
		if (!img.ok || !interior)
			return nullptr;

		const uint8_t *itr = static_cast<const uint8_t *>(interior);
		if (itr < img.text || itr >= img.text + img.text_size)
			return nullptr;

		if (img.x64)
		{
			if (void *e = unwind_entry(img, interior))
				return e;
		}

		if (!g_starts_built || g_starts_base != img.base)
			build_start_index(img);

		const uint32_t rva = static_cast<uint32_t>(itr - img.base);
		const uint8_t *limit = (static_cast<size_t>(itr - img.text) > max_back)
		                           ? itr - max_back
		                           : img.text;

		const uint8_t *best = nullptr;
		int best_score = -1;

		auto it = std::upper_bound(g_starts.begin(), g_starts.end(), rva);
		while (it != g_starts.begin())
		{
			--it;
			const uint8_t *cand = img.base + *it;
			if (cand < limit)
				break;
			if (cand > itr)
				continue;
			if (!spans_cleanly(img, cand, itr))
				continue;

			int score = 0;
			if (std::binary_search(g_code_ptrs.begin(), g_code_ptrs.end(), *it))
				score += 4;
			if (prologue_look(img, cand))
				score += 2;
			if (boundary_before(img, cand))
				score += 2;
			if ((reinterpret_cast<uintptr_t>(cand) & 0xF) == 0)
				score += 1;

			if (score >= best_score)
			{
				best_score = score;
				best = cand;
			}
		}

		return const_cast<uint8_t *>(best);
	}

	void *function_start_heuristic(const void *interior, size_t max_back)
	{
		return function_start_heuristic(image_of(nullptr), interior, max_back);
	}

	uint8_t *function_end(const ModuleImage &img, void *entry)
	{
		if (!img.ok || !entry)
			return nullptr;
		const auto rva = static_cast<uint32_t>(
		    reinterpret_cast<uint8_t *>(entry) - img.base);
		if (img.x64)
		{
			IMAGE_DATA_DIRECTORY d = exception_dir(img);
			if (d.VirtualAddress && d.Size)
			{
				auto *rf = reinterpret_cast<IMAGE_RUNTIME_FUNCTION_ENTRY *>(
				    img.base + d.VirtualAddress);
				size_t count = d.Size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);
				for (size_t i = 0; i < count; ++i)
					if (rf[i].BeginAddress == rva)
						return img.base + rf[i].EndAddress;
			}
		}

		uint8_t *e = reinterpret_cast<uint8_t *>(entry);
		uint8_t *cap = e + 0x2000;
		if (!img.x64)
		{
			if (!g_entries_built)
				build_entries(img);
			const uint32_t rva = static_cast<uint32_t>(e - img.base);
			auto it = std::upper_bound(g_entries.begin(), g_entries.end(), rva);
			if (it != g_entries.end())
			{
				uint8_t *nxt = img.base + *it;
				if (nxt < cap)
					return nxt;
			}
		}
		return cap;
	}

	std::vector<void *> function_candidates_argnum(const void *begin,
	                                               const void *end, int argnum)
	{
		std::vector<void *> out;
		if (!begin || !end || begin >= end || argnum < 0)
			return out;

		ZydisDecoder dec;
		init_decoder(dec, kNative64);

		const uint8_t *b = static_cast<const uint8_t *>(begin);
		const uint8_t *e = static_cast<const uint8_t *>(end);

		for (const uint8_t *p = b; p < e; ++p)
		{
			const bool at_start_of_range = (p == b);
			if (!at_start_of_range && p[-1] != 0xCC && p[-1] != 0x90)
				continue;
			if ((reinterpret_cast<uintptr_t>(p) & 0xF) != 0)
				continue;
			if (*p == 0xCC || *p == 0x90)
				continue;

			if (callee_argnum_matches(dec, p, e, argnum))
				out.push_back(const_cast<uint8_t *>(p));
		}
		return out;
	}

	std::vector<void *> function_calls_argnum(void *func_addr, int argnum)
	{
		std::vector<void *> out;
		if (!func_addr || argnum < 0)
			return out;

		ModuleImage img = image_of(nullptr);
		if (!img.ok)
			return out;

		uint8_t *entry = static_cast<uint8_t *>(func_addr);
		uint8_t *end = function_end(img, entry);
		if (!end)
			return out;

		ZydisDecoder dec;
		init_decoder(dec, img.x64);

		uint8_t *text_end = img.text + img.text_size;

		for_each_rel32(
		    entry, end, false, true,
		    [&](uint8_t *, uint8_t *tgt)
		    {
			    if (tgt >= img.text && tgt < text_end &&
			        callee_argnum_matches(dec, tgt, text_end, argnum))
				    out.push_back(tgt);
			    return false;
		    });

		dedupe(out);
		return out;
	}

	template <typename CH>
	static std::vector<void *> functions_referencing(const ModuleImage &img,
	                                                 const CH *needle)
	{
		std::vector<void *> fns;
		for (const void *s : find_lit<CH>(img, needle))
			for (void *site : find_refs(img, s))
				if (void *fn = function_entry(img, site))
					fns.push_back(fn);
		dedupe(fns);
		return fns;
	}

	std::vector<void *> functions_referencing_wstr(const ModuleImage &img,
	                                               const wchar_t *needle)
	{
		return functions_referencing<wchar_t>(img, needle);
	}

	std::vector<void *> functions_referencing_cstr(const ModuleImage &img,
	                                               const char *needle)
	{
		return functions_referencing<char>(img, needle);
	}

	bool function_calls(const ModuleImage &img, void *entry, const void *target)
	{
		if (!img.ok || !entry)
			return false;
		bool hit = false;
		for_each_rel32(reinterpret_cast<uint8_t *>(entry),
		               function_end(img, entry), false, false,
		               [&](uint8_t *, uint8_t *tgt)
		               {
			               if (tgt == reinterpret_cast<const uint8_t *>(target))
			               {
				               hit = true;
				               return true;
			               }
			               return false;
		               });
		return hit;
	}

	uint8_t *branch_target(const ModuleImage &img, const void *site)
	{
		ZydisDecodedInstruction ins;
		ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
		if (!decode_at(img, site, ins, ops))
			return nullptr;
		if (ins.mnemonic != ZYDIS_MNEMONIC_CALL &&
		    ins.mnemonic != ZYDIS_MNEMONIC_JMP)
			return nullptr;
		if (ins.operand_count_visible < 1)
			return nullptr;

		const ZydisDecodedOperand &op = ops[0];
		const ZyanU64 rip =
		    static_cast<ZyanU64>(reinterpret_cast<uintptr_t>(site));

		if (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op.imm.is_relative)
		{
			ZyanU64 abs = 0;
			if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&ins, &op, rip, &abs)))
				return nullptr;
			return reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(abs));
		}

		if (op.type == ZYDIS_OPERAND_TYPE_MEMORY)
		{
			if (op.mem.index != ZYDIS_REGISTER_NONE)
				return nullptr;
			if (op.mem.base != ZYDIS_REGISTER_NONE &&
			    op.mem.base != ZYDIS_REGISTER_RIP &&
			    op.mem.base != ZYDIS_REGISTER_EIP)
				return nullptr;

			ZyanU64 slot = 0;
			if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&ins, &op, rip, &slot)))
				return nullptr;

			const size_t psz = img.x64 ? 8u : 4u;
			if (psz > sizeof(uintptr_t))
				return nullptr;
			void *pslot =
			    reinterpret_cast<void *>(static_cast<uintptr_t>(slot));
			if (!readable(pslot, psz))
				return nullptr;

			uintptr_t tgt = 0;
			memcpy(&tgt, pslot, psz);
			return reinterpret_cast<uint8_t *>(tgt);
		}

		return nullptr;
	}

	uint8_t *resolve_thunk(const ModuleImage &img, void *fn, int max_hops)
	{
		uint8_t *cur = static_cast<uint8_t *>(fn);
		for (int i = 0; cur && i < max_hops; ++i)
		{
			ZydisDecodedInstruction ins;
			ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
			if (!decode_at(img, cur, ins, ops))
				break;
			if (ins.mnemonic != ZYDIS_MNEMONIC_JMP)
				break;
			uint8_t *next = branch_target(img, cur);
			if (!next || next == cur)
				break;
			cur = next;
		}
		return cur;
	}

	void *call_target(const ModuleImage &img, const void *site, int max_hops)
	{
		uint8_t *tgt = branch_target(img, site);
		if (!tgt)
			return nullptr;
		return resolve_thunk(img, tgt, max_hops);
	}

	void *callee_after_ref(const ModuleImage &img, const void *site,
	                       size_t max_scan)
	{
		if (!img.ok || !site)
			return nullptr;
		const uint8_t *begin = static_cast<const uint8_t *>(site);
		uint8_t *text_end = img.text + img.text_size;
		void *found = nullptr;
		for_each_rel32(begin, begin + max_scan, false, true,
		               [&](uint8_t *, uint8_t *tgt) -> bool
		               {
			               void *t = resolve_thunk(img, tgt, 8);
			               uint8_t *tb = static_cast<uint8_t *>(t);
			               if (tb >= img.text && tb < text_end)
			               {
				               found = t;
				               return true;
			               }
			               return false;
		               });
		return found;
	}

	void *nth_call_target(const ModuleImage &img, void *entry, int n)
	{
		if (!img.ok || !entry)
			return nullptr;
		uint8_t *text_end = img.text + img.text_size;
		void *found = nullptr;
		int seen = 0;
		for_each_rel32(reinterpret_cast<uint8_t *>(entry),
		               function_end(img, entry), true, true,
		               [&](uint8_t *, uint8_t *tgt)
		               {
			               if (tgt < img.text || tgt >= text_end)
				               return false;
			               if (seen++ == n)
			               {
				               found = tgt;
				               return true;
			               }
			               return false;
		               });
		return found;
	}

	std::vector<CallSite> call_sites(const ModuleImage &img, void *entry,
	                                 bool accept_jmp)
	{
		std::vector<CallSite> out;
		if (!img.ok || !entry)
			return out;
		uint8_t *text_end = img.text + img.text_size;
		for_each_rel32(reinterpret_cast<uint8_t *>(entry),
		               function_end(img, entry), accept_jmp, true,
		               [&](uint8_t *site, uint8_t *tgt)
		               {
			               if (tgt >= img.text && tgt < text_end)
				               out.push_back({site, tgt});
			               return false;
		               });
		return out;
	}

	std::vector<void *> direct_callers(const ModuleImage &img,
	                                   const void *target)
	{
		std::vector<void *> out;
		if (!img.ok || !target)
			return out;
		for_each_rel32(img.text, img.text + img.text_size, false, false,
		               [&](uint8_t *site, uint8_t *tgt)
		               {
			               if (tgt == reinterpret_cast<const uint8_t *>(target))
				               if (void *fn = function_entry(img, site))
					               out.push_back(fn);
			               return false;
		               });
		dedupe(out);
		return out;
	}

	std::vector<void *> direct_call_sites(const anchor::ModuleImage &img,
	                                      const void *target)
	{
		std::vector<void *> sites;
		if (!img.ok || !target)
			return sites;

		for_each_rel32(img.text, img.text + img.text_size, false, false,
		               [&](uint8_t *site, uint8_t *tgt)
		               {
			               if (tgt == reinterpret_cast<const uint8_t *>(target))
				               sites.push_back(site);
			               return false;
		               });
		return sites;
	}

}  // namespace anchor
