#define WIN32_LEAN_AND_MEAN
#include "gl_ctx.hpp"
#include "logs.hpp"
#include <windows.h>

#include "imgui.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd,
                                                             UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

namespace gl_ctx
{
	Funcs gl{};
}

namespace
{
	const wchar_t *kClass = L"CU3ML_Manager";

	using PFN_wglCreateContextAttribsARB = HGLRC(WINAPI *)(HDC, HGLRC,
	                                                       const int *);
	using PFN_wglSwapIntervalEXT = BOOL(WINAPI *)(int);

	PFN_wglSwapIntervalEXT g_swap_interval = nullptr;

	LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
	{
		if (ImGui_ImplWin32_WndProcHandler(h, m, w, l))
			return 1;

		auto *win = reinterpret_cast<gl_ctx::Window *>(
		    GetWindowLongPtrW(h, GWLP_USERDATA));

		switch (m)
		{
			case WM_SIZE:
				if (win && w != SIZE_MINIMIZED)
				{
					win->width = LOWORD(l);
					win->height = HIWORD(l);
				}
				return 0;

			case WM_CLOSE:
				if (win)
					win->quit = true;
				return 0;

			case WM_DESTROY:
				PostQuitMessage(0);
				return 0;

			case WM_ERASEBKGND:
				return 1;
		}
		return DefWindowProcW(h, m, w, l);
	}

	bool wParamIsNotMinimized(WPARAM w) { return w != SIZE_MINIMIZED; }

	void *gl_proc(const char *name)
	{
		void *p = (void *)wglGetProcAddress(name);
		if (p == nullptr || p == (void *)0x1 || p == (void *)0x2 ||
		    p == (void *)0x3 || p == (void *)-1)
		{
			static HMODULE lib = LoadLibraryA("opengl32.dll");
			p = lib ? (void *)GetProcAddress(lib, name) : nullptr;
		}
		return p;
	}

	PFN_wglCreateContextAttribsARB probe_arb()
	{
		WNDCLASSEXW wc{};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = DefWindowProcW;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = L"CU3ML_GLProbe";
		RegisterClassExW(&wc);

		HWND h =
		    CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0,
		                    1, 1, nullptr, nullptr, wc.hInstance, nullptr);
		if (!h)
			return nullptr;

		HDC dc = GetDC(h);
		PIXELFORMATDESCRIPTOR pfd{};
		pfd.nSize = sizeof(pfd);
		pfd.nVersion = 1;
		pfd.dwFlags =
		    PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
		pfd.iPixelType = PFD_TYPE_RGBA;
		pfd.cColorBits = 32;
		pfd.cDepthBits = 24;

		int fmt = ChoosePixelFormat(dc, &pfd);
		SetPixelFormat(dc, fmt, &pfd);

		HGLRC rc = wglCreateContext(dc);
		wglMakeCurrent(dc, rc);

		auto fn = reinterpret_cast<PFN_wglCreateContextAttribsARB>(
		    wglGetProcAddress("wglCreateContextAttribsARB"));

		wglMakeCurrent(nullptr, nullptr);
		wglDeleteContext(rc);
		ReleaseDC(h, dc);
		DestroyWindow(h);
		UnregisterClassW(wc.lpszClassName, wc.hInstance);
		return fn;
	}

	bool load_funcs()
	{
		using namespace gl_ctx;
		bool ok = true;
		auto need = [&](void **slot, const char *n)
		{
			*slot = gl_proc(n);
			if (!*slot)
			{
				log_err("gl_ctx: missing entry point %s", n);
				ok = false;
			}
		};

#define NEED(field, name) need((void **)&gl.field, name)
		NEED(GenFramebuffers, "glGenFramebuffers");
		NEED(BindFramebuffer, "glBindFramebuffer");
		NEED(FramebufferTexture2D, "glFramebufferTexture2D");
		NEED(CheckFramebufferStatus, "glCheckFramebufferStatus");
		NEED(DeleteFramebuffers, "glDeleteFramebuffers");
		NEED(CreateShader, "glCreateShader");
		NEED(ShaderSource, "glShaderSource");
		NEED(CompileShader, "glCompileShader");
		NEED(GetShaderiv, "glGetShaderiv");
		NEED(GetShaderInfoLog, "glGetShaderInfoLog");
		NEED(DeleteShader, "glDeleteShader");
		NEED(CreateProgram, "glCreateProgram");
		NEED(AttachShader, "glAttachShader");
		NEED(LinkProgram, "glLinkProgram");
		NEED(GetProgramiv, "glGetProgramiv");
		NEED(GetProgramInfoLog, "glGetProgramInfoLog");
		NEED(DeleteProgram, "glDeleteProgram");
		NEED(UseProgram, "glUseProgram");
		NEED(GetUniformLocation, "glGetUniformLocation");
		NEED(Uniform1i, "glUniform1i");
		NEED(Uniform1f, "glUniform1f");
		NEED(Uniform2f, "glUniform2f");
		NEED(GenVertexArrays, "glGenVertexArrays");
		NEED(BindVertexArray, "glBindVertexArray");
		NEED(DeleteVertexArrays, "glDeleteVertexArrays");
		NEED(ActiveTexture, "glActiveTexture");
#undef NEED
		return ok;
	}
}  // namespace

