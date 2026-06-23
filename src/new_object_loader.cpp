#define WIN32_LEAN_AND_MEAN
#include "hook.hpp"
#include "logs.hpp"
#include "override_loader.hpp"
#include "pattern_scanner.hpp"
#include "ue3_types.hpp"
#include "util.hpp"
#include "utypes/FArchive.hpp"
#include "utypes/FName.hpp"
#include "utypes/TArray.hpp"
#include "utypes/ULinker.hpp"
#include "utypes/ULinkerLoad_Preload.hpp"

#include <string>
#include <vector>
#include <windows.h>

static constexpr int32_t kPkgRootIndex = 0;

static inline bool pkg_is_import(int32_t i) { return i < 0; }

static inline int32_t pkg_export_to_index(int32_t e) { return e + 1; }

static inline int32_t pkg_import_to_index(int32_t i) { return -(i + 1); }

static constexpr uint64_t kRF_Public = 0x0000000400000000ULL;
static constexpr uint64_t kRF_Standalone = 0x0008000000000000ULL;

#pragma pack(push, 8)

struct FObjectImport_Mirror
{
	int32_t ObjectName_Index;
	int32_t ObjectName_Number;
	int32_t OuterIndex;
	int32_t ClassPackage_Index;
	int32_t ClassPackage_Number;
	int32_t ClassName_Index;
	int32_t ClassName_Number;
	int32_t _pad0;
	void *XObject;
	void *SourceLinker;
	int32_t SourceIndex;
	int32_t _pad1;
};

#pragma pack(pop)
static_assert(sizeof(FObjectImport_Mirror) == 0x38, "FObjectImport size");
static_assert(offsetof(FObjectImport_Mirror, ClassPackage_Index) == 0x0C);
static_assert(offsetof(FObjectImport_Mirror, XObject) == 0x20);
static_assert(offsetof(FObjectImport_Mirror, SourceIndex) == 0x30);

static inline TArray<FObjectImport_Mirror> *linker_import_map(void *linker)
{
	return reinterpret_cast<TArray<FObjectImport_Mirror> *>(
	    static_cast<uint8_t *>(linker) + kLinker_ImportMap);
}

#pragma pack(push, 8)

struct FPatchData_Mirror
{
	void *Data;
	int32_t Num;
	int32_t Max;
};

#pragma pack(pop)

using AppendNamesFn = void(__cdecl *)(void *, const TArray<FName> *);
using AppendImportsFn = void(__cdecl *)(void *,
                                        const TArray<FObjectImport_Mirror> *);
using AppendExportsFn = void(__cdecl *)(void *,
                                        const TArray<FObjectExport_Mirror> *,
                                        const void *);
using CreatePatchReaderFn = void(__cdecl *)(void *);
using FNameInitFn = void(__cdecl *)(void *, const wchar_t *, int32_t);

static AppendNamesFn g_AppendNames = nullptr;
static AppendImportsFn g_AppendImports = nullptr;
static AppendExportsFn g_AppendExports = nullptr;
static CreatePatchReaderFn g_CreatePatchReader = nullptr;

struct NewObjectSpec
{
	std::wstring full_path;
	std::wstring object_name;
	std::wstring outer_path;
	std::wstring class_package;
	std::wstring class_name;
	bool injected = false;
};

static std::vector<NewObjectSpec> g_specs;

namespace new_object_loader
{
	void add_spec(const NewObjectSpec &s);
}

void new_object_loader::add_spec(const NewObjectSpec &s)
{
	g_specs.push_back(s);
}

static std::wstring fname_text(int32_t entry_index)
{
	auto *names = ue3().FNameNames;
	if (!names || entry_index < 0 || entry_index >= names->Num)
		return {};
	void *e = names->Data[entry_index];
	if (!e)
		return {};
	if (ue3().name_layout.is_unicode(e))
		return std::wstring(ue3().name_layout.uni(e));
	return to_wide(std::string(ue3().name_layout.ansi(e)));
}

static std::wstring export_path(void *linker, int32_t exp_idx);

