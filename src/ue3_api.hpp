#pragma once

#define WIN32_LEAN_AND_MEAN
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "logs.hpp"

#ifdef _WIN64
#define UE3_THISCALL
#else
#define UE3_THISCALL __thiscall
#endif

#ifdef _WIN64
#define UE3_EDX
#define UE3_EDX_ARG
#else
#define UE3_EDX void *,
#define UE3_EDX_ARG *,
#endif

namespace ue3_api
{
	enum class MatchMode
	{
		Any,
		All
	};

	void *resolve_wstr_ex(const wchar_t *const *needles, int count,
	                      const wchar_t *const *nots, int not_count,
	                      MatchMode mode, const char *label);

	void *resolve_cstr_ex(const char *const *needles, int count,
	                      const char *const *nots, int not_count,
	                      MatchMode mode, const char *label);

	void *resolve_wstr(const wchar_t *needle, const char *label);

	void *resolve_wstr_any(const wchar_t *const *needles, int count,
	                       const char *label);

	void *resolve_wstr_all(const wchar_t *const *needles, int count,
	                       const char *label);

	void *resolve_wstr_any_not(const wchar_t *const *needles, int count,
	                           const wchar_t *const *nots, int not_count,
	                           const char *label);

	void *resolve_wstr_all_not(const wchar_t *const *needles, int count,
	                           const wchar_t *const *nots, int not_count,
	                           const char *label);

	void *resolve_cstr(const char *needle, const char *label);

	void *resolve_cstr_any(const char *const *needles, int count,
	                       const char *label);

	void *resolve_cstr_all(const char *const *needles, int count,
	                       const char *label);

	void *resolve_cstr_all_not(const char *const *needles, int count,
	                           const char *const *nots, int not_count,
	                           const char *label);

	template <class W, size_t M>
	inline void *resolve_wstr_all_not(const wchar_t *const *needles,
	                                  W (&nots)[M], int count,
	                                  const char *label)
	{
		return resolve_wstr_all_not(needles, count, nots, static_cast<int>(M),
		                            label);
	}

	template <class W, size_t M>
	inline void *resolve_wstr_any_not(const wchar_t *const *needles,
	                                  W (&nots)[M], int count,
	                                  const char *label)
	{
		return resolve_wstr_any_not(needles, count, nots, static_cast<int>(M),
		                            label);
	}

	template <class C, size_t M>
	inline void *resolve_cstr_all_not(const char *const *needles, C (&nots)[M],
	                                  int count, const char *label)
	{
		return resolve_cstr_all_not(needles, count, nots, static_cast<int>(M),
		                            label);
	}

	void reset_cache();

	void set_engine_free(void *app_free_fn);
	void engine_free(void *ptr);

	template <class Fn> class EngineFn
	{
	  public:
		constexpr EngineFn(const wchar_t *anchor, const char *label)
		    : anchor_(anchor), label_(label)
		{
		}

		bool ensure()
		{
			if (!tried_)
			{
				tried_ = true;
				ptr_ = reinterpret_cast<Fn *>(resolve_wstr(anchor_, label_));
			}
			return ptr_ != nullptr;
		}

		explicit operator bool() { return ensure(); }

		Fn *raw()
		{
			ensure();
			return ptr_;
		}

		template <class... A> auto operator()(A &&...a)
		{
			if (!ensure())
			{
				log_err("ue3_api: call to unresolved engine fn '%s'", label_);
			}
			return ptr_(std::forward<A>(a)...);
		}

	  private:
		const wchar_t *anchor_;
		const char *label_;
		Fn *ptr_ = nullptr;
		bool tried_ = false;
	};

	class CaptureOutputDevice
	{
	  public:
		CaptureOutputDevice();

		void *device() { return &layout_; }

		const std::string &text() const { return buf_; }

		std::string take()
		{
			std::string s = std::move(buf_);
			buf_.clear();
			return s;
		}

		void clear() { buf_.clear(); }

		void on_serialize(const wchar_t *v);

	  private:
		struct Layout
		{
			const void *vtable;
			int32_t bAllowSuppression;
			int32_t bSuppressEventTag;
			int32_t bAutoEmitLineTerminator;
			CaptureOutputDevice *owner;
		};

		Layout layout_{};
		std::string buf_;
	};

}  // namespace ue3_api
