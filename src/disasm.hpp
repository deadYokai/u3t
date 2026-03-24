#pragma once
#include <cstddef>
#include <cstdint>

int find_fpf_vtable_slot(void **vtbl, int max_slots, uintptr_t exe_base,
                         size_t exe_size, int fallback_slot);
