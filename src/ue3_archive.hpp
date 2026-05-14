#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace FArchiveVtSlots
{
	static constexpr int Serialize = 2;
	static constexpr int IsLoading = 5;
	static constexpr int IsSaving = 6;
	static constexpr int SerializeName = 7;
	static constexpr int Ver = 8;
	static constexpr int LicVer = 9;
	static constexpr int TotalSlots = 32;
}  // namespace FArchiveVtSlots

struct UE3MemoryReader
{
	void **vptr;
	uint8_t archiveData[64] = {};

	const uint8_t *buf;
	size_t pos;
	size_t size;
	int32_t ver;
	int32_t licver;

	static void *g_vtable[FArchiveVtSlots::TotalSlots];
	static bool g_vtable_built;

	static void build_vtable();
	UE3MemoryReader(const uint8_t *data, size_t sz, int32_t version,
	                int32_t lver = 0);
};
