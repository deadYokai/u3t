#pragma once

namespace hook
{
	void add(void *target, void *detour, void **original);
	void install_all();
	void remove_all();
}  // namespace hook
