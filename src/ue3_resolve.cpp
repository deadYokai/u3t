#include "ue3_layout.hpp"

#include "addr_cache.hpp"
#include "anchor.hpp"
#include "asm_pat.hpp"
#include "decode.hpp"
#include "disp_extract.hpp"
#include "disp_extract_arch.hpp"
#include "ue3_api.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "logs.hpp"

static UE3Layout g_ue3;

UE3Layout &ue3() { return g_ue3; }

namespace
{
	using anchor::ModuleImage;
	constexpr ptrdiff_t PS = static_cast<ptrdiff_t>(sizeof(void *));

	void *first_fn_referencing(const ModuleImage &img,
	                           std::initializer_list<const wchar_t *> anchors)
	{
		for (const wchar_t *s : anchors)
		{
			auto v = anchor::functions_referencing_wstr(img, s);
			if (v.size() == 1)
				return v.front();
		}
		return nullptr;
	}

	std::vector<ptrdiff_t> vcall_field_offsets(void *fn, uint8_t *end)
	{
		std::vector<ptrdiff_t> out;
		for (int slot = 0; slot < 64; ++slot)
		{
			ptrdiff_t off = 0;
			if (dxa::field_off_for_vslot(fn, end, slot, off) && off > 0 &&
			    (off % PS) == 0 && off <= 0x800)
				out.push_back(off);
		}
		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());
		std::reverse(out.begin(), out.end());
		return out;
	}

	void *preload_for_farchive_off(const ModuleImage &img, ptrdiff_t far_off)
	{
		for (void *site : dx::neg_lea_sites(img.text, img.text + img.text_size,
		                                    static_cast<int64_t>(far_off)))
		{
			void *fn = anchor::function_entry(img, site);
			if (!fn)
				continue;
			uint8_t *end = anchor::function_end(img, fn);
			int64_t v = 0;
			if (!dx::first_neg_lea(fn, end, v, 48) ||
			    v != static_cast<int64_t>(far_off))
				continue;
			if (!dx::first_imul_imm(fn, end, v) || v < 0x40 || v > 0x200)
				continue;
			return fn;
		}
		return nullptr;
	}

	void fill_formula_offsets(UE3Layout &L)
	{
		L.o_ObjectFlags = PS + 4;
		ptrdiff_t afterFlags = L.o_ObjectFlags + 8;
		L.o_Linker = afterFlags + 3 * PS;
		L.o_LinkerIndex = L.o_Linker + PS;
		L.o_Outer = L.o_LinkerIndex + 8;
		L.o_Name = L.o_Outer + PS;
		L.o_Class = L.o_Name + 8;
		L.sizeof_UObject = L.o_Class + 2 * PS;
		L.l_LinkerRoot = L.sizeof_UObject;

		L.e_ObjectName = 0x00;
		L.e_OuterIndex = 0x08;
		L.e_ClassIndex = 0x0C;
		L.e_SuperIndex = 0x10;
		L.e_ArchetypeIndex = 0x14;
		L.e_ObjectFlags = 0x18;
		L.e_SerialSize = 0x20;
		L.e_SerialOffset = 0x24;
		L.e_Object = 0x30;
		L.e_iHashNext = 0x30 + PS;
		L.e_ExportFlags = 0x30 + PS + 4;
	}

	bool probe_name_layout(uint8_t *names_arr, FNameLayout &nl)
	{
		if (!names_arr)
			return false;
		auto *data = static_cast<uint8_t *>(ue3raw::rd_ptr(names_arr, 0));
		if (!data)
			return false;
		auto *e0 = static_cast<uint8_t *>(ue3raw::rd_ptr(data, 0));
		if (!e0)
			return false;

		for (size_t off :
		     {(size_t)(PS + 4), (size_t)12, (size_t)16, (size_t)20, (size_t)24})
		{
			if (memcmp(e0 + off, "None", 4) == 0 && e0[off + 4] == 0)
			{
				nl.str_off = off;
				nl.with_flags = (off >= (size_t)(8 + 4 + PS));
				nl.always_wide = false;
				return true;
			}
		}

		static const unsigned char kWNone[] = {'N', 0, 'o', 0, 'n', 0, 'e', 0};
		for (size_t off :
		     {(size_t)16, (size_t)24, (size_t)12, (size_t)20, (size_t)28})
		{
			if (memcmp(e0 + off, kWNone, sizeof kWNone) == 0 &&
			    e0[off + 8] == 0 && e0[off + 9] == 0)
			{
				nl.str_off = off;
				nl.with_flags = false;
				nl.always_wide = true;
				return true;
			}
		}
		return false;
	}

	constexpr int kFindArgBytes = 8 * 4;

	bool find_arity_ok(const ModuleImage &img, uint8_t *site, void *callee)
	{
		if (img.x64)
			return true;
		void *caller = anchor::function_entry(img, site);
		int n = dx::call_stack_bytes(
		    site, caller ? anchor::function_end(img, caller) : nullptr, callee,
		    anchor::function_end(img, callee));
		return n == kFindArgBytes;
	}

	void *delegate_target(const ModuleImage &img, void *fn)
	{
		void *found = nullptr;
		int hits = 0;
		for (const auto &cs : anchor::call_sites(img, fn, /*accept_jmp=*/true))
		{
			if (cs.target == fn)
				continue;
			if (!find_arity_ok(img, cs.site, cs.target))
				continue;
			if (found != cs.target)
				++hits;
			found = cs.target;
		}
		return hits == 1 ? found : nullptr;
	}
}  // namespace

