#pragma once
#include "ue3_layout.hpp"

int ue3_append_names(void *linker, const FNameStack *names, int count);

int ue3_append_name_strings(void *linker, const wchar_t *const *names,
                            int count);

void *ue3_append_exports(void *linker, int count);
