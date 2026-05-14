#define WIN32_LEAN_AND_MEAN
#include "ue3_archive.hpp"
#include "logs.hpp"
#include <cstring>

void *UE3MemoryReader::g_vtable[FArchiveVtSlots::TotalSlots];
bool UE3MemoryReader::g_vtable_built = false;

static void __cdecl thunk_SerializeName(UE3MemoryReader *self, void *data,
                                        int32_t)
{
	static constexpr size_t kFNameSize = 8;
	if (self->pos + kFNameSize > self->size)
	{
		log_err("UE3MemoryReader::SerializeName: overread at pos=%zu",
		        self->pos);
		memset(data, 0, kFNameSize);
		return;
	}
	memcpy(data, self->buf + self->pos, kFNameSize);
	self->pos += kFNameSize;
}

static void __cdecl thunk_Serialize(UE3MemoryReader *self, void *data,
                                    int32_t len)
{
	if (len <= 0)
		return;
	size_t to_read = (size_t)len;
	if (self->pos + to_read > self->size)
	{
		log_err("UE3MemoryReader: overread! pos=%zu want=%zu size=%zu",
		        self->pos, to_read, self->size);
		to_read = self->size - self->pos;
	}
	if (to_read > 0)
	{
		memcpy(data, self->buf + self->pos, to_read);
		self->pos += to_read;
	}
}

static int32_t __cdecl thunk_IsLoading(UE3MemoryReader *) { return 1; }

static int32_t __cdecl thunk_IsSaving(UE3MemoryReader *) { return 0; }

static int32_t __cdecl thunk_Ver(UE3MemoryReader *self) { return self->ver; }

static int32_t __cdecl thunk_LicVer(UE3MemoryReader *self)
{
	return self->licver;
}

static int32_t __cdecl thunk_noop(UE3MemoryReader *) { return 0; }

void UE3MemoryReader::build_vtable()
{
	if (g_vtable_built)
		return;
	for (int i = 0; i < FArchiveVtSlots::TotalSlots; ++i)
		g_vtable[i] = reinterpret_cast<void *>(&thunk_noop);
	g_vtable[FArchiveVtSlots::Serialize] =
	    reinterpret_cast<void *>(&thunk_Serialize);
	g_vtable[FArchiveVtSlots::SerializeName] =
	    reinterpret_cast<void *>(&thunk_SerializeName);
	g_vtable[FArchiveVtSlots::IsLoading] =
	    reinterpret_cast<void *>(&thunk_IsLoading);
	g_vtable[FArchiveVtSlots::Ver] = reinterpret_cast<void *>(&thunk_Ver);
	g_vtable[FArchiveVtSlots::LicVer] = reinterpret_cast<void *>(&thunk_LicVer);
	g_vtable_built = true;
}

UE3MemoryReader::UE3MemoryReader(const uint8_t *data, size_t sz,
                                 int32_t version, int32_t lver)
    : buf(data), pos(0), size(sz), ver(version), licver(lver)
{
	build_vtable();
	vptr = g_vtable;
	memset(archiveData, 0, sizeof(archiveData));
}
