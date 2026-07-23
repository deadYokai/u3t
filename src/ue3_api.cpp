#define WIN32_LEAN_AND_MEAN
#include "ue3_api.hpp"

#include "anchor.hpp"
#include "logs.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

namespace ue3_api
{
	namespace
	{
		std::unordered_map<std::wstring, std::vector<void *>> g_set_cache;
		std::unordered_map<std::wstring, void *> g_result_cache;

		std::wstring key_of(const wchar_t *s)
		{
			return std::wstring(L"w:") + (s ? s : L"");
		}

		std::wstring key_of(const char *s)
		{
			std::wstring w(L"c:");
			for (; s && *s; ++s)
				w.push_back(
				    static_cast<wchar_t>(static_cast<unsigned char>(*s)));
			return w;
		}

		template <class CH>
		std::vector<void *> scan(const anchor::ModuleImage &img,
		                         const CH *needle);

		template <>
		std::vector<void *> scan<wchar_t>(const anchor::ModuleImage &img,
		                                  const wchar_t *needle)
		{
			return anchor::functions_referencing_wstr(img, needle);
		}

		template <>
		std::vector<void *> scan<char>(const anchor::ModuleImage &img,
		                               const char *needle)
		{
			return anchor::functions_referencing_cstr(img, needle);
		}

		template <class CH>
		const std::vector<void *> &fns_for(const anchor::ModuleImage &img,
		                                   const CH *needle)
		{
			std::wstring k = key_of(needle);
			auto it = g_set_cache.find(k);
			if (it != g_set_cache.end())
				return it->second;
			return g_set_cache.emplace(std::move(k), scan<CH>(img, needle))
			    .first->second;
		}

		void intersect(std::vector<void *> &cur,
		               const std::vector<void *> &other)
		{
			std::vector<void *> keep;
			keep.reserve(cur.size());
			for (void *f : cur)
				if (std::find(other.begin(), other.end(), f) != other.end())
					keep.push_back(f);
			cur.swap(keep);
		}

		void subtract(std::vector<void *> &cur,
		              const std::vector<void *> &other)
		{
			if (cur.empty() || other.empty())
				return;
			std::vector<void *> keep;
			keep.reserve(cur.size());
			for (void *f : cur)
				if (std::find(other.begin(), other.end(), f) == other.end())
					keep.push_back(f);
			cur.swap(keep);
		}

		template <class CH>
		void exclude(const anchor::ModuleImage &img, std::vector<void *> &cur,
		             const CH *const *nots, int not_count)
		{
			if (!nots)
				return;
			for (int i = 0; i < not_count && !cur.empty(); ++i)
				if (nots[i])
					subtract(cur, fns_for(img, nots[i]));
		}

		template <class CH>
		std::wstring request_key(const CH *const *needles, int count,
		                         const CH *const *nots, int not_count,
		                         MatchMode mode)
		{
			std::wstring k = (mode == MatchMode::All) ? L"A" : L"O";
			for (int i = 0; i < count; ++i)
			{
				k += L'\x01';
				k += key_of(needles[i]);
			}
			for (int i = 0; nots && i < not_count; ++i)
			{
				k += L'\x02';
				k += key_of(nots[i]);
			}
			return k;
		}

		template <class CH>
		void *resolve_impl(const CH *const *needles, int count,
		                   const CH *const *nots, int not_count, MatchMode mode,
		                   const char *label)
		{
			if (!needles || count <= 0)
				return nullptr;
			if (!nots)
				not_count = 0;

			std::wstring key =
			    request_key(needles, count, nots, not_count, mode);
			auto cached = g_result_cache.find(key);
			if (cached != g_result_cache.end())
				return cached->second;

			anchor::ModuleImage img = anchor::image_of(nullptr);
			if (!img.ok)
			{
				log_warn("ue3_api: bad PE image resolving '%s'", label);
				g_result_cache.emplace(std::move(key), nullptr);
				return nullptr;
			}

			std::vector<void *> cur;

			if (mode == MatchMode::All)
			{
				cur = fns_for(img, needles[0]);
				for (int i = 1; i < count && !cur.empty(); ++i)
					intersect(cur, fns_for(img, needles[i]));
				exclude(img, cur, nots, not_count);
			}
			else
			{
				for (int i = 0; i < count; ++i)
				{
					std::vector<void *> c = fns_for(img, needles[i]);
					exclude(img, c, nots, not_count);
					if (!c.empty())
					{
						cur.swap(c);
						break;
					}
				}
			}

			void *fn = anchor::only(cur, label);
			if (fn)
				log_info("ue3_api: resolved '%s' = %p (%d %s anchor(s), %d "
				         "exclusion(s))",
				         label, fn, count,
				         mode == MatchMode::All ? "all" : "any", not_count);
			else
				log_warn("ue3_api: '%s' - %zu candidate(s) after %s over %d "
				         "anchor(s) and %d exclusion(s)",
				         label, cur.size(),
				         mode == MatchMode::All ? "intersecting" : "scanning",
				         count, not_count);

			g_result_cache.emplace(std::move(key), fn);
			return fn;
		}
	}  // namespace

	void *resolve_wstr_ex(const wchar_t *const *needles, int count,
	                      const wchar_t *const *nots, int not_count,
	                      MatchMode mode, const char *label)
	{
		return resolve_impl(needles, count, nots, not_count, mode, label);
	}

	void *resolve_cstr_ex(const char *const *needles, int count,
	                      const char *const *nots, int not_count,
	                      MatchMode mode, const char *label)
	{
		return resolve_impl(needles, count, nots, not_count, mode, label);
	}

