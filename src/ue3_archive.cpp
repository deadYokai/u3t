#define WIN32_LEAN_AND_MEAN
#include "ue3_archive.hpp"
#include "logs.hpp"
#include <cstring>

void *UE3MemoryReader::g_vtable[FArchiveVtSlots::TotalSlots];
bool UE3MemoryReader::g_vtable_built = false;

static void __cdecl thunk_Serialize(UE3MemoryReader *self, void *dst,
                                    int32_t len)
{
	if (len <= 0)
		return;
	const size_t want = static_cast<size_t>(len);
	const size_t avail =
	    (self->pos < self->size) ? (self->size - self->pos) : 0;
	const size_t n = (want <= avail) ? want : avail;
	if (n)
		memcpy(dst, self->buf + self->pos, n);
	self->pos += n;
	if (n < want)
	{
		log_err(
		    "UE3MemoryReader::Serialize overread: pos=%zu want=%zu size=%zu",
		    self->pos - n, want, self->size);

		memset(static_cast<uint8_t *>(dst) + n, 0, want - n);
		self->ar.ArIsError = 1;
	}
}

static int32_t __cdecl thunk_Tell(UE3MemoryReader *self)
{
	return static_cast<int32_t>(self->pos);
}

static int32_t __cdecl thunk_TotalSize(UE3MemoryReader *self)
{
	return static_cast<int32_t>(self->size);
}

static int32_t __cdecl thunk_AtEnd(UE3MemoryReader *self)
{
	return self->pos >= self->size ? 1 : 0;
}

static void __cdecl thunk_Seek(UE3MemoryReader *self, int32_t pos)
{
	if (pos >= 0 && static_cast<size_t>(pos) <= self->size)
		self->pos = static_cast<size_t>(pos);
	else
		log_warn("UE3MemoryReader::Seek out of range: pos=%d size=%zu", pos,
		         self->size);
}

static int32_t __cdecl thunk_Precache(UE3MemoryReader * /*self*/,
                                      int32_t /*off*/, int32_t /*sz*/)
{
	return 1;
}

static int32_t __cdecl thunk_GetError(UE3MemoryReader *self)
{
	return self->ar.ArIsError;
}

static int32_t __cdecl thunk_noop(UE3MemoryReader * /*self*/) { return 0; }

void UE3MemoryReader::build_vtable()
{
	if (g_vtable_built)
		return;

	for (int i = 0; i < FArchiveVtSlots::TotalSlots; ++i)
		g_vtable[i] = reinterpret_cast<void *>(&thunk_noop);

	g_vtable[FArchiveVtSlots::Serialize] =
	    reinterpret_cast<void *>(&thunk_Serialize);
	g_vtable[FArchiveVtSlots::Tell] = reinterpret_cast<void *>(&thunk_Tell);
	g_vtable[FArchiveVtSlots::TotalSize] =
	    reinterpret_cast<void *>(&thunk_TotalSize);
	g_vtable[FArchiveVtSlots::AtEnd] = reinterpret_cast<void *>(&thunk_AtEnd);
	g_vtable[FArchiveVtSlots::Seek] = reinterpret_cast<void *>(&thunk_Seek);
	g_vtable[FArchiveVtSlots::Precache] =
	    reinterpret_cast<void *>(&thunk_Precache);
	g_vtable[FArchiveVtSlots::GetError] =
	    reinterpret_cast<void *>(&thunk_GetError);

	g_vtable_built = true;
}

UE3MemoryReader::UE3MemoryReader(const uint8_t *data, size_t sz,
                                 int32_t version, int32_t lver, int32_t netver)
    : buf(data), pos(0), size(sz)
{
	build_vtable();
	vptr = g_vtable;

	memset(&ar, 0, sizeof(ar));
	ar.ArVer = version;
	ar.ArNetVer = netver ? netver : version;
	ar.ArLicenseeVer = lver;
	ar.ArIsLoading = 1;
	ar.ArIsSaving = 0;
	ar.ArIsPersistent = 1;
	ar.ArForEdit = 1;
	ar.ArForClient = 1;
	ar.ArForServer = 1;
	ar.ArAllowEliminatingReferences = 1;
}