namespace gl_ctx
{
	bool create(Window &w, const wchar_t *title, int width, int height)
	{
		auto create_attribs = probe_arb();
		if (!create_attribs)
		{
			log_err("gl_ctx: wglCreateContextAttribsARB unavailable - "
			        "GL 3.3 core not supported by this driver");
			return false;
		}

		HINSTANCE inst = GetModuleHandleW(nullptr);

		WNDCLASSEXW wc{};
		wc.cbSize = sizeof(wc);
		wc.style = CS_OWNDC;
		wc.lpfnWndProc = wndproc;
		wc.hInstance = inst;
		wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
		wc.hbrBackground = nullptr;
		wc.lpszClassName = kClass;
		RegisterClassExW(&wc);

		RECT r{0, 0, width, height};
		AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

		int sx = (GetSystemMetrics(SM_CXSCREEN) - (r.right - r.left)) / 2;
		int sy = (GetSystemMetrics(SM_CYSCREEN) - (r.bottom - r.top)) / 2;

		w.hwnd = CreateWindowExW(0, kClass, title, WS_OVERLAPPEDWINDOW, sx, sy,
		                         r.right - r.left, r.bottom - r.top, nullptr,
		                         nullptr, inst, nullptr);
		if (!w.hwnd)
		{
			log_err("gl_ctx: CreateWindowExW failed (err=%lu)", GetLastError());
			return false;
		}
		SetWindowLongPtrW(w.hwnd, GWLP_USERDATA, (LONG_PTR)&w);

		w.hdc = GetDC(w.hwnd);

		PIXELFORMATDESCRIPTOR pfd{};
		pfd.nSize = sizeof(pfd);
		pfd.nVersion = 1;
		pfd.dwFlags =
		    PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
		pfd.iPixelType = PFD_TYPE_RGBA;
		pfd.cColorBits = 32;
		pfd.cAlphaBits = 8;

		int fmt = ChoosePixelFormat(w.hdc, &pfd);
		if (!fmt || !SetPixelFormat(w.hdc, fmt, &pfd))
		{
			log_err("gl_ctx: SetPixelFormat failed (err=%lu)", GetLastError());
			destroy(w);
			return false;
		}

		const int attribs[] = {0x2091 /* MAJOR_VERSION_ARB */,
		                       3,
		                       0x2092 /* MINOR_VERSION_ARB */,
		                       3,
		                       0x9126 /* PROFILE_MASK_ARB  */,
		                       0x00000001 /* CORE_PROFILE */,
		                       0};

		w.hglrc = create_attribs(w.hdc, nullptr, attribs);
		if (!w.hglrc)
		{
			log_err("gl_ctx: GL 3.3 core context creation failed");
			destroy(w);
			return false;
		}
		wglMakeCurrent(w.hdc, w.hglrc);

		if (!load_funcs())
		{
			destroy(w);
			return false;
		}

		g_swap_interval = reinterpret_cast<PFN_wglSwapIntervalEXT>(
		    wglGetProcAddress("wglSwapIntervalEXT"));
		if (g_swap_interval)
			g_swap_interval(1);

		w.width = width;
		w.height = height;

		log_info("gl_ctx: GL %s / %s", (const char *)glGetString(GL_VERSION),
		         (const char *)glGetString(GL_RENDERER));

		ShowWindow(w.hwnd, SW_SHOW);
		SetForegroundWindow(w.hwnd);
		SetActiveWindow(w.hwnd);
		return true;
	}

	void destroy(Window &w)
	{
		if (w.hglrc)
		{
			wglMakeCurrent(nullptr, nullptr);
			wglDeleteContext(w.hglrc);
			w.hglrc = nullptr;
		}
		if (w.hdc && w.hwnd)
		{
			ReleaseDC(w.hwnd, w.hdc);
			w.hdc = nullptr;
		}
		if (w.hwnd)
		{
			DestroyWindow(w.hwnd);
			w.hwnd = nullptr;
		}
		UnregisterClassW(kClass, GetModuleHandleW(nullptr));
	}

	void pump(Window &w)
	{
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
			if (msg.message == WM_QUIT)
				w.quit = true;
		}
	}

	void present(Window &w) { SwapBuffers(w.hdc); }
}  // namespace gl_ctx
