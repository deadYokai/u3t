#define WIN32_LEAN_AND_MEAN
#include "logs.hpp"
#include "overlay.hpp"
#include <atomic>
#include <windows.h>

namespace overlay_dx11
{
	bool install();
	void remove();
}  // namespace overlay_dx11

namespace overlay_dx10
{
	bool install();
	void remove();
}  // namespace overlay_dx10

namespace overlay_dx9
{
	bool install();
	void remove();
}  // namespace overlay_dx9

namespace overlay
{

	static std::atomic<bool> g_dx9{false};
	static std::atomic<bool> g_dx10{false};
	static std::atomic<bool> g_dx11{false};

	void init()
	{
		int n = 0;

		if (overlay_dx10::install())
		{
			g_dx10.store(true);
			++n;
		}
		if (overlay_dx11::install())
		{
			g_dx11.store(true);
			++n;
		}
		if (overlay_dx9::install())
		{
			g_dx9.store(true);
			++n;
		}

		if (n)
			log_info("overlay: %d backend(s) armed", n);
		else
			log_warn("overlay: no backends installed");
	}

	void shutdown()
	{
		if (g_dx11.load())
			overlay_dx11::remove();
		if (g_dx10.load())
			overlay_dx10::remove();
		if (g_dx9.load())
			overlay_dx9::remove();

		g_dx9.store(false);
		g_dx10.store(false);
		g_dx11.store(false);
		clear_state();
		log_info("overlay: shutdown");
	}
}  // namespace overlay
