#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "overlay.hpp"

#include "hook.hpp"
#include "logs.hpp"
#include "ui/theme.hpp"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <atomic>
#include <d3d11.h>
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

	ID3D11Device *g_device = nullptr;
	ID3D11DeviceContext *g_context = nullptr;
	ID3D11RenderTargetView *g_rtv = nullptr;

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
		ID3D11Texture2D *back = nullptr;
		if (SUCCEEDED(
		        sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back)))
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
		DXGI_SWAP_CHAIN_DESC desc{};
		if (FAILED(sc->GetDesc(&desc)))
		{
			log_err("overlay_dx11: GetDesc failed");
			return false;
		}

		g_hwnd = desc.OutputWindow;
		if (!g_hwnd)
		{
			log_err("overlay_dx11: no OutputWindow");
			return false;
		}

		if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void **)&g_device)))
		{
			log_err("overlay_dx11: GetDevice failed");
			return false;
		}

		g_device->GetImmediateContext(&g_context);
		create_rtv(sc);

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGui::GetIO().IniFilename = nullptr;

		theme::load_fonts();
		theme::apply();

		if (!ImGui_ImplWin32_Init(g_hwnd))
		{
			log_err("overlay_dx11: ImGui_ImplWin32_Init failed");
			return false;
		}
		if (!ImGui_ImplDX11_Init(g_device, g_context))
		{
			log_err("overlay_dx11: ImGui_ImplDX11_Init failed");
			return false;
		}

		g_orig_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
		    g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wndproc_hook)));

		g_imgui_init.store(true);
		overlay::on_backend_ready();
		log_info("overlay_dx11: imgui ready  hwnd=%p dev=%p", (void *)g_hwnd,
		         (void *)g_device);
		return true;
	}

	void shutdown_imgui()
	{
		if (!g_imgui_init.load())
			return;
		g_imgui_init.store(false);

		ImGui_ImplDX11_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();

		cleanup_rtv();
		if (g_context)
		{
			g_context->Release();
			g_context = nullptr;
		}
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

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		overlay::render();

		ImGui::EndFrame();
		ImGui::Render();

		if (g_rtv)
			g_context->OMSetRenderTargets(1, &g_rtv, nullptr);

		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

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

		log_info("overlay_dx11: game swapchain vtable  Present=%p "
		         "ResizeBuffers=%p",
		         present_fn, resize_fn);

		hook::add(present_fn, reinterpret_cast<void *>(&hk_present),
		          reinterpret_cast<void **>(&g_orig_present));
		hook::add(resize_fn, reinterpret_cast<void *>(&hk_resize),
		          reinterpret_cast<void **>(&g_orig_resize));
		hook::install_all();

		if (g_orig_present)
			log_info("overlay_dx11: Present hook live");
		else
			log_err("overlay_dx11: Present hook failed");
	}

	using CreateDevAndSC_t = HRESULT(WINAPI *)(
	    IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
	    const D3D_FEATURE_LEVEL *, UINT, UINT, const DXGI_SWAP_CHAIN_DESC *,
	    IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *,
	    ID3D11DeviceContext **);

	CreateDevAndSC_t g_orig_create = nullptr;

	HRESULT WINAPI hk_create(IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE dt,
	                         HMODULE sw, UINT flags,
	                         const D3D_FEATURE_LEVEL *pFL, UINT nFL,
	                         UINT sdkVer, const DXGI_SWAP_CHAIN_DESC *pSCDesc,
	                         IDXGISwapChain **ppSC, ID3D11Device **ppDev,
	                         D3D_FEATURE_LEVEL *pFL_out,
	                         ID3D11DeviceContext **ppCtx)
	{
		HRESULT hr = g_orig_create(pAdapter, dt, sw, flags, pFL, nFL, sdkVer,
		                           pSCDesc, ppSC, ppDev, pFL_out, ppCtx);

		if (SUCCEEDED(hr) && ppSC && *ppSC)
		{
			log_info("overlay_dx11: intercepted game swapchain %p", *ppSC);
			hook_swapchain_vtable(*ppSC);
		}

		return hr;
	}

	using FactoryCreateSC_t = HRESULT(WINAPI *)(IDXGIFactory *, IUnknown *,
	                                            DXGI_SWAP_CHAIN_DESC *,
	                                            IDXGISwapChain **);

	FactoryCreateSC_t g_orig_factory_create_sc = nullptr;

	HRESULT WINAPI hk_factory_create_sc(IDXGIFactory *factory, IUnknown *dev,
	                                    DXGI_SWAP_CHAIN_DESC *desc,
	                                    IDXGISwapChain **ppSC)
	{
		HRESULT hr = g_orig_factory_create_sc(factory, dev, desc, ppSC);

		if (SUCCEEDED(hr) && ppSC && *ppSC)
		{
			log_info("overlay_dx11: intercepted factory swapchain %p", *ppSC);
			hook_swapchain_vtable(*ppSC);
		}
		return hr;
	}

	void hook_factory_create_swapchain()
	{
		IDXGIFactory *factory = nullptr;
		if (FAILED(
		        CreateDXGIFactory(__uuidof(IDXGIFactory), (void **)&factory)))
			return;

		void **vtbl = *reinterpret_cast<void ***>(factory);
		void *create_sc = vtbl[10];  // IDXGIFactory::CreateSwapChain
		factory->Release();

		if (!create_sc)
			return;

		hook::add(create_sc, reinterpret_cast<void *>(&hk_factory_create_sc),
		          reinterpret_cast<void **>(&g_orig_factory_create_sc));

		log_info("overlay_dx11: hooked IDXGIFactory::CreateSwapChain=%p",
		         create_sc);
	}
}  // namespace

namespace overlay_dx11
{
	bool install()
	{
		HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
		if (!d3d11)
			return false;

		void *create_fn =
		    (void *)GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain");
		if (create_fn)
		{
			hook::add(create_fn, reinterpret_cast<void *>(&hk_create),
			          reinterpret_cast<void **>(&g_orig_create));
		}

		hook_factory_create_swapchain();

		hook::install_all();

		bool ok = g_orig_create || g_orig_factory_create_sc;
		if (ok)
			log_info("overlay_dx11: creation hooks installed  "
			         "D3D11CreateDeviceAndSwapChain=%s "
			         "Factory::CreateSwapChain=%s",
			         g_orig_create ? "yes" : "no",
			         g_orig_factory_create_sc ? "yes" : "no");
		else
			log_err("overlay_dx11: no creation hooks installed");

		return ok;
	}

	void remove() { shutdown_imgui(); }
}  // namespace overlay_dx11
