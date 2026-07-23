#include "ue3_layout.hpp"

#include "addr_cache.hpp"
#include "anchor.hpp"
#include "disp_extract.hpp"
#include "disp_extract_arch.hpp"
#include "ue3_api.hpp"

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
				return true;
			}
		}
		return false;
	}
}  // namespace

static void dump_layout(const UE3Layout &L, const char *how)
{
	constexpr ptrdiff_t PS_ = static_cast<ptrdiff_t>(sizeof(void *));
	const bool override_ok = L.Preload && L.l_FArchiveOff && L.exp_stride &&
	                         L.l_ExportMap && L.l_Loader && L.vt_Serialize &&
	                         L.GSerializedObject && L.FNameNamesArr &&
	                         L.name.str_off;
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

		log_info("Resolving `appScriptOutputDir` -- if it has 1 warn - its ok");

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
			if (dx::first_rip_global_noncookie(
			        fn, anchor::function_end(img, fn), g))
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

	L.Preload = anchor::only(
	    anchor::functions_referencing_wstr(img, L"SerialSize"), "Preload");
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

		for (int slot : {10, 13, 16})
		{
			ptrdiff_t off = 0;
			if (dxa::field_off_for_vslot(L.Preload, end, slot, off))
			{
				L.l_Loader = L.l_FArchiveOff + off;
				break;
			}
		}

		if (!L.l_Loader)
			log_warn("resolve: Loader offset not derived");

		if (L.l_Loader)
			L.l_OriginalLoader = L.l_Loader + PS;

		{
			void **g = nullptr;
			ptrdiff_t vt = 0;
			if (dxa::serialized_object_and_serialize(L.Preload, end, g, vt))
			{
				L.GSerializedObject = g;
				L.vt_Serialize = vt;
			}
			else
				log_warn("resolve: GSerializedObject/Serialize not found");
		}

		void *fname_op = anchor::only(
		    anchor::functions_referencing_wstr(img, L"Bad name index %i/%i"),
		    "operator<<(FName)");
		void *seek_impl =
		    anchor::only(anchor::functions_referencing_wstr(
		                     img, L"SetFilePointer Failed %i/%i: %i %s"),
		                 "GetError (via FArchiveFileReaderWindows::Seek)");

		if (L.l_FArchiveOff && L.l_Loader && fname_op &&
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
	if (L.FNameNamesArr && !L.ArrayRealloc)
		log_warn("resolve: ArrayRealloc not found (runtime append disabled)");

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
	       L.l_Loader && L.vt_Serialize && L.GSerializedObject &&
	       L.FNameNamesArr && L.name.str_off;

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
