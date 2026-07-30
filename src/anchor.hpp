#pragma once
#define WIN32_LEAN_AND_MEAN
#include <cstdint>
#include <vector>
#include <windows.h>

namespace anchor
{
	struct ModuleImage
	{
		uint8_t *base = nullptr;
		size_t size = 0;
		uint8_t *text = nullptr;
		size_t text_size = 0;
		bool x64 = (sizeof(void *) == 8);
		bool ok = false;
	};

	ModuleImage image_of(HMODULE mod);

	void *only(const std::vector<void *> &v, const char *what);

	std::vector<const void *> find_wstr_all(const ModuleImage &img,
	                                        const wchar_t *needle);
	std::vector<const void *> find_cstr_all(const ModuleImage &img,
	                                        const char *needle);

	std::vector<void *> find_refs(const ModuleImage &img, const void *data);

	void *function_entry(const ModuleImage &img, const void *interior);

	std::vector<void *> function_candidates_argnum(const void *begin,
	                                               const void *end, int argnum);

	std::vector<void *> function_calls_argnum(void *func_addr, int argnum);

	std::vector<void *> functions_referencing_wstr(const ModuleImage &img,
	                                               const wchar_t *needle);

	std::vector<void *> functions_referencing_cstr(const ModuleImage &img,
	                                               const char *needle);

	uint8_t *function_end(const ModuleImage &img, void *entry);

	bool function_calls(const ModuleImage &img, void *entry,
	                    const void *target);

	uint8_t *branch_target(const ModuleImage &img, const void *site);
	uint8_t *resolve_thunk(const ModuleImage &img, void *fn, int max_hops = 8);
	void *call_target(const ModuleImage &img, const void *site,
	                  int max_hops = 8);

	void *nth_call_target(const ModuleImage &img, void *entry, int n);

	struct CallSite
	{
		uint8_t *site;
		uint8_t *target;
	};

	std::vector<CallSite> call_sites(const ModuleImage &img, void *entry,
	                                 bool accept_jmp = false);

	std::vector<void *> direct_callers(const ModuleImage &img,
	                                   const void *target);

	std::vector<void *> direct_call_sites(const ModuleImage &img,
	                                      const void *target);

	void *function_start_heuristic(const ModuleImage &img, const void *interior,
	                               size_t max_back = 0x4000);

	void *function_start_heuristic(const void *interior,
	                               size_t max_back = 0x4000);

	void *callee_after_ref(const ModuleImage &img, const void *site,
	                       size_t max_scan = 0x40);

	void reset_xref_index();
}  // namespace anchor
