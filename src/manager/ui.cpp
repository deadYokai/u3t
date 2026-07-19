#define WIN32_LEAN_AND_MEAN
#include "ui.hpp"

#include "crt_post.hpp"
#include "gl_ctx.hpp"
#include "theme.hpp"

#include "logs.hpp"
#include "mod_loader.hpp"
#include "util.hpp"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"

#include <string>
#include <vector>
#include <windows.h>

namespace
{
	int g_selected = -1;
	std::vector<char> g_enabled;

	std::string exe_name()
	{
		std::wstring p = get_exe_dir();
		wchar_t buf[MAX_PATH]{};
		GetModuleFileNameW(nullptr, buf, MAX_PATH);
		std::wstring full(buf);
		auto s = full.find_last_of(L"/\\");
		return to_narrow(s == std::wstring::npos ? full : full.substr(s + 1));
	}

	void center_text(const char *text)
	{
		float width = ImGui::CalcTextSize(text).x;

		ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
		                     (ImGui::GetContentRegionAvail().x - width) * 0.5f);

		ImGui::TextUnformatted(text);
	}

	void draw_header(ImDrawList *dl, float time)
	{
		ImVec2 p = ImGui::GetCursorScreenPos();
		float a = theme::flicker_text_alpha(time);
		ImGui::PushFont(theme::fonts.display);
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.000f, 0.416f, 0.000f, a));
		center_text("CU3ML");
		ImGui::PopStyleColor();
		ImGui::PopFont();

		ImGui::PushFont(theme::fonts.mono);
		ImGui::PushStyleColor(ImGuiCol_Text, theme::col::o2);
		center_text("CRAPPY UNREAL ENGINE 3 MOD LOADER");
		ImGui::PopStyleColor();
		ImGui::PopFont();

		ImGui::PushFont(theme::fonts.mono);
		ImGui::PushStyleColor(ImGuiCol_Text, theme::col::txt);
		ImGui::TextWrapped(
		    "Engine boot intercepted at GEngineLoop::Init. No packages have "
		    "been loaded and no hooks are committed. Review the mod set, then "
		    "launch or exit.");
		ImGui::PopStyleColor();
		ImGui::PopFont();

		ImGui::Dummy(ImVec2(0, 8));
		theme::badge(sizeof(void *) == 8 ? "X64" : "X86");
		ImGui::SameLine(0, 4);
		theme::badge("MANAGER MODE");
		ImGui::SameLine(0, 4);
		theme::badge("BOOT HALTED");

		(void)dl;
		(void)p;
	}

	void draw_meta(const std::vector<LoadedMod> &mods)
	{
		struct Cell
		{
			const char *k;
			std::string v;
		};

		int enabled = 0;
		for (size_t i = 0; i < mods.size(); ++i)
			if (g_enabled[i])
				++enabled;

		Cell cells[] = {
		    {"MODS", std::to_string(mods.size())},
		    {"ENABLED", std::to_string(enabled)},
		    {"ARCH", sizeof(void *) == 8 ? "x64" : "x86"},
		    {"TARGET", exe_name()},
		};

		float w = ImGui::GetContentRegionAvail().x / 4.0f;
		ImDrawList *dl = ImGui::GetWindowDrawList();

		for (int i = 0; i < 4; ++i)
		{
			ImGui::BeginGroup();
			float cx = ImGui::GetCursorPosX();

			ImGui::PushFont(theme::fonts.small_);
			ImGui::PushStyleColor(ImGuiCol_Text, theme::col::dim);
			ImGui::TextUnformatted(cells[i].k);
			ImGui::PopStyleColor();
			ImGui::PopFont();

			ImGui::SetCursorPosX(cx);
			ImGui::PushFont(theme::fonts.mono);
			ImGui::PushStyleColor(ImGuiCol_Text, theme::col::hi);
			ImGui::TextUnformatted(cells[i].v.c_str());
			ImGui::PopStyleColor();
			ImGui::PopFont();
			ImGui::EndGroup();

			if (i < 3)
			{
				ImVec2 mn = ImGui::GetItemRectMin();
				ImVec2 mx = ImGui::GetItemRectMax();
				float x = mn.x + w - 8.0f;
				dl->AddLine(ImVec2(x, mn.y - 4), ImVec2(x, mx.y + 4),
				            theme::accent(0.07f), 1.0f);
				ImGui::SameLine(0, 0);
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + w -
				                     ImGui::GetItemRectSize().x);
			}
		}
	}

	void draw_mod_row(int i, const LoadedMod &m)
	{
		ImGui::PushID(i);

		bool sel = (g_selected == i);
		char idx[8];
		snprintf(idx, sizeof(idx), "%02d", i + 1);

		float row_h = ImGui::GetTextLineHeight() * 2.2f;

		ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));

		if (ImGui::Selectable("##row", sel, 0, ImVec2(0, row_h)))
			g_selected = i;

		ImGui::PopStyleColor(3);

		bool hovered = ImGui::IsItemHovered();
		ImVec2 p0 = ImGui::GetItemRectMin();
		ImVec2 p1 = ImGui::GetItemRectMax();
		ImDrawList *dl = ImGui::GetWindowDrawList();

		if (hovered || sel)
		{
			float fill = sel ? (hovered ? 0.10f : 0.07f) : 0.06f;
			float edge = sel ? (hovered ? 0.40f : 0.30f) : 0.16f;
			dl->AddRectFilled(p0, p1, theme::accent(fill), 6.0f);
			dl->AddRect(p0, p1, theme::accent(edge), 6.0f, 0, 1.0f);
		}

		float ty = p0.y + (row_h - ImGui::GetTextLineHeight()) * 0.5f;

		dl->AddText(theme::fonts.mono, theme::fonts.mono->LegacySize,
		            ImVec2(p0.x + 14, ty), ImGui::GetColorU32(theme::col::dim),
		            idx);

		std::string nm = m.cfg.name.empty() ? to_narrow(m.dir_w) : m.cfg.name;
		dl->AddText(ImVec2(p0.x + 48, ty), ImGui::GetColorU32(theme::col::hi),
		            nm.c_str());

		std::string meta = m.cfg.version.empty() ? "" : ("v" + m.cfg.version);
		if (!m.cfg.author.empty())
			meta +=
			    (meta.empty() ? "" : "  ") + std::string("· ") + m.cfg.author;
		if (!meta.empty())
		{
			ImVec2 sz = ImGui::CalcTextSize(meta.c_str());
			dl->AddText(ImVec2(p1.x - sz.x - 44, ty),
			            ImGui::GetColorU32(theme::col::dim), meta.c_str());
		}

		dl->AddText(ImVec2(p1.x - 26, ty), theme::accent(hovered ? 1.0f : 0.5f),
		            "->");

		ImGui::PopID();
	}

	void draw_detail(const LoadedMod &m)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, theme::col::txt);
		if (!m.cfg.description.empty())
			ImGui::TextWrapped("%s", m.cfg.description.c_str());
		ImGui::PopStyleColor();

		ImGui::Dummy(ImVec2(0, 6));
		ImGui::PushFont(theme::fonts.mono);

		auto kv = [](const char *k, const std::string &v)
		{
			ImGui::PushStyleColor(ImGuiCol_Text, theme::col::dim);
			ImGui::TextUnformatted(k);
			ImGui::PopStyleColor();
			ImGui::SameLine(150.0f);
			ImGui::PushStyleColor(ImGuiCol_Text, theme::col::hi);

			ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 24.0f);
			ImGui::TextUnformatted(v.empty() ? "-" : v.c_str());
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
		};

		kv("dir", to_narrow(m.dir_w));
		kv("content paths", std::to_string(m.cfg.content_paths.size()));
		kv("spawn patches", std::to_string(m.cfg.spawn_patches.size()));
		kv("replace patches", std::to_string(m.cfg.replace_patches.size()));

		if (!m.cfg.dependencies.empty())
		{
			std::string deps;
			for (const auto &d : m.cfg.dependencies)
				deps += (deps.empty() ? "" : ", ") + d;
			kv("requires", deps);
		}
		ImGui::PopFont();
	}
}  // namespace

