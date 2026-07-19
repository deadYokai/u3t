#pragma once
#include <cstdint>
#include <vector>

namespace dx
{
	bool first_neg_lea(const void *begin, const void *end, int64_t &out_disp,
	                   int max_insns = 24);

	bool first_imul_imm(const void *begin, const void *end, int64_t &out_imm);

	bool rip_store_then_vcall(const void *begin, const void *end,
	                          void **&out_global, int &out_slot,
	                          int call_window = 8);

	bool nth_rip_global(const void *begin, const void *end, int n,
	                    void **&out_global);

	bool first_rip_global_noncookie(const void *begin, const void *end,
	                                void **&out_global);

	std::vector<int> indirect_call_slots(const void *begin, const void *end);

	bool indexed_store_global(const void *begin, const void *end,
	                          void **&out_array_data, int scale = 8);

	bool array_base_disp_for_stride(const void *begin, const void *end,
	                                int64_t stride, int64_t &out_disp);

	bool field_disp_before_vcall(const void *begin, const void *end, int slot,
	                             int64_t &out_disp);

	std::vector<void *> field_lea_call_targets(const void *begin,
	                                           const void *end,
	                                           int64_t field_disp);

	bool imm_then_call(const void *begin, const void *end, int64_t imm,
	                   int window = 8);

	void *find_split_name_setup(const void *begin, const void *end);

	bool has_fname_none_store(const void *begin, const void *end);

	void *call_feeding_global_store(const void *begin, const void *end,
	                                void **global);

	int x64_argnum_liveness(const void *begin, const void *end,
	                        int max_insns = 32);

}  // namespace dx