static void dump_layout(const UE3Layout &L, const char *how)
{
	constexpr ptrdiff_t PS_ = static_cast<ptrdiff_t>(sizeof(void *));
	const bool override_ok = L.Preload && L.l_FArchiveOff && L.exp_stride &&
	                         L.l_ExportMap && L.l_Loader && L.vt_Serialize &&
	                         L.FNameNamesArr && L.name.str_off;
	const bool find_ok = L.StaticFindObjectFast && L.FNameInit;

	log_info("resolve[%d-bit, %s]: GPL=%p SFOF=%p SLO=%p Init=%p Preload=%p",
	         (int)(PS_ * 8), how, L.GetPackageLinker, L.StaticFindObjectFast,
	         L.StaticLoadObject, L.FNameInit, L.Preload);
	log_info("resolve: GConfig=%p (%p)", (void *)L.GConfig,
	         L.GConfig ? *L.GConfig : nullptr);
	log_info("resolve: FArchiveOff=0x%zX stride=0x%zX Name/Imp/Exp=0x%zX/0x%zX/"
	         "0x%zX Loader=0x%zX Root=0x%zX",
	         (size_t)L.l_FArchiveOff, (size_t)L.exp_stride, (size_t)L.l_NameMap,
	         (size_t)L.l_ImportMap, (size_t)L.l_ExportMap, (size_t)L.l_Loader,
	         (size_t)L.l_LinkerRoot);
	log_info(
	    "resolve: uobj Flags/Linker/LinkerIdx/Outer/Name=0x%zX/0x%zX/0x%zX/"
	    "0x%zX/0x%zX vtSerialize=0x%zX str_off=%zu wf=%d",
	    (size_t)L.o_ObjectFlags, (size_t)L.o_Linker, (size_t)L.o_LinkerIndex,
	    (size_t)L.o_Outer, (size_t)L.o_Name, (size_t)L.vt_Serialize,
	    L.name.str_off, (int)L.name.with_flags);
	log_info("resolve: override_ok=%d find_ok=%d", (int)override_ok,
	         (int)find_ok);
}

bool has_and_this_imm(const ModuleImage &img, void *fn, uint64_t imm,
                      size_t window = 120)
{
	if (!fn)
		return false;

	const uint8_t *p = static_cast<const uint8_t *>(fn);
	const uint8_t *end = p + window;
	if (img.ok && end > img.text + img.text_size)
		end = img.text + img.text_size;

	ZydisDecoder dec = dx::native_decoder(img.x64);

	while (p < end)
	{
		if (*p == 0xCC)
			break;

		ZydisDecodedInstruction in;
		ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
		if (ZYAN_FAILED(ZydisDecoderDecodeFull(
		        &dec, p, static_cast<ZyanUSize>(end - p), &in, ops)))
			break;

		if ((in.mnemonic == ZYDIS_MNEMONIC_AND ||
		     in.mnemonic == ZYDIS_MNEMONIC_TEST) &&
		    in.operand_count_visible >= 2 && is_imm(ops[1], imm) &&
		    ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER)
		{
			const ZydisRegister r = ops[0].reg.value;
			if (r == ZYDIS_REGISTER_ECX || r == ZYDIS_REGISTER_RCX ||
			    r == ZYDIS_REGISTER_CX)
				return true;
		}

		p += in.length;
	}
	return false;
}

