#pragma once
#include <cstddef>

namespace dx
{
	enum class CallConv
	{
		Unknown,
		Cdecl,
		Stdcall,
		Thiscall,
		Fastcall,
		Ms64,
	};

	const char *to_string(CallConv cc);

	struct CallConvResult
	{
		CallConv conv = CallConv::Unknown;

		int reg_int_args = 0;
		int stack_cleanup_bytes = -1;
		bool ecx_looks_like_this = false;
		bool confident = false;
	};

	class CallConvDetector
	{
	  public:
		explicit CallConvDetector(int max_prologue_insns = 24);

		CallConvResult detect(const void *begin,
		                      const void *end = nullptr) const;

	  private:
		int max_prologue_insns_;
	};

	CallConvResult detect_call_conv(const void *begin,
	                                const void *end = nullptr,
	                                int max_prologue_insns = 24);

	const void *guess_function_end(const void *begin,
	                               size_t max_scan_bytes = 4096,
	                               int padding_run = 2);

	template <typename Ret, typename... Args> class CallConvInvoker
	{
	  public:
		explicit CallConvInvoker(void *fn, const void *end = nullptr,
		                         int max_prologue_insns = 24)
		    : fn_(fn),
		      result_(CallConvDetector(max_prologue_insns).detect(fn, end))
		{
		}

		explicit CallConvInvoker(void *fn, CallConv conv) : fn_(fn)
		{
			result_.conv = conv;
			result_.confident = true;
		}

		const CallConvResult &result() const { return result_; }

		Ret operator()(Args... args) const
		{
			switch (result_.conv)
			{
				case CallConv::Cdecl:
					return reinterpret_cast<Ret(__cdecl *)(Args...)>(fn_)(
					    args...);
				case CallConv::Stdcall:
					return reinterpret_cast<Ret(__stdcall *)(Args...)>(fn_)(
					    args...);
				case CallConv::Thiscall:
					return reinterpret_cast<Ret(__thiscall *)(Args...)>(fn_)(
					    args...);
				case CallConv::Fastcall:
					return reinterpret_cast<Ret(__fastcall *)(Args...)>(fn_)(
					    args...);
				case CallConv::Ms64:
				case CallConv::Unknown:
				default:
					return reinterpret_cast<Ret (*)(Args...)>(fn_)(args...);
			}
		}

	  private:
		void *fn_;
		CallConvResult result_;
	};
}  // namespace dx