	void *resolve_wstr(const wchar_t *needle, const char *label)
	{
		if (!needle)
			return nullptr;
		return resolve_wstr_ex(&needle, 1, nullptr, 0, MatchMode::Any, label);
	}

	void *resolve_wstr_any(const wchar_t *const *needles, int count,
	                       const char *label)
	{
		return resolve_wstr_ex(needles, count, nullptr, 0, MatchMode::Any,
		                       label);
	}

	void *resolve_wstr_all(const wchar_t *const *needles, int count,
	                       const char *label)
	{
		return resolve_wstr_ex(needles, count, nullptr, 0, MatchMode::All,
		                       label);
	}

	void *resolve_wstr_any_not(const wchar_t *const *needles, int count,
	                           const wchar_t *const *nots, int not_count,
	                           const char *label)
	{
		return resolve_wstr_ex(needles, count, nots, not_count, MatchMode::Any,
		                       label);
	}

	void *resolve_wstr_all_not(const wchar_t *const *needles, int count,
	                           const wchar_t *const *nots, int not_count,
	                           const char *label)
	{
		return resolve_wstr_ex(needles, count, nots, not_count, MatchMode::All,
		                       label);
	}

	void *resolve_cstr(const char *needle, const char *label)
	{
		if (!needle)
			return nullptr;
		return resolve_cstr_ex(&needle, 1, nullptr, 0, MatchMode::Any, label);
	}

	void *resolve_cstr_any(const char *const *needles, int count,
	                       const char *label)
	{
		return resolve_cstr_ex(needles, count, nullptr, 0, MatchMode::Any,
		                       label);
	}

	void *resolve_cstr_all(const char *const *needles, int count,
	                       const char *label)
	{
		return resolve_cstr_ex(needles, count, nullptr, 0, MatchMode::All,
		                       label);
	}

	void *resolve_cstr_all_not(const char *const *needles, int count,
	                           const char *const *nots, int not_count,
	                           const char *label)
	{
		return resolve_cstr_ex(needles, count, nots, not_count, MatchMode::All,
		                       label);
	}

	void reset_cache()
	{
		g_set_cache.clear();
		g_result_cache.clear();
		anchor::reset_xref_index();
	}

	namespace
	{
		void *g_engine_free = nullptr;
		bool g_warned_leak = false;
	}  // namespace

	void set_engine_free(void *app_free_fn)
	{
		g_engine_free = app_free_fn;
		log_info("ue3_api: engine_free set to %p", app_free_fn);
	}

	void engine_free(void *ptr)
	{
		if (!ptr)
			return;
		if (g_engine_free)
		{
			reinterpret_cast<void(__cdecl *)(void *)>(g_engine_free)(ptr);
			return;
		}
		if (!g_warned_leak)
		{
			g_warned_leak = true;
			log_warn("ue3_api: engine_free not set - leaking engine buffers");
		}
	}

	namespace
	{
		struct OdView
		{
			const void *vtable;
			int32_t bAllowSuppression;
			int32_t bSuppressEventTag;
			int32_t bAutoEmitLineTerminator;
			CaptureOutputDevice *owner;
		};

		inline void od_do_serialize(void *self, const wchar_t *v)
		{
			auto *view = static_cast<OdView *>(self);
			if (view && view->owner && v)
				view->owner->on_serialize(v);
		}

#ifdef _WIN64
		void od_serialize(void *self, const wchar_t *v, int /*Event*/)
		{
			od_do_serialize(self, v);
		}

		void *od_dtor(void *self, unsigned int) { return self; }

		void od_flush(void *) {}

		void od_teardown(void *) {}
#else
		void __fastcall od_serialize(void *self, void * /*edx*/,
		                             const wchar_t *v, int /*Event*/)
		{
			od_do_serialize(self, v);
		}

		void *__fastcall od_dtor(void *self, void * /*edx*/,
		                         unsigned int /*flags*/)
		{
			return self;
		}

		void __fastcall od_flush(void * /*self*/, void * /*edx*/) {}

		void __fastcall od_teardown(void * /*self*/, void * /*edx*/) {}
#endif

		// MSVC-order vtable: [dtor][Serialize][Flush][TearDown].
		const void *const g_od_vtable[4] = {
		    reinterpret_cast<const void *>(&od_dtor),
		    reinterpret_cast<const void *>(&od_serialize),
		    reinterpret_cast<const void *>(&od_flush),
		    reinterpret_cast<const void *>(&od_teardown),
		};
	}  // namespace

	CaptureOutputDevice::CaptureOutputDevice()
	{
		layout_.vtable = g_od_vtable;
		layout_.bAllowSuppression = 0;
		layout_.bSuppressEventTag = 1;
		layout_.bAutoEmitLineTerminator = 1;
		layout_.owner = this;
		buf_.reserve(256);
	}

	void CaptureOutputDevice::on_serialize(const wchar_t *v)
	{
		int n = WideCharToMultiByte(CP_UTF8, 0, v, -1, nullptr, 0, nullptr,
		                            nullptr);
		if (n <= 1)
			return;
		std::string tmp(static_cast<size_t>(n - 1), '\0');
		WideCharToMultiByte(CP_UTF8, 0, v, -1, tmp.data(), n, nullptr, nullptr);
		buf_ += tmp;
		if (!buf_.empty() && buf_.back() != '\n')
			buf_ += '\n';
	}

}  // namespace ue3_api
