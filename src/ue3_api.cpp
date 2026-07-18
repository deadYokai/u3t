#define WIN32_LEAN_AND_MEAN
#include "ue3_api.hpp"

#include "anchor.hpp"
#include "logs.hpp"

#include <string>
#include <unordered_map>

#include <windows.h>

namespace ue3_api
{
	std::unordered_map<std::wstring, void *> g_cache;

	void *resolve_wstr(const wchar_t *needle, const char *label)
	{
		if (!needle)
			return nullptr;

		auto it = g_cache.find(needle);
		if (it != g_cache.end())
			return it->second;

		anchor::ModuleImage img = anchor::image_of(nullptr);
		void *fn = nullptr;
		if (!img.ok)
			log_warn("ue3_api: bad PE image resolving '%s'", label);
		else
			fn = anchor::only(anchor::functions_referencing_wstr(img, needle),
			                  label);

		g_cache.emplace(needle, fn);
		if (fn)
			log_info("ue3_api: resolved '%s' = %p", label, fn);
		return fn;
	}

	void *resolve_wstr_any(const wchar_t *const *needles, int count,
	                       const char *label)
	{
		for (int i = 0; i < count; ++i)
		{
			void *fn = resolve_wstr(needles[i], label);
			if (fn)
				return fn;
		}
		log_warn("ue3_api: '%s' - no candidate anchor matched", label);
		return nullptr;
	}

	void reset_cache() { g_cache.clear(); }

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

		void *od_dtor(void *self, unsigned int /*flags*/) { return self; }

		void od_flush(void * /*self*/) {}

		void od_teardown(void * /*self*/) {}
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
