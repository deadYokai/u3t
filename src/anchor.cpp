#define WIN32_LEAN_AND_MEAN
#include "anchor.hpp"
#include "disp_extract.hpp"
#include "logs.hpp"
#include "util.hpp"

#include <Zydis/Zydis.h>
#include <algorithm>
#include <cstring>
#include <string>

namespace anchor
{
	namespace
	{
		constexpr bool kNative64 = (sizeof(void *) == 8);

		void init_decoder(ZydisDecoder &dec, bool x64)
		{
			ZydisDecoderInit(&dec,
			                 x64 ? ZYDIS_MACHINE_MODE_LONG_64
			                     : ZYDIS_MACHINE_MODE_LEGACY_32,
			                 x64 ? ZYDIS_STACK_WIDTH_64 : ZYDIS_STACK_WIDTH_32);
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
	}  // namespace

	struct XrefEntry
	{
		uint32_t target;
		uint32_t site;
	};

	std::vector<XrefEntry> g_xrefs;
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
		g_xref_base = img.base;
		g_xref_built = true;

		const uintptr_t lo = reinterpret_cast<uintptr_t>(img.base);
		const uintptr_t hi = lo + img.size;

		ZydisDecoder dec;
		init_decoder(dec, img.x64);
		g_xrefs.reserve(img.text_size / 64);

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

		log_info("anchor: xref index built (%zu refs over %zu bytes)",
		         g_xrefs.size(), img.text_size);
	}

	void *only(const std::vector<void *> &v, const char *what)
	{
		if (v.empty())
		{
			log_warn("resolve: '%s' anchor matched 0 functions", what);
			return nullptr;
		}
		if (v.size() > 1)
			log_warn("resolve: '%s' anchor ambiguous (%zu), using first", what,
			         v.size());
		return v.front();
	}

	static bool decodes_cleanly_to(const uint8_t *start,
	                               const uint8_t *interior)
	{
		ZydisDecoder dec;
		init_decoder(dec, kNative64);
		ZydisDecodedInstruction in;
		const uint8_t *p = start;
		while (p < interior)
		{
			if (ZYAN_FAILED(ZydisDecoderDecodeInstruction(
			        &dec, nullptr, p, static_cast<ZyanUSize>(interior - p),
			        &in)))
				return false;
			p += in.length;
		}
		return p == interior;
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

	void *function_entry(const ModuleImage &img, const void *interior)
	{
		if (!img.ok || !interior)
			return nullptr;
		const auto rva = static_cast<uint32_t>(
		    reinterpret_cast<const uint8_t *>(interior) - img.base);

		if (img.x64)
		{
			IMAGE_DATA_DIRECTORY d = exception_dir(img);
			if (d.VirtualAddress && d.Size)
			{
				auto *rf = reinterpret_cast<IMAGE_RUNTIME_FUNCTION_ENTRY *>(
				    img.base + d.VirtualAddress);
				size_t count = d.Size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);
				size_t lo = 0, hi = count;
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
						return img.base + (primary ? primary->BeginAddress
						                           : rf[mid].BeginAddress);
					}
				}
				return nullptr;
			}
		}

		const uint8_t *p = reinterpret_cast<const uint8_t *>(interior);
		const uint8_t *limit = (p - 0x4000 < img.text) ? img.text : p - 0x4000;
		const uint8_t *itr = reinterpret_cast<const uint8_t *>(interior);
		for (; p > limit; --p)
		{
			if (p[-1] != 0xCC)
				continue;
			const uint8_t *q = p;
			while (q < itr && (*q == 0xCC || *q == 0x90))
				++q;
			if (q >= itr)
				continue;
			if ((reinterpret_cast<uintptr_t>(q) & 0xF) != 0)
				continue;
			if (decodes_cleanly_to(q, itr))
				return const_cast<uint8_t *>(q);
		}
		return nullptr;
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
		return reinterpret_cast<uint8_t *>(entry) + 0x2000;
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

}  // namespace anchor