struct FieldVCall
{
	int base;
	ptrdiff_t off;
	int slot;
};

std::vector<FieldVCall> field_vcalls(const void *begin, const void *end)
{
	enum Kind
	{
		K_NONE,
		K_PTR,
		K_VT,
		K_FPTR
	};

	struct RV
	{
		Kind k = K_NONE;
		int base = -1;
		ptrdiff_t off = 0;
		int slot = 0;
	};

	std::vector<FieldVCall> out;
	if (!begin || !end || begin >= end)
		return out;

	RV r[16];
	ZydisDecoder dec = dx::native_decoder();

	auto is_stack = [](int gi) { return gi == 4 || gi == 5; };

	const uint8_t *p = static_cast<const uint8_t *>(begin);
	const uint8_t *e = static_cast<const uint8_t *>(end);

	while (p < e)
	{
		ZydisDecodedInstruction in;
		ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
		if (ZYAN_FAILED(ZydisDecoderDecodeFull(&dec, p, e - p, &in, ops)))
			break;

		if (in.mnemonic == ZYDIS_MNEMONIC_CALL && in.operand_count_visible >= 1)
		{
			if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER)
			{
				const int b = dxa::gpr_idx(ops[0].reg.value);
				if (b >= 0 && r[b].k == K_FPTR)
					out.push_back({r[b].base, r[b].off, r[b].slot});
			}
			else if (ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
			         ops[0].mem.index == ZYDIS_REGISTER_NONE &&
			         ops[0].mem.base != ZYDIS_REGISTER_RIP)
			{
				const int b = dxa::gpr_idx(ops[0].mem.base);
				const int64_t d = ops[0].mem.disp.has_displacement
				                      ? ops[0].mem.disp.value
				                      : 0;
				if (b >= 0 && r[b].k == K_VT && d >= 0 && (d % PS) == 0 &&
				    d < 64 * PS)
					out.push_back(
					    {r[b].base, r[b].off, static_cast<int>(d / PS)});
			}
		}

		RV prev[16];
		for (int i = 0; i < 16; ++i)
			prev[i] = r[i];

		for (int i = 0; i < in.operand_count; ++i)
			if (ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER &&
			    (ops[i].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE))
			{
				const int gi = dxa::gpr_idx(ops[i].reg.value);
				if (gi >= 0)
					r[gi] = {};
			}

		if (in.mnemonic == ZYDIS_MNEMONIC_CALL)
		{
			if (PS == 8)
				for (int gi : {0, 1, 2, 8, 9, 10, 11})
					r[gi] = {};
			else
				for (int gi : {0, 1, 2})
					r[gi] = {};
		}

		if (in.mnemonic == ZYDIS_MNEMONIC_MOV &&
		    in.operand_count_visible >= 2 &&
		    ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
		    ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
		    ops[1].mem.index == ZYDIS_REGISTER_NONE &&
		    ops[1].mem.base != ZYDIS_REGISTER_RIP)
		{
			const int dst = dxa::gpr_idx(ops[0].reg.value);
			const int b = dxa::gpr_idx(ops[1].mem.base);
			const int64_t d =
			    ops[1].mem.disp.has_displacement ? ops[1].mem.disp.value : 0;
			if (dst >= 0 && b >= 0)
			{
				if (prev[b].k == K_PTR && d == 0)
					r[dst] = {K_VT, prev[b].base, prev[b].off, 0};
				else if (prev[b].k == K_VT && d >= 0 && (d % PS) == 0 &&
				         d < 64 * PS)
					r[dst] = {K_FPTR, prev[b].base, prev[b].off,
					          static_cast<int>(d / PS)};
				else if (!is_stack(b) && d != 0)
					r[dst] = {K_PTR, b, static_cast<ptrdiff_t>(d), 0};
			}
		}

		p += in.length;
	}
	return out;
}

