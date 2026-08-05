#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "overlay.hpp"

#include "hook.hpp"
#include "logs.hpp"
#include "ui/theme.hpp"

#include "imgui.h"
#include "imgui_impl_dx10.h"
#include "imgui_impl_win32.h"

#include <atomic>
#include <d3d10.h>
#include <dxgi.h>
#include <windows.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd,
                                                             UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

namespace
{
	using PresentFn = HRESULT(WINAPI *)(IDXGISwapChain *, UINT, UINT);
	using ResizeBuffersFn = HRESULT(WINAPI *)(IDXGISwapChain *, UINT, UINT,
	                                          UINT, DXGI_FORMAT, UINT);

	PresentFn g_orig_present = nullptr;
	ResizeBuffersFn g_orig_resize = nullptr;

	WNDPROC g_orig_wndproc = nullptr;
	HWND g_hwnd = nullptr;

	ID3D10Device *g_device = nullptr;
	ID3D10RenderTargetView *g_rtv = nullptr;
	IDXGISwapChain *g_game_sc = nullptr;

	std::atomic<bool> g_imgui_init{false};
	std::atomic<bool> g_vtable_hooked{false};

	LRESULT CALLBACK wndproc_hook(HWND hWnd, UINT msg, WPARAM wParam,
	                              LPARAM lParam)
	{
		if (msg == WM_KEYDOWN && wParam == VK_INSERT)
		{
			overlay::toggle();
			return 0;
		}

		if (overlay::visible())
		{
			ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
			if (ImGui::GetIO().WantCaptureMouse ||
			    ImGui::GetIO().WantCaptureKeyboard)
				return 0;
		}

		return CallWindowProcW(g_orig_wndproc, hWnd, msg, wParam, lParam);
	}

	void create_rtv(IDXGISwapChain *sc)
	{
		ID3D10Texture2D *back = nullptr;
		if (SUCCEEDED(
		        sc->GetBuffer(0, __uuidof(ID3D10Texture2D), (void **)&back)))
		{
			g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
			back->Release();
		}
	}

	void cleanup_rtv()
	{
		if (g_rtv)
		{
			g_rtv->Release();
			g_rtv = nullptr;
		}
	}

