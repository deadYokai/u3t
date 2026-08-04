#define WIN32_LEAN_AND_MEAN
#include "logs.hpp"
#include "overlay.hpp"
#include <windows.h>

namespace overlay_dx11
{
	bool install();
	void remove();
}  // namespace overlay_dx11

namespace overlay_dx9
{
	bool install();
	void remove();
}  // namespace overlay_dx9

namespace overlay
{
	enum class Backend
	{
		None,
		DX9,
		DX11
	};
	static Backend g_backend = Backend::None;

	void init()
	{
		if (g_backend != Backend::None)
			return;

		if (overlay_dx11::install())
		{
			g_backend = Backend::DX11;
			log_info("overlay: using DX11 backend");
			return;
		}

		if (overlay_dx9::install())
		{
			g_backend = Backend::DX9;
			log_info("overlay: using DX9 backend");
			return;
		}

		log_warn("overlay: no DX backend available "
		         "(neither d3d9.dll nor d3d11.dll loaded)");
	}

	void shutdown()
	{
		switch (g_backend)
		{
			case Backend::DX11:
				overlay_dx11::remove();
				break;
			case Backend::DX9:
				overlay_dx9::remove();
				break;
			default:
				break;
		}
		g_backend = Backend::None;
		clear_state();
		log_info("overlay: shutdown");
	}
}  // namespace overlay