bool derive_loader_off(const void *begin, const void *end, ptrdiff_t &out_off,
                       int &out_tell)
{
	const auto calls = field_vcalls(begin, end);

	struct Cand
	{
		int base;
		ptrdiff_t off;
		std::vector<int> seq;
	};

	std::vector<Cand> cands;
	for (const FieldVCall &c : calls)
	{
		auto it = std::find_if(cands.begin(), cands.end(), [&](const Cand &x)
		                       { return x.base == c.base && x.off == c.off; });
		if (it == cands.end())
			cands.push_back({c.base, c.off, {c.slot}});
		else
			it->seq.push_back(c.slot);
	}

	const Cand *best = nullptr;
	size_t best_seeks = 0;

	for (const Cand &c : cands)
	{
		std::vector<int> d;
		for (int s : c.seq)
			if (std::find(d.begin(), d.end(), s) == d.end())
				d.push_back(s);

		log_info("resolve:   vcalls on [r%d+0x%zX]: %zu distinct slot(s)"
		         "%s%d%s",
		         c.base, (size_t)c.off, d.size(),
		         d.empty() ? "" : ", first=", d.empty() ? 0 : d[0],
		         d.size() >= 3 ? "" : " (too few)");

		if (d.size() < 3 || (c.off % PS) != 0)
			continue;
		if (d[1] != d[0] + 3 || d[2] != d[0] + 6)
			continue;

		const size_t seeks = static_cast<size_t>(
		    std::count(c.seq.begin(), c.seq.end(), d[0] + 3));
		if (!best || seeks > best_seeks)
		{
			best = &c;
			best_seeks = seeks;
			out_tell = d[0];
		}
	}

	if (!best)
		return false;
	out_off = best->off;
	return true;
}