	bool init_imgui(IDXGISwapChain *sc)
	{
		if (!overlay::try_claim_backend("dx10"))
			return false;
		DXGI_SWAP_CHAIN_DESC desc{};
		if (FAILED(sc->GetDesc(&desc)))
		{
			log_err("overlay_dx10: GetDesc failed");
			return false;
		}

		g_hwnd = desc.OutputWindow;
		if (!g_hwnd)
		{
			log_err("overlay_dx10: no OutputWindow");
			return false;
		}

		if (FAILED(sc->GetDevice(__uuidof(ID3D10Device), (void **)&g_device)))
		{
			log_err("overlay_dx10: GetDevice failed");
			return false;
		}

		create_rtv(sc);

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGui::GetIO().IniFilename = nullptr;

		theme::load_fonts();
		theme::apply();

		if (!ImGui_ImplWin32_Init(g_hwnd))
		{
			log_err("overlay_dx10: ImGui_ImplWin32_Init failed");
			return false;
		}
		if (!ImGui_ImplDX10_Init(g_device))
		{
			log_err("overlay_dx10: ImGui_ImplDX10_Init failed");
			return false;
		}

		g_orig_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
		    g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wndproc_hook)));

		g_imgui_init.store(true);
		overlay::on_backend_ready();
		log_info("overlay_dx10: imgui ready  hwnd=%p dev=%p", (void *)g_hwnd,
		         (void *)g_device);
		return true;
	}

	void shutdown_imgui()
	{
		if (!g_imgui_init.load())
			return;
		g_imgui_init.store(false);

		ImGui_ImplDX10_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();

		cleanup_rtv();
		if (g_device)
		{
			g_device->Release();
			g_device = nullptr;
		}

		if (g_hwnd && g_orig_wndproc)
			SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC,
			                  reinterpret_cast<LONG_PTR>(g_orig_wndproc));
		g_orig_wndproc = nullptr;
		g_hwnd = nullptr;
	}

	HRESULT WINAPI hk_present(IDXGISwapChain *sc, UINT sync, UINT flags)
	{
		if (!g_imgui_init.load())
		{
			if (!init_imgui(sc))
				return g_orig_present(sc, sync, flags);
		}

		ImGui_ImplDX10_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		overlay::render();

		ImGui::EndFrame();
		ImGui::Render();

		if (g_rtv)
			g_device->OMSetRenderTargets(1, &g_rtv, nullptr);

		ImGui_ImplDX10_RenderDrawData(ImGui::GetDrawData());

		return g_orig_present(sc, sync, flags);
	}

	HRESULT WINAPI hk_resize(IDXGISwapChain *sc, UINT count, UINT w, UINT h,
	                         DXGI_FORMAT fmt, UINT flags)
	{
		cleanup_rtv();
		HRESULT hr = g_orig_resize(sc, count, w, h, fmt, flags);
		if (SUCCEEDED(hr) && g_imgui_init.load())
			create_rtv(sc);
		return hr;
	}

	void hook_swapchain_vtable(IDXGISwapChain *sc)
	{
		if (g_vtable_hooked.exchange(true))
			return;

		g_game_sc = sc;
		void **vtbl = *reinterpret_cast<void ***>(sc);

		void *present_fn = vtbl[8];
		void *resize_fn = vtbl[13];

		log_info("overlay_dx10: game swapchain vtable  Present=%p "
		         "ResizeBuffers=%p",
		         present_fn, resize_fn);

		hook::add(present_fn, reinterpret_cast<void *>(&hk_present),
		          reinterpret_cast<void **>(&g_orig_present));
		hook::add(resize_fn, reinterpret_cast<void *>(&hk_resize),
		          reinterpret_cast<void **>(&g_orig_resize));
		hook::install_all();

		if (g_orig_present)
			log_info("overlay_dx10: Present hook live");
		else
			log_err("overlay_dx10: Present hook failed");
	}

	using CreateDevAndSC_t = HRESULT(WINAPI *)(IDXGIAdapter *,
	                                           D3D10_DRIVER_TYPE, HMODULE, UINT,
	                                           UINT, DXGI_SWAP_CHAIN_DESC *,
	                                           IDXGISwapChain **,
	                                           ID3D10Device **);

	CreateDevAndSC_t g_orig_create = nullptr;

	HRESULT WINAPI hk_create(IDXGIAdapter *pAdapter, D3D10_DRIVER_TYPE dt,
	                         HMODULE sw, UINT flags, UINT sdkVer,
	                         DXGI_SWAP_CHAIN_DESC *pSCDesc,
	                         IDXGISwapChain **ppSC, ID3D10Device **ppDev)
	{
		HRESULT hr = g_orig_create(pAdapter, dt, sw, flags, sdkVer, pSCDesc,
		                           ppSC, ppDev);

		if (SUCCEEDED(hr) && ppSC && *ppSC)
		{
			log_info("overlay_dx10: intercepted game swapchain %p", *ppSC);
			hook_swapchain_vtable(*ppSC);
		}

		return hr;
	}
}  // namespace

namespace overlay_dx10
{
	bool install()
	{
		HMODULE d3d10 = GetModuleHandleW(L"d3d10.dll");
		if (!d3d10)
			return false;

		void *create_fn =
		    (void *)GetProcAddress(d3d10, "D3D10CreateDeviceAndSwapChain");

		if (!create_fn)
		{
			log_warn("overlay_dx10: D3D10CreateDeviceAndSwapChain not found");
			return false;
		}

		hook::add(create_fn, reinterpret_cast<void *>(&hk_create),
		          reinterpret_cast<void **>(&g_orig_create));
		hook::install_all();

		if (!g_orig_create)
		{
			log_err("overlay_dx10: hook install failed");
			return false;
		}

		log_info("overlay_dx10: hooked D3D10CreateDeviceAndSwapChain=%p",
		         create_fn);
		return true;
	}

	void remove() { shutdown_imgui(); }
}  // namespace overlay_dx10
