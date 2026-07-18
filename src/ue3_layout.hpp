#pragma once

#define WIN32_LEAN_AND_MEAN
#include "farchive_vtable.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <windows.h>

struct FNameStack
{
	int32_t Index;
	int32_t Number;
};

struct FArchiveFields
{
	int32_t ArVer, ArNetVer, ArLicenseeVer, ArIsLoading, ArIsSaving,
	    ArIsTransacting, ArWantBinaryPropertySerialization, ArForceUnicode,
	    ArIsPersistent, ArForEdit, ArForClient, ArForServer, ArIsError,
	    ArIsCriticalError, ArContainsCookedData, ArContainsCode, ArContainsMap,
	    ArForceByteSwapping, ArSerializingDefaults, ArIgnoreArchetypeRef,
	    ArIgnoreOuterRef, ArIgnoreClassRef, ArAllowEliminatingReferences,
	    ArAllowLazyLoading, ArIsObjectReferenceCollector, ArIsCountingMemory;
	uint32_t ArPortFlags;
	int32_t ArShouldSkipBulkData, ArIsSaveGame, ArIsFinalPackageSave,
	    ArMaxSerializeSize, ArIsFilterEditorOnly;
};

static_assert(sizeof(FArchiveFields) == 128, "FArchiveFields layout wrong");

namespace ue3raw
{
	inline uint8_t *at(const void *base, ptrdiff_t off)
	{
		return const_cast<uint8_t *>(static_cast<const uint8_t *>(base)) + off;
	}

	inline void *rd_ptr(const void *base, ptrdiff_t off)
	{
		void *v;
		memcpy(&v, at(base, off), sizeof(v));
		return v;
	}

	inline uint32_t rd_u32(const void *base, ptrdiff_t off)
	{
		uint32_t v;
		memcpy(&v, at(base, off), sizeof(v));
		return v;
	}

	inline uint64_t rd_u64(const void *base, ptrdiff_t off)
	{
		uint64_t v;
		memcpy(&v, at(base, off), sizeof(v));
		return v;
	}

	inline void wr_u64(void *base, ptrdiff_t off, uint64_t v)
	{
		memcpy(at(base, off), &v, sizeof(v));
	}
}  // namespace ue3raw

struct FNameLayout
{
	size_t str_off = 0;
	bool with_flags = false;

	int32_t raw_index(const void *e) const
	{
		return static_cast<int32_t>(
		    ue3raw::rd_u32(e, with_flags ? sizeof(void *) : 0));
	}

	bool is_unicode(const void *e) const { return (raw_index(e) & 1) != 0; }

	int name_index(const void *e) const { return raw_index(e) >> 1; }

	const char *ansi(const void *e) const
	{
		return reinterpret_cast<const char *>(ue3raw::at(e, str_off));
	}

	const wchar_t *uni(const void *e) const
	{
		return reinterpret_cast<const wchar_t *>(ue3raw::at(e, str_off));
	}
};

struct TArrayView
{
	uint8_t *data = nullptr;
	int32_t num = 0;

	bool valid() const { return data != nullptr; }
};

struct UE3Layout
{
	void *FNameInit = nullptr;
	void *ArrayRealloc = nullptr;
	void *StaticFindObjectFast = nullptr;
	void *StaticLoadObject = nullptr;
	void *GetPackageLinker = nullptr;
	void *Preload = nullptr;

	void **GPackageFileCache = nullptr;
	TArrayView *FNameNames = nullptr;
	uint8_t *FNameNamesArr = nullptr;
	void **GSerializedObject = nullptr;

	FNameLayout name;

	ptrdiff_t o_ObjectFlags = 0;
	ptrdiff_t o_Linker = 0;
	ptrdiff_t o_LinkerIndex = 0;
	ptrdiff_t o_Outer = 0;
	ptrdiff_t o_Name = 0;
	ptrdiff_t sizeof_UObject = 0;

	ptrdiff_t l_FArchiveOff = 0;
	ptrdiff_t l_LinkerRoot = 0;  // == sizeof_UObject
	ptrdiff_t l_NameMap = 0;
	ptrdiff_t l_ImportMap = 0;
	ptrdiff_t l_ExportMap = 0;
	ptrdiff_t l_Loader = 0;
	ptrdiff_t l_OriginalLoader = 0;

	ptrdiff_t exp_stride = 0;
	ptrdiff_t e_SerialSize = 0;
	ptrdiff_t e_SerialOffset = 0;
	ptrdiff_t e_ExportFlags = 0;

	ptrdiff_t vt_Serialize = 0;       // UObject::Serialize
	ptrdiff_t vt_Tell = 0;            // FArchive::Tell
	ptrdiff_t vt_Seek = 0;            // FArchive::Seek
	ptrdiff_t vt_Precache = 0;        // FArchive::Precache
	ptrdiff_t vt_SerializeFName = 0;  // FArchive::operator<<(FName&)