static std::wstring import_path(void *linker, int32_t imp_idx)
{
	auto *im = linker_import_map(linker);
	if (imp_idx < 0 || imp_idx >= im->Num)
		return {};
	const FObjectImport_Mirror &i = im->Data[imp_idx];
	std::wstring nm = fname_text(i.ObjectName_Index >> kNameIndexShift);
	if (i.OuterIndex == kPkgRootIndex)
		return nm;
	std::wstring outer = pkg_is_import(i.OuterIndex)
	                         ? import_path(linker, -i.OuterIndex - 1)
	                         : export_path(linker, i.OuterIndex - 1);
	return outer.empty() ? nm : outer + L"." + nm;
}

static std::wstring export_path(void *linker, int32_t exp_idx)
{
	auto *em = linker_export_map(linker);
	if (exp_idx < 0 || exp_idx >= em->Num)
		return {};
	const FObjectExport_Mirror &e = em->Data[exp_idx];
	std::wstring nm = fname_text(e.ObjectName_Index >> kNameIndexShift);
	if (e.OuterIndex == kPkgRootIndex)
		return nm;
	std::wstring outer = pkg_is_import(e.OuterIndex)
	                         ? import_path(linker, -e.OuterIndex - 1)
	                         : export_path(linker, e.OuterIndex - 1);
	return outer.empty() ? nm : outer + L"." + nm;
}

static int32_t find_package_index(void *linker, const std::wstring &want)
{
	auto *em = linker_export_map(linker);
	for (int32_t i = 0; i < em->Num; ++i)
		if (export_path(linker, i) == want)
			return pkg_export_to_index(i);
	auto *im = linker_import_map(linker);
	for (int32_t i = 0; i < im->Num; ++i)
		if (import_path(linker, i) == want)
			return pkg_import_to_index(i);
	return 0;
}

static FName make_fname(const wchar_t *s)
{
	FNameOnStack tmp{};
	if (ue3().FNameInit)
		reinterpret_cast<FNameInitFn>(ue3().FNameInit)(&tmp, s,
		                                               1 /*FNAME_Add*/);
	FName n;
	n.Index = tmp.Index;
	n.Number = tmp.Number;
	return n;
}

static FName append_one_name(void *linker, const wchar_t *s)
{
	FName n = make_fname(s);
	TArray<FName> a{};
	a.Data = &n;
	a.Num = 1;
	a.Max = 1;
	g_AppendNames(linker, &a);
	return n;
}

static void inject_into_linker(void *linker)
{
	if (!g_AppendNames || !g_AppendImports || !g_AppendExports ||
	    !g_CreatePatchReader || !ue3().FNameInit)
		return;

	bool created_reader = false;

	for (auto &spec : g_specs)
	{
		if (spec.injected)
			continue;

		int32_t outer_idx = find_package_index(linker, spec.outer_path);
		if (outer_idx == 0)
			continue;

		if (!created_reader)
		{
			g_CreatePatchReader(linker);
			created_reader = true;
		}

		std::wstring class_full = spec.class_package + L"." + spec.class_name;
		int32_t class_idx = find_package_index(linker, class_full);
		if (class_idx == 0)
		{
			int32_t cpkg_idx = find_package_index(linker, spec.class_package);
			if (cpkg_idx == 0)
			{
				FName pname =
				    append_one_name(linker, spec.class_package.c_str());
				FName coreName = make_fname(L"Core");
				FName pkgClass = make_fname(L"Package");
				FObjectImport_Mirror pkgimp{};
				pkgimp.ObjectName_Index = pname.Index;
				pkgimp.ObjectName_Number = pname.Number;
				pkgimp.OuterIndex = kPkgRootIndex;
				pkgimp.ClassPackage_Index = coreName.Index;
				pkgimp.ClassPackage_Number = coreName.Number;
				pkgimp.ClassName_Index = pkgClass.Index;
				pkgimp.ClassName_Number = pkgClass.Number;
				TArray<FObjectImport_Mirror> a{};
				a.Data = &pkgimp;
				a.Num = 1;
				a.Max = 1;
				g_AppendImports(linker, &a);
				cpkg_idx =
				    pkg_import_to_index(linker_import_map(linker)->Num - 1);
			}
			FName cname = append_one_name(linker, spec.class_name.c_str());
			FName coreName = make_fname(L"Core");
			FName clsClass = make_fname(L"Class");
			FObjectImport_Mirror clsimp{};
			clsimp.ObjectName_Index = cname.Index;
			clsimp.ObjectName_Number = cname.Number;
			clsimp.OuterIndex = cpkg_idx;
			clsimp.ClassPackage_Index = coreName.Index;
			clsimp.ClassPackage_Number = coreName.Number;
			clsimp.ClassName_Index = clsClass.Index;
			clsimp.ClassName_Number = clsClass.Number;
			TArray<FObjectImport_Mirror> a{};
			a.Data = &clsimp;
			a.Num = 1;
			a.Max = 1;
			g_AppendImports(linker, &a);
			class_idx = pkg_import_to_index(linker_import_map(linker)->Num - 1);
		}

		FName obj_fname = append_one_name(linker, spec.object_name.c_str());

		FObjectExport_Mirror exp{};
		exp.ObjectName_Index = obj_fname.Index;
		exp.ObjectName_Number = obj_fname.Number;
		exp.OuterIndex = outer_idx;
		exp.ClassIndex = class_idx;
		exp.SuperIndex = kPkgRootIndex;
		exp.ArchetypeIndex = kPkgRootIndex;
		exp.ObjectFlags = kRF_NeedLoad | kRF_Public | kRF_Standalone;
		exp.SerialSize = 0;
		exp.SerialOffset = 0;

		auto *rec = override_loader::find(spec.full_path);
		FPatchData_Mirror pd{};
		if (rec && !rec->bin.empty())
		{
			pd.Data = const_cast<uint8_t *>(rec->bin.data());
			pd.Num = pd.Max = static_cast<int32_t>(rec->bin.size());
		}
		else
		{
			log_warn("newobj: no data record for '%ls' — object will load as "
			         "class defaults",
			         spec.full_path.c_str());
		}
		TArray<FObjectExport_Mirror> ea{};
		ea.Data = &exp;
		ea.Num = 1;
		ea.Max = 1;
		TArray<FPatchData_Mirror> pa{};
		pa.Data = &pd;
		pa.Num = 1;
		pa.Max = 1;
		g_AppendExports(linker, &ea, &pa);

		spec.injected = true;
		log_info("newobj: injected '%ls' outer=%d class=%d (ExportMap=%d)",
		         spec.full_path.c_str(), outer_idx, class_idx,
		         linker_export_map(linker)->Num);
	}
}

