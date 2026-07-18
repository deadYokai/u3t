#include "ue3_patch.hpp"
#include "ue3_api.hpp"
#include "ue3_layout.hpp"

#include <cstdint>
#include <cstring>

#include "logs.hpp"

namespace
{
	constexpr int PS = static_cast<int>(sizeof(void *));

	void *game_realloc(void *data, uint32_t old_bytes, uint32_t new_bytes)
	{
		UE3Layout &L = ue3();
		if (!L.ArrayRealloc)
			return nullptr;
		if (PS == 8)
		{
			using Fn = void *(*)(void *, uint32_t, uint32_t);
			return reinterpret_cast<Fn>(L.ArrayRealloc)(data, old_bytes,
			                                            new_bytes);
		}
		else
		{
			// x86 appRealloc(Original, NewBytes, Alignment)
			using Fn = void *(*)(void *, uint32_t, uint32_t);
			return reinterpret_cast<Fn>(L.ArrayRealloc)(data, new_bytes,
			                                            /*align*/ 8);
		}
	}

	void *tarray_add_uninit(UE3TArray *arr, int count, int elem_size)
	{
		if (count <= 0)
			return nullptr;
		const int old_num = arr->Num;
		const int new_num = old_num + count;
		if (new_num > arr->Max)
		{
			const int new_max = new_num;
			void *nd = game_realloc(arr->Data,
			                        static_cast<uint32_t>(arr->Max * elem_size),
			                        static_cast<uint32_t>(new_max * elem_size));
			if (!nd)
				return nullptr;
			arr->Data = nd;
			arr->Max = new_max;
		}
		arr->Num = new_num;
		return static_cast<uint8_t *>(arr->Data) +
		       static_cast<size_t>(old_num) * elem_size;
	}

	bool make_fname(const wchar_t *name, UE3FName &out)
	{
		UE3Layout &L = ue3();
		if (!L.FNameInit)
			return false;
		out.Index = out.Number = 0;
		using Fn = void(UE3_THISCALL *)(void *, const wchar_t *, int, int, int);
		reinterpret_cast<Fn>(L.FNameInit)(&out, name, 0, 1, 1);
		return true;
	}
}  // namespace

int ue3_append_names(void *linker, const UE3FName *names, int count)
{
	UE3Layout &L = ue3();
	if (!linker || !names || count <= 0 || !L.l_NameMap || !L.ArrayRealloc)
		return -1;

	auto *name_map = reinterpret_cast<UE3TArray *>(
	    static_cast<uint8_t *>(linker) + L.l_NameMap);
	const int base = name_map->Num;

	void *dst =
	    tarray_add_uninit(name_map, count, static_cast<int>(sizeof(UE3FName)));
	if (!dst)
		return -1;
	memcpy(dst, names, static_cast<size_t>(count) * sizeof(UE3FName));

	log_info("append_names: linker=%p base=%d count=%d -> Num=%d", linker, base,
	         count, name_map->Num);
	return base;
}

int ue3_append_name_strings(void *linker, const wchar_t *const *names,
                            int count)
{
	if (count <= 0)
		return -1;
	UE3FName tmp[256];
	if (count > 256)
		return -1;
	for (int i = 0; i < count; ++i)
		if (!make_fname(names[i], tmp[i]))
			return -1;
	return ue3_append_names(linker, tmp, count);
}
