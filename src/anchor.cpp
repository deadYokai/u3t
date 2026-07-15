#include <string>
#define WIN32_LEAN_AND_MEAN
#include "anchor.hpp"
#include <Zydis/Zydis.h>
#include <algorithm>
#include <cstring>

namespace anchor
{

	static bool decodes_cleanly_to(const uint8_t *start,
	                               const uint8_t *interior)
	{
		ZydisDecoder dec;
		ZydisDecoderInit(&dec,
		                 sizeof(void *) == 8 ? ZYDIS_MACHINE_MODE_LONG_64
		                                     : ZYDIS_MACHINE_MODE_LEGACY_32,
		                 sizeof(void *) == 8 ? ZYDIS_STACK_WIDTH_64
		                                     : ZYDIS_STACK_WIDTH_32);
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
		if (!img.ok)
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
		const auto target = reinterpret_cast<uintptr_t>(data);

		if (img.x64)
		{
			uint8_t *p = img.text;
			uint8_t *end = img.text + img.text_size - 7;
			for (; p < end; ++p)
			{
				const uint8_t rex = p[0];
				if (rex != 0x48 && rex != 0x49 && rex != 0x4C && rex != 0x4D)
					continue;
				if (p[1] != 0x8D)
					continue;
				if ((p[2] & 0xC7) != 0x05)
					continue;
				int32_t disp;
				memcpy(&disp, p + 3, 4);
				auto tgt = reinterpret_cast<uintptr_t>(p + 7) + disp;
				if (tgt == target)
					out.push_back(p);
			}
		}
		else
		{
			uint32_t va32 = static_cast<uint32_t>(target);
			uint8_t *p = img.text;
			uint8_t *end = img.text + img.text_size - 5;
			for (; p < end; ++p)
			{
				uint32_t v;
				memcpy(&v, p, 4);
				if (v != va32)
					continue;
				const uint8_t op = p[-1];
				const bool push_imm = (op == 0x68);
				const bool mov_imm = (op >= 0xB8 && op <= 0xBF);
				const bool modrm_disp = true;
				if (push_imm || mov_imm || modrm_disp)
					out.push_back(p - 1);
			}
		}
		return out;
	}

	static IMAGE_DATA_DIRECTORY exception_dir(const ModuleImage &img)
	{
		IMAGE_NT_HEADERS *nt = nt_of(img.base);
		return nt->OptionalHeader
		    .DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
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
						uint32_t begin = rf[mid].BeginAddress;
						return img.base + begin;
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

	std::vector<void *> functions_referencing_wstr(const ModuleImage &img,
	                                               const wchar_t *needle)
	{
		std::vector<void *> fns;
		for (const void *s : find_wstr_all(img, needle))
			for (void *site : find_refs(img, s))
				if (void *fn = function_entry(img, site))
					fns.push_back(fn);
		std::sort(fns.begin(), fns.end());
		fns.erase(std::unique(fns.begin(), fns.end()), fns.end());
		return fns;
	}

	std::vector<void *> functions_referencing_cstr(const ModuleImage &img,
	                                               const char *needle)
	{
		std::vector<void *> fns;
		for (const void *s : find_cstr_all(img, needle))
			for (void *site : find_refs(img, s))
				if (void *fn = function_entry(img, site))
					fns.push_back(fn);
		std::sort(fns.begin(), fns.end());
		fns.erase(std::unique(fns.begin(), fns.end()), fns.end());
		return fns;
	}

	bool function_calls(const ModuleImage &img, void *entry, const void *target)
	{
		if (!img.ok || !entry)
			return false;
		uint8_t *p = reinterpret_cast<uint8_t *>(entry);
		uint8_t *end = function_end(img, entry);
		for (; p + 5 <= end; ++p)
		{
			if (p[0] != 0xE8)
				continue;
			int32_t rel;
			memcpy(&rel, p + 1, 4);
			if (reinterpret_cast<uint8_t *>(p + 5) + rel ==
			    reinterpret_cast<const uint8_t *>(target))
				return true;
		}
		return false;
	}

	void *nth_call_target(const ModuleImage &img, void *entry, int n)
	{
		if (!img.ok || !entry)
			return nullptr;
		uint8_t *p = reinterpret_cast<uint8_t *>(entry);
		uint8_t *end = function_end(img, entry);
		int seen = 0;
		for (; p + 5 <= end; ++p)
		{
			if (p[0] != 0xE8 && p[0] != 0xE9)
				continue;
			int32_t rel;
			memcpy(&rel, p + 1, 4);
			uint8_t *tgt = p + 5 + rel;
			if (tgt < img.text || tgt >= img.text + img.text_size)
				continue;
			if (seen++ == n)
				return tgt;
			p += 4;
		}
		return nullptr;
	}

	std::vector<void *> direct_callers(const ModuleImage &img,
	                                   const void *target)
	{
		std::vector<void *> out;
		if (!img.ok || !target)
			return out;
		uint8_t *p = img.text;
		uint8_t *end = img.text + img.text_size - 5;
		for (; p < end; ++p)
		{
			if (p[0] != 0xE8)
				continue;
			int32_t rel;
			memcpy(&rel, p + 1, 4);
			if (reinterpret_cast<uint8_t *>(p + 5) + rel !=
			    reinterpret_cast<const uint8_t *>(target))
				continue;
			if (void *fn = function_entry(img, p))
				out.push_back(fn);
		}
		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());
		return out;
	}
}  // namespace anchor
