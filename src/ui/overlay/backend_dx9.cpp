#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "overlay.hpp"

#include "hook.hpp"
#include "logs.hpp"
#include "ui/theme.hpp"

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#include <atomic>
#include <d3d9.h>
#include <windows.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd,
                                                             UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

namespace
{
	using EndSceneFn = HRESULT(WINAPI *)(IDirect3DDevice9 *);
	EndSceneFn g_orig_endscene = nullptr;

	using ResetFn = HRESULT(WINAPI *)(IDirect3DDevice9 *,
	                                  D3DPRESENT_PARAMETERS *);
	ResetFn g_orig_reset = nullptr;

	WNDPROC g_orig_wndproc = nullptr;
	HWND g_hwnd = nullptr;
	std::atomic<bool> g_imgui_init{false};

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

	bool init_imgui(IDirect3DDevice9 *dev)
	{
		if (!overlay::try_claim_backend("dx9"))
			return false;
		D3DDEVICE_CREATION_PARAMETERS cp{};
		if (FAILED(dev->GetCreationParameters(&cp)))
			return false;

		g_hwnd = cp.hFocusWindow;
		if (!g_hwnd)
			return false;

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGui::GetIO().IniFilename = nullptr;

		theme::load_fonts();
		theme::apply();

		if (!ImGui_ImplWin32_Init(g_hwnd))
			return false;
		if (!ImGui_ImplDX9_Init(dev))
			return false;

		g_orig_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
		    g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wndproc_hook)));

		g_imgui_init.store(true);
		overlay::on_backend_ready();
		return true;
	}

	void shutdown_imgui()
	{
		if (!g_imgui_init.load())
			return;
		g_imgui_init.store(false);

		ImGui_ImplDX9_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();

		if (g_hwnd && g_orig_wndproc)
			SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC,
			                  reinterpret_cast<LONG_PTR>(g_orig_wndproc));
		g_orig_wndproc = nullptr;
		g_hwnd = nullptr;
	}

	HRESULT WINAPI hk_endscene(IDirect3DDevice9 *dev)
	{
		if (!g_imgui_init.load())
		{
			if (!init_imgui(dev))
				return g_orig_endscene(dev);
		}

		ImGui_ImplDX9_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		overlay::render();

		ImGui::EndFrame();
		ImGui::Render();
		ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());

		return g_orig_endscene(dev);
	}

	HRESULT WINAPI hk_reset(IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pp)
	{
		ImGui_ImplDX9_InvalidateDeviceObjects();
		HRESULT hr = g_orig_reset(dev, pp);
		if (SUCCEEDED(hr))
			ImGui_ImplDX9_CreateDeviceObjects();
		return hr;
	}

	void *get_vtable_fn(int index)
	{
		WNDCLASSEXW wc{};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = DefWindowProcW;
		wc.lpszClassName = L"cu3ml_dx9_dummy";
		wc.hInstance = GetModuleHandleW(nullptr);
		RegisterClassExW(&wc);

		HWND hw =
		    CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 4, 4,
		                    nullptr, nullptr, wc.hInstance, nullptr);
		if (!hw)
			return nullptr;

		IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
		if (!d3d)
		{
			DestroyWindow(hw);
			UnregisterClassW(wc.lpszClassName, wc.hInstance);
			return nullptr;
		}

		D3DPRESENT_PARAMETERS pp{};
		pp.Windowed = TRUE;
		pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
		pp.hDeviceWindow = hw;

		IDirect3DDevice9 *dev = nullptr;
		HRESULT hr =
		    d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, hw,
		                      D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
		void *fn = nullptr;
		if (SUCCEEDED(hr) && dev)
		{
			void **vtbl = *reinterpret_cast<void ***>(dev);
			fn = vtbl[index];
			dev->Release();
		}
		d3d->Release();
		DestroyWindow(hw);
		UnregisterClassW(wc.lpszClassName, wc.hInstance);
		return fn;
	}
}  // namespace

namespace overlay_dx9
{
	bool install()
	{
		if (!GetModuleHandleW(L"d3d9.dll"))
			return false;

		void *endscene = get_vtable_fn(42);
		void *reset = get_vtable_fn(16);

		if (!endscene)
		{
			log_warn("overlay_dx9: EndScene vtable lookup failed");
			return false;
		}

		hook::add(endscene, reinterpret_cast<void *>(&hk_endscene),
		          reinterpret_cast<void **>(&g_orig_endscene));

		if (reset)
			hook::add(reset, reinterpret_cast<void *>(&hk_reset),
			          reinterpret_cast<void **>(&g_orig_reset));

		hook::install_all();

		if (!g_orig_endscene)
		{
			log_err("overlay_dx9: hook install failed");
			return false;
		}

		log_info("overlay_dx9: hooked EndScene=%p Reset=%p", endscene, reset);
		return true;
	}

	void remove() { shutdown_imgui(); }
}  // namespace overlay_dx9
