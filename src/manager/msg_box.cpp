#include "msg_box.hpp"
#include "theme.hpp"

#include "logs.hpp"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"

#include "manager/gl_ctx.hpp"

#include "util.hpp"

#include <unordered_map>

#include <windows.h>

namespace msg_box
{
	namespace
	{
		struct Entry
		{
			int handle;
			Config cfg;
			bool opened = false;
		};

		std::vector<Entry> g_queue;
		std::unordered_map<int, Result> g_results;
		int g_next_handle = 1;

		const char *popup_id() { return "##msg_box_modal"; }

		ImU32 icon_color(Icon icon)
		{
			switch (icon)
			{
				case Icon::Warning:
					return ImGui::ColorConvertFloat4ToU32(theme::col::o2);
				case Icon::Error:
					return IM_COL32(0xff, 0x4a, 0x2e, 0xff);
				case Icon::Question:
					return ImGui::ColorConvertFloat4ToU32(theme::col::o3);
				case Icon::Info:
					return theme::accent(1.0f);
				default:
					return ImGui::ColorConvertFloat4ToU32(theme::col::dim);
			}
		}

		const char *icon_glyph(Icon icon)
		{
			switch (icon)
			{
				case Icon::Warning:
					return "!";
				case Icon::Error:
					return "X";
				case Icon::Question:
					return "?";
				case Icon::Info:
					return "i";
				default:
					return nullptr;
			}
		}

		void draw_icon(Icon icon)
		{
			const char *glyph = icon_glyph(icon);
			if (!glyph)
				return;

			const float d = 40.0f;
			ImVec2 p0 = ImGui::GetCursorScreenPos();
			ImVec2 center(p0.x + d * 0.5f, p0.y + d * 0.5f);
			ImDrawList *dl = ImGui::GetWindowDrawList();
			ImU32 col = icon_color(icon);

			dl->AddCircle(center, d * 0.5f, col, 32, 1.5f);

			ImGui::PushFont(theme::fonts.display);
			ImVec2 ts = ImGui::CalcTextSize(glyph);
			dl->AddText(ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f),
			            col, glyph);
			ImGui::PopFont();

			ImGui::Dummy(ImVec2(d, d));
		}