	FArchiveSlots ar;
	bool ok = false;
};

UE3Layout &ue3();
bool ue3_resolve(UE3Layout &out);

static constexpr uint64_t RF_ClassDefaultObject = 0x0000000000000200ULL;
static constexpr uint64_t RF_NeedLoad = 0x0000020000000000ULL;
static constexpr uint32_t EF_ForcedExport = 0x00000001u;
static constexpr uint32_t EF_ScriptPatcherExport = 0x00000002u;

// --- UObject ---
inline uint64_t uobj_flags(const void *o)
{
	return ue3raw::rd_u64(o, ue3().o_ObjectFlags);
}

inline bool uobj_has_flag(const void *o, uint64_t f)
{
	return (uobj_flags(o) & f) != 0;
}

inline void uobj_clear_flag(void *o, uint64_t f)
{
	ue3raw::wr_u64(o, ue3().o_ObjectFlags, uobj_flags(o) & ~f);
}

inline void *uobj_linker(const void *o)
{
	return ue3raw::rd_ptr(o, ue3().o_Linker);
}

inline int32_t uobj_linker_index(const void *o)
{
	return static_cast<int32_t>(ue3raw::rd_u32(o, ue3().o_LinkerIndex));
}

inline void *uobj_outer(const void *o)
{
	return ue3raw::rd_ptr(o, ue3().o_Outer);
}

inline int32_t uobj_name_index(const void *o)
{
	return static_cast<int32_t>(ue3raw::rd_u32(o, ue3().o_Name));
}

inline int32_t uobj_name_number(const void *o)
{
	return static_cast<int32_t>(ue3raw::rd_u32(o, ue3().o_Name + 4));
}

// --- ULinkerLoad ---
inline uint8_t *linker_base(void *l) { return static_cast<uint8_t *>(l); }

inline void *linker_farchive(void *l)
{
	return linker_base(l) + ue3().l_FArchiveOff;
}

inline void *linker_root(void *l)
{
	return ue3raw::rd_ptr(l, ue3().l_LinkerRoot);
}

inline void **linker_loader_ptr(void *l)
{
	return reinterpret_cast<void **>(linker_base(l) + ue3().l_Loader);
}

inline void **linker_original_loader_ptr(void *l)
{
	return reinterpret_cast<void **>(linker_base(l) + ue3().l_OriginalLoader);
}

inline TArrayView tarr_at(void *base, ptrdiff_t off)
{
	TArrayView v;
	v.data = static_cast<uint8_t *>(ue3raw::rd_ptr(base, off));
	v.num = static_cast<int32_t>(ue3raw::rd_u32(base, off + sizeof(void *)));
	return v;
}

inline TArrayView linker_namemap(void *l)
{
	return tarr_at(l, ue3().l_NameMap);
}

inline TArrayView linker_importmap(void *l)
{
	return tarr_at(l, ue3().l_ImportMap);
}

inline TArrayView linker_exportmap(void *l)
{
	return tarr_at(l, ue3().l_ExportMap);
}

inline void *lk_export(void *l, intptr_t idx)
{
	TArrayView em = linker_exportmap(l);
	if (!em.data || idx < 0 || idx >= em.num)
		return nullptr;
	return em.data + static_cast<ptrdiff_t>(idx) * ue3().exp_stride;
}

inline int32_t linker_namemap_index(void *l, intptr_t i)
{
	TArrayView nm = linker_namemap(l);
	if (!nm.data || i < 0 || i >= nm.num)
		return -1;
	return static_cast<int32_t>(ue3raw::rd_u32(nm.data, i * 8));
}

// --- FObjectExport field reads ---
inline int32_t exp_serial_size(const void *e)
{
	return e ? static_cast<int32_t>(ue3raw::rd_u32(e, ue3().e_SerialSize)) : 0;
}

inline int32_t exp_serial_offset(const void *e)
{
	return e ? static_cast<int32_t>(ue3raw::rd_u32(e, ue3().e_SerialOffset))
	         : 0;
}

inline uint32_t exp_export_flags(const void *e)
{
	return e ? ue3raw::rd_u32(e, ue3().e_ExportFlags) : 0u;
}

// --- FName table ---
inline void *fname_entry(int32_t index)
{
	UE3Layout &L = ue3();
	if (!L.FNameNamesArr || index < 0)
		return nullptr;
	uint8_t *data = static_cast<uint8_t *>(
	    ue3raw::rd_ptr(L.FNameNamesArr, 0));  // Names.Data
	int32_t num = static_cast<int32_t>(
	    ue3raw::rd_u32(L.FNameNamesArr, sizeof(void *)));  // Names.Num
	if (!data || index >= num)
		return nullptr;
	return ue3raw::rd_ptr(data, static_cast<ptrdiff_t>(index) * sizeof(void *));
}