using CreateExportHashFn = int32_t(__cdecl *)(void *);
static CreateExportHashFn g_orig_CreateExportHash = nullptr;

static const char *kPat_CreateExportHash = "?? CreateExportHash pattern ??";
static const char *kPat_AppendNames = "?? AppendNames pattern ??";
static const char *kPat_AppendImports = "?? AppendImports pattern ??";
static const char *kPat_AppendExports = "?? AppendExports pattern ??";
static const char *kPat_CreatePatchReader = "?? CreatePatchReader pattern ??";

static int32_t __cdecl hooked_CreateExportHash(void *self)
{
	int32_t r = g_orig_CreateExportHash(self);
	inject_into_linker(self);
	return r;
}

namespace new_object_loader
{
	void install_hook()
	{
		if (g_specs.empty())
		{
			log_info("newobj: no specs — hook skipped");
			return;
		}
		HMODULE m = GetModuleHandleW(nullptr);
		g_AppendNames = reinterpret_cast<AppendNamesFn>(
		    FindPatternString(m, kPat_AppendNames));
		g_AppendImports = reinterpret_cast<AppendImportsFn>(
		    FindPatternString(m, kPat_AppendImports));
		g_AppendExports = reinterpret_cast<AppendExportsFn>(
		    FindPatternString(m, kPat_AppendExports));
		g_CreatePatchReader = reinterpret_cast<CreatePatchReaderFn>(
		    FindPatternString(m, kPat_CreatePatchReader));
		void *h = FindPatternString(m, kPat_CreateExportHash);

		if (!h || !g_AppendNames || !g_AppendImports || !g_AppendExports ||
		    !g_CreatePatchReader)
		{
			log_err("newobj: missing helper(s) — injection disabled "
			        "(hash=%p AN=%p AI=%p AE=%p CPR=%p)",
			        h, (void *)g_AppendNames, (void *)g_AppendImports,
			        (void *)g_AppendExports, (void *)g_CreatePatchReader);
			return;
		}
		hook::add(h, reinterpret_cast<void *>(&hooked_CreateExportHash),
		          reinterpret_cast<void **>(&g_orig_CreateExportHash));
		log_info("newobj: CreateExportHash hook at %p (%zu spec(s))", h,
		         g_specs.size());
	}
}  // namespace new_object_loader