		void resolve(Entry &e, int button_index, bool *close_requested)
		{
			Result r;
			r.closed = true;
			r.button_index = button_index;
			r.button_label =
			    (button_index >= 0 && button_index < (int)e.cfg.buttons.size())
			        ? e.cfg.buttons[(size_t)button_index]
			        : std::string();
			g_results[e.handle] = r;
			*close_requested = true;
		}
	}  // namespace

	int show(Config cfg)
	{
		int handle = g_next_handle++;
		Entry e;
		e.handle = handle;
		e.cfg = std::move(cfg);
		g_queue.push_back(std::move(e));

		gl_ctx::Window win;
		if (!gl_ctx::create(win, to_wide(e.cfg.title).c_str(), e.cfg.width, 200))
		{
			log_err("msg_box: failed to create window");
			return -1;
		}
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGui::GetIO().IniFilename = nullptr;

		theme::apply();

		ImGui_ImplWin32_Init(win.hwnd);
		ImGui_ImplOpenGL3_Init("#version 330");

		int result = -1;
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

			ImGui_ImplWin32_NewFrame();
			ImGui_ImplOpenGL3_NewFrame();
			ImGui::NewFrame();

			msg_box::draw();

			ImGui::Render();

			glViewport(0, 0, win.width, win.height);
			glClearColor(0.082f, 0.051f, 0.024f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT);

			ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

			gl_ctx::present(win);

			Sleep(10);

			if (g_queue.empty())
				break;
		}

		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
		gl_ctx::destroy(win);

		return handle;
	}

	int show_info(const std::string &title, const std::string &message)
	{
		Config c;
		c.title = title;
		c.message = message;
		c.icon = Icon::Info;
		c.buttons = {"OK"};
		return show(std::move(c));
	}

	int show_warning(const std::string &title, const std::string &message)
	{
		Config c;
		c.title = title;
		c.message = message;
		c.icon = Icon::Warning;
		c.buttons = {"OK"};
		return show(std::move(c));
	}

	int show_error(const std::string &title, const std::string &message)
	{
		Config c;
		c.title = title;
		c.message = message;
		c.icon = Icon::Error;
		c.buttons = {"OK"};
		return show(std::move(c));
	}

	int show_confirm(const std::string &title, const std::string &message,
	                 const std::string &confirm_label,
	                 const std::string &cancel_label)
	{
		Config c;
		c.title = title;
		c.message = message;
		c.icon = Icon::Question;
		c.buttons = {cancel_label, confirm_label};
		c.default_button = 1;
		c.escape_button = 0;
		return show(std::move(c));
	}

	void draw()
	{
		if (g_queue.empty())
			return;

		Entry &e = g_queue.front();

		if (!e.opened)
		{
			ImGui::OpenPopup(popup_id());
			e.opened = true;
		}

		ImGuiIO &io = ImGui::GetIO();
		ImGui::SetNextWindowPos(
		    ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
		    ImGuiCond_Always, ImVec2(0.5f, 0.5f));

		float width = e.cfg.width > 0.0f ? e.cfg.width : 380.0f;
		ImGui::SetNextWindowSize(ImVec2(width, 0), ImGuiCond_Always);

		ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg,
		                      ImVec4(0.02f, 0.01f, 0.00f, 0.65f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 22));
		ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);

		ImGuiWindowFlags flags =
		    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
		    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar;

		bool open = true;
		bool close_requested = false;

		if (ImGui::BeginPopupModal(popup_id(), &open, flags))
		{
			ImDrawList *dl = ImGui::GetWindowDrawList();
			ImVec2 wp = ImGui::GetWindowPos();
			ImVec2 ws = ImGui::GetWindowSize();
			theme::draw_top_edge(dl, wp, ImVec2(wp.x + ws.x, wp.y + 1.0f));

			bool has_icon = icon_glyph(e.cfg.icon) != nullptr;
			if (has_icon)
			{
				draw_icon(e.cfg.icon);
				ImGui::SameLine(0, 14);
				ImGui::BeginGroup();
			}

			if (!e.cfg.title.empty())
			{
				ImGui::PushFont(theme::fonts.mono);
				ImGui::PushStyleColor(ImGuiCol_Text, theme::col::hi);
				ImGui::TextUnformatted(e.cfg.title.c_str());
				ImGui::PopStyleColor();
				ImGui::PopFont();
				ImGui::Dummy(ImVec2(0, 6));
			}

			ImGui::PushStyleColor(ImGuiCol_Text, theme::col::txt);
			ImGui::PushTextWrapPos(has_icon ? (ImGui::GetContentRegionMax().x)
			                                : ImGui::GetContentRegionMax().x);
			ImGui::TextWrapped("%s", e.cfg.message.c_str());
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();

			if (has_icon)
				ImGui::EndGroup();

			ImGui::Dummy(ImVec2(0, 18));
			ImGui::Separator();
			ImGui::Dummy(ImVec2(0, 14));

			int n = (int)e.cfg.buttons.size();
			if (n == 0)
			{
				e.cfg.buttons.push_back("OK");
				n = 1;
			}

			const float bw = 108.0f;
			const float gap = 10.0f;
			float total_w = bw * n + gap * (n - 1);
			ImGui::SetCursorPosX(ImGui::GetWindowWidth() - total_w -
			                     ImGui::GetStyle().WindowPadding.x);

			for (int i = 0; i < n; ++i)
			{
				ImGui::PushID(i);
				bool is_default = (i == e.cfg.default_button);
				bool pressed =
				    is_default
				        ? theme::btn_primary(e.cfg.buttons[(size_t)i].c_str(),
				                             ImVec2(bw, 0))
				        : theme::btn_ghost(e.cfg.buttons[(size_t)i].c_str(),
				                           ImVec2(bw, 0));
				ImGui::PopID();

				if (pressed)
					resolve(e, i, &close_requested);

				if (i + 1 < n)
					ImGui::SameLine(0, gap);
			}

			if (!close_requested && e.cfg.default_button >= 0 &&
			    e.cfg.default_button < n &&
			    ImGui::IsKeyPressed(ImGuiKey_Enter, false))
				resolve(e, e.cfg.default_button, &close_requested);

			if (!close_requested && e.cfg.escape_button >= 0 &&
			    e.cfg.escape_button < n &&
			    ImGui::IsKeyPressed(ImGuiKey_Escape, false))
				resolve(e, e.cfg.escape_button, &close_requested);

			if (close_requested)
				ImGui::CloseCurrentPopup();

			ImGui::EndPopup();
		}

		if (!open && !close_requested)
		{
			if (e.cfg.escape_button >= 0)
				resolve(e, e.cfg.escape_button, &close_requested);
			else
				close_requested = false;
		}

		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor();

		if (close_requested)
			g_queue.erase(g_queue.begin());
	}

	Result poll(int handle)
	{
		auto it = g_results.find(handle);
		if (it == g_results.end())
			return Result{};

		Result r = it->second;
		g_results.erase(it);
		return r;
	}

	bool active() { return !g_queue.empty(); }
}  // namespace msg_box