namespace ui
{
	Result run()
	{
		const std::vector<LoadedMod> &mods = mod_loader::loaded_mods();
		g_enabled.assign(mods.size(), 1);
		for (size_t i = 0; i < mods.size(); ++i)
			g_enabled[i] = mods[i].cfg.enabled ? 1 : 0;

		gl_ctx::Window win;
		if (!gl_ctx::create(win, L"CU3ML Manager", 720, 1024))
		{
			log_err("manager_ui: window/context creation failed - "
			        "continuing boot");
			return Result::Launch;
		}

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGui::GetIO().IniFilename = nullptr;
		theme::apply();
		theme::load_fonts();

		ImGui_ImplWin32_Init(win.hwnd);
		ImGui_ImplOpenGL3_Init("#version 330");

		bool post_ok = crt_post::init();
		if (!post_ok)
			log_warn("manager_ui: CRT pass unavailable - rendering flat");

		LARGE_INTEGER freq, start;
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&start);

		Result result = Result::Quit;
		bool running = true;

		while (running)
		{
			gl_ctx::pump(win);
			if (win.quit)
				break;

			if (IsIconic(win.hwnd))
			{
				Sleep(16);
				continue;
			}

			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			float t =
			    (float)(now.QuadPart - start.QuadPart) / (float)freq.QuadPart;

			ImGui_ImplOpenGL3_NewFrame();
			ImGui_ImplWin32_NewFrame();
			ImGui::NewFrame();

			ImGuiViewport *vp = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(vp->WorkPos);
			ImGui::SetNextWindowSize(vp->WorkSize);

			ImGui::Begin(
			    "##card", nullptr,
			    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
			        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
			        ImGuiWindowFlags_NoBringToFrontOnFocus |
			        ImGuiWindowFlags_NoScrollbar);
			{
				ImDrawList *bg = ImGui::GetBackgroundDrawList();

				theme::draw_radial_glow(
				    bg,
				    ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
				           vp->WorkPos.y - 260.0f + 280.0f),
				    450.0f, 280.0f, theme::accent(0.12f));

				ImVec2 wp = ImGui::GetWindowPos();
				ImVec2 ws = ImGui::GetWindowSize();
				theme::draw_top_edge(ImGui::GetWindowDrawList(), wp,
				                     ImVec2(wp.x + ws.x, wp.y + 1.0f));

				draw_header(ImGui::GetWindowDrawList(), t);

				ImGui::Dummy(ImVec2(0, 10));
				ImGui::Separator();
				ImGui::Dummy(ImVec2(0, 10));

				draw_meta(mods);

				ImGui::Dummy(ImVec2(0, 10));
				ImGui::Separator();
				ImGui::Dummy(ImVec2(0, 14));

				theme::section_label("discovered mods");

				float footer_h = ImGui::GetFrameHeightWithSpacing() + 20.0f;
				float detail_h = (g_selected >= 0) ? 256.0f : 0.0f;

				ImGui::BeginChild("##mods",
				                  ImVec2(0, ImGui::GetContentRegionAvail().y -
				                                footer_h - detail_h - 48),
				                  false);
				ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2));
				if (mods.empty())
				{
					ImGui::PushStyleColor(ImGuiCol_Text, theme::col::txt);
					ImGui::TextWrapped(
					    "No mods found. Drop mod folders containing a "
					    "cu3ml.toml into the Mods directory next to the game.");
					ImGui::PopStyleColor();
				}
				for (int i = 0; i < (int)mods.size(); ++i)
					draw_mod_row(i, mods[i]);
				ImGui::PopStyleVar();
				ImGui::EndChild();

				if (g_selected >= 0 && g_selected < (int)mods.size())
				{
					ImGui::Separator();
					ImGui::Dummy(ImVec2(0, 8));
					theme::section_label("detail");
					ImGui::BeginChild("##detail", ImVec2(0, detail_h - 46.0f),
					                  false);
					draw_detail(mods[g_selected]);
					ImGui::EndChild();
				}

				ImGui::Separator();
				ImGui::Dummy(ImVec2(0, 8));

				float bw = 128.0f;
				ImGui::SetCursorPosX(ImGui::GetWindowWidth() - (bw * 2 + 38) -
				                     48);

				if (theme::btn_ghost("QUIT", ImVec2(bw, 0)))
				{
					result = Result::Quit;
					running = false;
				}
				ImGui::SameLine(0, 10);

				if (theme::btn_primary("LAUNCH GAME  ->", ImVec2(bw + 48, 0)))
				{
					result = Result::Launch;
					running = false;
				}
			}
			ImGui::End();

			ImGui::Render();

			if (post_ok)
				crt_post::begin(win.width, win.height);
			else
			{
				glViewport(0, 0, win.width, win.height);
				glClearColor(0.047f, 0.027f, 0.012f, 1.0f);
				glClear(GL_COLOR_BUFFER_BIT);
			}

			ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

			if (post_ok)
				crt_post::end(win.width, win.height, t);

			gl_ctx::present(win);
		}

		crt_post::shutdown();
		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
		gl_ctx::destroy(win);

		log_info("manager_ui: exit (%s)",
		         result == Result::Launch ? "launch" : "quit");
		return result;
	}
}  // namespace ui