bool ue3_resolve(UE3Layout &L)
{
	ModuleImage img = anchor::image_of(nullptr);
	if (!img.ok)
	{
		log_warn("resolve: bad PE image");
		return false;
	}
	fill_formula_offsets(L);

	if (addr_cache::load_ue3(L))
	{
		dump_layout(L, "cached");
		return L.ok;
	}

	L.GetPackageLinker = anchor::only(
	    anchor::functions_referencing_wstr(img, L"PackageResolveFailed"),
	    "GetPackageLinker");
	if (L.GetPackageLinker)
	{
		void **g = nullptr;
		if (dxa::gpackagefilecache(
		        L.GetPackageLinker,
		        anchor::function_end(img, L.GetPackageLinker), g))
			L.GPackageFileCache = g;
		else
			log_warn("resolve: GPackageFileCache not found");
	}

	{
		// (UnrealEd.EditorEngine | Editor.EditorEngine) & EditPackages & !Core
		static const wchar_t *kCfgAnchors[] = {L"UnrealEd.EditorEngine",
		                                       L"Editor.EditorEngine"};
		static const wchar_t *kCfgNot[] = {L"Core"};

		void *fn = nullptr;
		for (const wchar_t *a : kCfgAnchors)
		{
			const wchar_t *yes[] = {a, L"EditPackages"};
			fn = ue3_api::resolve_wstr_all_not(yes, kCfgNot, 2,
			                                   "appScriptOutputDir");
			if (fn)
				break;
		}

		if (!fn)
			log_warn("resolve: appScriptOutputDir anchor missing (GConfig "
			         "falls back to runtime capture)");
		else
		{
			void **g = nullptr;
			if (dx::first_global_noncookie(fn, anchor::function_end(img, fn),
			                               g))
				L.GConfig = g;
			else
				log_warn("resolve: GConfig not derived from "
				         "appScriptOutputDir");
		}
	}

	L.StaticFindObjectFast = anchor::only(
	    anchor::functions_referencing_wstr(
	        img,
	        L"Illegal call to StaticFindObjectFast() while serializing object "
	        L"data or garbage collecting!"),
	    "StaticFindObjectFast");

	{
		std::vector<void *> hits;
		for (void *fn :
		     anchor::functions_referencing_wstr(img, L"ObjectNotFound"))
			if (L.GetPackageLinker &&
			    anchor::function_calls(img, fn, L.GetPackageLinker))
				hits.push_back(fn);
		L.StaticLoadObject = anchor::only(hits, "StaticLoadObject");
	}

	if (!L.StaticFindObjectFast && L.StaticLoadObject)
	{
		static constexpr int64_t kRF_AsyncLoading = 0x400;
		std::vector<void *> hits;

		for (int n = 0;
		     void *t = anchor::nth_call_target(img, L.StaticLoadObject, n); ++n)
		{
			uint8_t *te = anchor::function_end(img, t);
			if (te && dx::imm_then_call(t, te, kRF_AsyncLoading, 24))
				hits.push_back(t);
		}

		L.StaticFindObjectFast = anchor::only(hits, "StaticFindObjectFast");
		if (L.StaticFindObjectFast)
			log_info(
			    "fallback resolve: StaticFindObjectFast=%p Internal=%p (via "
			    "StaticLoadObject)",
			    L.StaticFindObjectFast,
			    anchor::nth_call_target(img, L.StaticFindObjectFast, 0));
	}

	{
		auto hits = anchor::functions_referencing_wstr(img, L"SerialSize");
		L.Preload = hits.empty() ? nullptr : hits.front();
	}

	if (!L.Preload)
	{
		void *cx = first_fn_referencing(
		    img, {L"Circular reference to archetype for %s.%s",
		          L"ObjectArchetype for '%s': %s", L"Outer object for %s"});
		if (!cx)
			log_warn("resolve: CreateExport anchor missing");
		else
		{
			uint8_t *cxe = anchor::function_end(img, cx);
			for (ptrdiff_t cand : vcall_field_offsets(cx, cxe))
			{
				log_info("resolve: FArchiveOff candidate 0x%zX", (size_t)cand);
				if (void *pl = preload_for_farchive_off(img, cand))
				{
					L.Preload = pl;
					log_info("resolve: Preload=%p via CreateExport "
					         "(FArchiveOff=0x%zX)",
					         pl, (size_t)cand);
					break;
				}
			}
		}
	}

	if (PS == 4 && !L.Preload)
	{
		static const wchar_t *kN[] = {L"%s[%i]", L"%s %s"};
		void *a = ue3_api::resolve_wstr_all(kN, 2, "Preload child");

		for (void *r : anchor::direct_call_sites(img, a))
		{
			void *fn = anchor::function_start_heuristic(r);
			if (!fn)
			{
				log_warn("Preload: no start for site %p", r);
				continue;
			}

			if (!has_and_this_imm(img, fn, 0x200, 120))
			{
				continue;
			}

			L.Preload = fn;
			break;
		}

		if (!L.Preload)
			log_warn("resolve: Preload not found via '%%s[%%i]' callers");
	}

	if (L.Preload)
	{
		uint8_t *end = anchor::function_end(img, L.Preload);
		int64_t v = 0;

		if (dx::first_neg_lea(L.Preload, end, v, 48))
			L.l_FArchiveOff = static_cast<ptrdiff_t>(v);
		else
			log_warn("resolve: FArchiveOff not found");

		if (dx::first_imul_imm(L.Preload, end, v))
			L.exp_stride = static_cast<ptrdiff_t>(v);
		else
			log_warn("resolve: export stride not found");

		if (L.exp_stride &&
		    dx::array_base_disp_for_stride(L.Preload, end, L.exp_stride, v))
		{
			L.l_ExportMap = L.l_FArchiveOff + static_cast<ptrdiff_t>(v);
			ptrdiff_t ta = PS + 8;
			L.l_ImportMap = L.l_ExportMap - ta;
			L.l_NameMap = L.l_ExportMap - 2 * ta;
		}
		else
			log_warn("resolve: ExportMap offset not derived");

		{
			std::vector<std::pair<ptrdiff_t, int>> tally;
			for (int slot = 0; slot < 48; ++slot)
			{
				ptrdiff_t off = 0;
				if (!dxa::field_off_for_vslot(L.Preload, end, slot, off))
					continue;
				auto it = std::find_if(tally.begin(), tally.end(),
				                       [&](auto &p) { return p.first == off; });
				if (it == tally.end())
					tally.push_back({off, 1});
				else
					++it->second;
			}
			auto best = std::max_element(tally.begin(), tally.end(),
			                             [](auto &a, auto &b)
			                             { return a.second < b.second; });
			if (best != tally.end() && best->second >= 2)
				L.l_Loader = L.l_FArchiveOff + best->first;
		}

		if (!L.l_Loader)
		{
			ptrdiff_t off = 0;
			int tell_slot = -1;
			if (derive_loader_off(L.Preload, end, off, tell_slot))
			{
				L.l_Loader = L.l_FArchiveOff + off;
				log_info("resolve: Loader=0x%zX (FArchive+0x%zX, "
				         "Tell=%d Seek=%d Precache=%d)",
				         (size_t)L.l_Loader, (size_t)off, tell_slot,
				         tell_slot + 3, tell_slot + 6);
			}
			else
			{
				log_warn("resolve: no field in Preload matches the "
				         "Tell/Seek/Precache shape — FArchive vtable layout "
				         "changed, or Preload's body wasn't fully scanned");
			}

			if (L.l_Loader && L.l_ExportMap && L.l_Loader <= L.l_ExportMap)
			{
				log_warn("resolve: Loader offset 0x%zX precedes ExportMap "
				         "0x%zX — impossible layout, rejecting",
				         (size_t)L.l_Loader, (size_t)L.l_ExportMap);
				L.l_Loader = 0;
			}
		}

		if (!L.l_Loader)
			log_warn("resolve: Loader offset not derived");

		if (L.l_Loader)
			L.l_OriginalLoader = L.l_Loader + PS;

		{
			ptrdiff_t vt = 0;
			if (dxa::serialize_vslot(L.Preload, end, vt))
			{
				L.vt_Serialize = vt;
				log_info("resolve: Serialize slot = %zd (vt+0x%zX)",
				         (size_t)(vt / PS), (size_t)vt);
			}

			void **g = nullptr;
			ptrdiff_t gvt = 0;
			if (dxa::serialized_object_and_serialize(L.Preload, end, g, gvt))
			{
				L.GSerializedObject = g;
				if (!L.vt_Serialize)
					L.vt_Serialize = gvt;
			}
		}

		if (!L.vt_Serialize)
			log_warn("resolve: Serialize slot not derived");

		if (!L.GSerializedObject)
			log_warn("resolve: GSerializedObject not found "
			         "(expected on old game builds; load-error diagnostics "
			         "will be less detailed)");

		void *fname_op = anchor::only(
		    anchor::functions_referencing_wstr(img, L"Bad name index %i/%i"),
		    "operator<<(FName)");

		void *seek_impl =
		    anchor::only(anchor::functions_referencing_wstr(
		                     img, L"SetFilePointer Failed %i/%i: %i %s"),
		                 "GetError (via FArchiveFileReaderWindows::Seek)");

		if (L.l_FArchiveOff && L.l_Loader &&
		    !resolve_farchive_slots(L.ar, L.Preload, fname_op, seek_impl,
		                            L.l_FArchiveOff, L.l_Loader))
			log_warn("resolve: FArchive slots not fully derived "
			         "(validated=%d Serialize=%d Tell=%d) — check this build",
			         (int)L.ar.validated, L.ar.Serialize, L.ar.Tell);
	}

	for (void *fn : anchor::functions_referencing_wstr(
	         img, L"Hardcoded name '%s' at index %i was duplicated. "
	              L"Existing entry is '%s'."))
	{
		uint8_t *fend = anchor::function_end(img, fn);
		void **arr = nullptr;
		if (dxa::indexed_store_global(fn, fend, arr, static_cast<int>(PS)))
		{
			L.FNameNamesArr = reinterpret_cast<uint8_t *>(arr);
			L.ArrayRealloc = dx::call_feeding_global_store(fn, fend, arr);
			break;
		}
	}

	if (!L.FNameNamesArr)
	{
		static const wchar_t *kStaticInitAnchors[] = {
		    L"None", L"ByteProperty", L"IntProperty", L"BoolProperty"};
		void *static_init = ue3_api::resolve_wstr_all(kStaticInitAnchors, 4,
		                                              "FName::StaticInit");

		if (static_init)
		{
			struct CallTally
			{
				void *target;
				int count;
			};

			std::vector<CallTally> tally;
			for (const auto &cs : anchor::call_sites(img, static_init, false))
			{
				auto it = std::find_if(tally.begin(), tally.end(),
				                       [&](const CallTally &t)
				                       { return t.target == cs.target; });
				if (it == tally.end())
					tally.push_back({cs.target, 1});
				else
					++it->count;
			}
			std::sort(tally.begin(), tally.end(),
			          [](const CallTally &a, const CallTally &b)
			          { return a.count > b.count; });

			for (const CallTally &t : tally)
			{
				if (t.count < 20)
					break;

				uint8_t *tend = anchor::function_end(img, t.target);
				if (!tend)
					continue;

				void **arr = nullptr;
				if (dxa::indexed_store_global(t.target, tend, arr,
				                              static_cast<int>(PS)))
				{
					L.FNameNamesArr = reinterpret_cast<uint8_t *>(arr);
					L.ArrayRealloc =
					    dx::call_feeding_global_store(t.target, tend, arr);
					log_info(
					    "resolve: FNameNamesArr=%p via FName::StaticInit=%p "
					    "(register-fn=%p called %dx)",
					    (void *)L.FNameNamesArr, static_init, t.target,
					    t.count);
					break;
				}
			}

			if (!L.FNameNamesArr)
				log_warn("resolve: FName::StaticInit=%p found but its "
				         "Names-register helper wasn't identified",
				         static_init);
		}
	}

	if (L.FNameNamesArr && !L.ArrayRealloc)
		log_warn("resolve: ArrayRealloc not found (runtime append disabled)");

	if (PS == 4 && !L.FNameInit)
	{
		for (const void *s : anchor::find_wstr_all(img, L"<uninitialized>"))
		{
			for (void *r : anchor::find_refs(img, s))
			{
				void *entry = anchor::function_entry(img, r);
				if (!entry)
					continue;

				void *end = anchor::function_end(img, entry);
				if (!end)
					continue;

				const uint8_t *hit =
				    asmfindpat(entry, end, {"push 0x1", "push 0x1"}, {"ret"},
				               "call", 4, asmpat::OnDecodeFail::Stop);

				if (hit)
				{
					void *addr = anchor::call_target(img, hit);

					void *entry = anchor::function_entry(img, addr);
					if (!entry)
						continue;

					void *end = anchor::function_end(img, entry);
					if (!end)
						continue;

					const uint8_t *hit =
					    asmfindpat(entry, end, {}, {"ret"}, "call", 4,
					               asmpat::OnDecodeFail::Stop);
					if (hit)
					{
						L.FNameInit = anchor::call_target(img, hit);
						break;
					}
				}
			}
		}
	}

	if (!L.FNameInit)
	{
		uint8_t *cur = img.text, *tend = img.text + img.text_size;
		while (void *site = dx::find_split_name_setup(cur, tend))
		{
			void *fn = anchor::function_entry(img, site);
			uint8_t *fend = fn ? anchor::function_end(img, fn) : nullptr;

			if (fn && fend && dx::has_fname_none_store(fn, fend))
			{
				L.FNameInit = fn;
				break;
			}

			uint8_t *next = static_cast<uint8_t *>(site) + 1;
			if (fend && fend > next)
				next = fend;
			cur = next;
		}
	}

	if (!L.FNameNamesArr)
		log_warn("resolve: FName::Names not identified");
	else if (!L.FNameInit)
		log_warn("resolve: FName::Init not identified (name-remap disabled)");

	if (L.FNameNamesArr && !probe_name_layout(L.FNameNamesArr, L.name))
		log_warn("resolve: FName entry layout probe failed");

	L.ok = L.Preload && L.l_FArchiveOff && L.exp_stride && L.l_ExportMap &&
	       L.l_Loader && L.vt_Serialize && L.FNameNamesArr && L.name.str_off;

	dump_layout(L, "scanned");

	if (L.ok)
	{
		addr_cache::store_ue3(L);
		addr_cache::save();
	}
	else
	{
		log_warn("resolve: layout incomplete — not caching");
	}

	return L.ok;
}
