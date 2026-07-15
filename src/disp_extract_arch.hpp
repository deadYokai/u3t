#pragma once
#include <cstddef>

namespace dxa
{
	bool serialized_object_and_serialize(const void *begin, const void *end,
	                                     void **&out_global,
	                                     ptrdiff_t &out_vtoff, int window = 10);

	bool gpackagefilecache(const void *begin, const void *end,
	                       void **&out_global);

	bool field_off_for_vslot(const void *begin, const void *end, int slot_index,
	                         ptrdiff_t &out_off);

	bool indexed_store_global(const void *begin, const void *end,
	                          void **&out_array, int scale);

	bool load_and_indexed_store_to_global(const void *b, const void *e,
	                                      void **global_ptr, int scale);
}  // namespace dxa
