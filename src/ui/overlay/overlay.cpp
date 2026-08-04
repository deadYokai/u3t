#define SOL_ALL_SAFETIES_ON 1
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "overlay.hpp"

#include <sol/sol.hpp>

#include "imgui.h"
#include "logs.hpp"
#include "ui/theme.hpp"

#include <atomic>
#include <string>
#include <vector>

namespace
{
	std::vector<sol::protected_function> g_draw_cbs;
	std::atomic<bool> g_visible{false};
	std::atomic<bool> g_ready{false};
}  // namespace

namespace overlay
{
	void clear_state()
	{
		g_draw_cbs.clear();
		g_visible.store(false);
		g_ready.store(false);
	}

	bool visible() { return g_visible.load(std::memory_order_relaxed); }

	void set_visible(bool v) { g_visible.store(v, std::memory_order_relaxed); }

	void toggle() { g_visible.store(!g_visible.load()); }

	bool backend_ready() { return g_ready.load(std::memory_order_relaxed); }

	void on_backend_ready()
	{
		g_ready.store(true, std::memory_order_relaxed);
		log_info("overlay: backend ready");
	}

	void render()
	{
		if (!g_visible.load(std::memory_order_relaxed))
			return;

		for (auto &fn : g_draw_cbs)
		{
			auto r = fn();
			if (!r.valid())
			{
				sol::error e = r;
				log_err("overlay: draw callback: %s", e.what());
			}
		}
	}

	void bind_lua(sol::state &lua)
	{
		sol::table tbl = lua["overlay"].get_or_create<sol::table>();

		tbl.set_function("visible", []() { return visible(); });
		tbl.set_function("set_visible", [](bool v) { set_visible(v); });
		tbl.set_function("toggle", []() { toggle(); });
		tbl.set_function("ready", []() { return backend_ready(); });

		tbl.set_function("on_draw",
		                 [](sol::protected_function fn) -> bool
		                 {
			                 if (!fn.valid())
				                 return false;
			                 g_draw_cbs.push_back(std::move(fn));
			                 return true;
		                 });

		tbl.set_function(
		    "begin_window",
		    [](const std::string &name, sol::optional<int> flags) -> bool
		    { return ImGui::Begin(name.c_str(), nullptr, flags.value_or(0)); });

		tbl.set_function("begin_window_closeable",
		                 [](const std::string &name, bool open,
		                    sol::optional<int> flags) -> std::tuple<bool, bool>
		                 {
			                 bool o = open;
			                 bool v = ImGui::Begin(name.c_str(), &o,
			                                       flags.value_or(0));
			                 return {v, o};
		                 });

		tbl.set_function("end_window", []() { ImGui::End(); });

		tbl.set_function("text", [](const std::string &s)
		                 { ImGui::TextUnformatted(s.c_str()); });

		tbl.set_function(
		    "text_colored",
		    [](float r, float g, float b, float a, const std::string &s)
		    { ImGui::TextColored(ImVec4(r, g, b, a), "%s", s.c_str()); });

		tbl.set_function("text_wrapped", [](const std::string &s)
		                 { ImGui::TextWrapped("%s", s.c_str()); });

		tbl.set_function(
		    "label_text", [](const std::string &label, const std::string &text)
		    { ImGui::LabelText(label.c_str(), "%s", text.c_str()); });

		tbl.set_function("bullet_text", [](const std::string &s)
		                 { ImGui::BulletText("%s", s.c_str()); });

		tbl.set_function("button",
		                 [](const std::string &label, sol::optional<float> w,
		                    sol::optional<float> h) -> bool
		                 {
			                 return ImGui::Button(
			                     label.c_str(),
			                     ImVec2(w.value_or(0), h.value_or(0)));
		                 });

		tbl.set_function("small_button", [](const std::string &label) -> bool
		                 { return ImGui::SmallButton(label.c_str()); });

		tbl.set_function(
		    "checkbox",
		    [](const std::string &label, bool v) -> std::tuple<bool, bool>
		    {
			    bool changed = ImGui::Checkbox(label.c_str(), &v);
			    return {changed, v};
		    });

		tbl.set_function("slider_float",
		                 [](const std::string &label, float v, float mn,
		                    float mx) -> std::tuple<bool, float>
		                 {
			                 bool changed =
			                     ImGui::SliderFloat(label.c_str(), &v, mn, mx);
			                 return {changed, v};
		                 });

		tbl.set_function("slider_int",
		                 [](const std::string &label, int v, int mn,
		                    int mx) -> std::tuple<bool, int>
		                 {
			                 bool changed =
			                     ImGui::SliderInt(label.c_str(), &v, mn, mx);
			                 return {changed, v};
		                 });

		tbl.set_function(
		    "input_text",
		    [](const std::string &label, const std::string &val,
		       sol::optional<int> max_len) -> std::tuple<bool, std::string>
		    {
			    int cap = max_len.value_or(256);
			    std::vector<char> buf(cap + 1, 0);
			    size_t copy = val.size() < (size_t)cap ? val.size() : cap;
			    memcpy(buf.data(), val.c_str(), copy);
			    bool changed = ImGui::InputText(label.c_str(), buf.data(), cap);
			    return {changed, std::string(buf.data())};
		    });

		tbl.set_function(
		    "input_float",
		    [](const std::string &label, float v) -> std::tuple<bool, float>
		    {
			    bool changed = ImGui::InputFloat(label.c_str(), &v);
			    return {changed, v};
		    });

		tbl.set_function(
		    "input_int",
		    [](const std::string &label, int v) -> std::tuple<bool, int>
		    {
			    bool changed = ImGui::InputInt(label.c_str(), &v);
			    return {changed, v};
		    });

		tbl.set_function("color_edit3",
		                 [](const std::string &label, float r, float g,
		                    float b) -> std::tuple<bool, float, float, float>
		                 {
			                 float c[3] = {r, g, b};
			                 bool changed = ImGui::ColorEdit3(label.c_str(), c);
			                 return {changed, c[0], c[1], c[2]};
		                 });

		tbl.set_function(
		    "color_edit4",
		    [](const std::string &label, float r, float g, float b,
		       float a) -> std::tuple<bool, float, float, float, float>
		    {
			    float c[4] = {r, g, b, a};
			    bool changed = ImGui::ColorEdit4(label.c_str(), c);
			    return {changed, c[0], c[1], c[2], c[3]};
		    });

		tbl.set_function("combo",
		                 [](const std::string &label, int current,
		                    sol::table items) -> std::tuple<bool, int>
		                 {
			                 std::vector<std::string> strs;
			                 for (auto &kv : items)
				                 strs.push_back(kv.second.as<std::string>());

			                 std::vector<const char *> ptrs;
			                 for (auto &s : strs)
				                 ptrs.push_back(s.c_str());

			                 bool changed =
			                     ImGui::Combo(label.c_str(), &current,
			                                  ptrs.data(), (int)ptrs.size());
			                 return {changed, current};
		                 });

		tbl.set_function("separator", []() { ImGui::Separator(); });

		tbl.set_function(
		    "same_line",
		    [](sol::optional<float> offset, sol::optional<float> spacing)
		    { ImGui::SameLine(offset.value_or(0), spacing.value_or(-1.0f)); });

		tbl.set_function("spacing", []() { ImGui::Spacing(); });
		tbl.set_function("dummy",
		                 [](float w, float h) { ImGui::Dummy(ImVec2(w, h)); });

		tbl.set_function("indent", [](sol::optional<float> w)
		                 { ImGui::Indent(w.value_or(0)); });
		tbl.set_function("unindent", [](sol::optional<float> w)
		                 { ImGui::Unindent(w.value_or(0)); });

		tbl.set_function("begin_group", []() { ImGui::BeginGroup(); });
		tbl.set_function("end_group", []() { ImGui::EndGroup(); });

		tbl.set_function(
		    "set_next_window_pos", [](float x, float y, sol::optional<int> cond)
		    { ImGui::SetNextWindowPos(ImVec2(x, y), cond.value_or(0)); });

		tbl.set_function(
		    "set_next_window_size",
		    [](float w, float h, sol::optional<int> cond)
		    { ImGui::SetNextWindowSize(ImVec2(w, h), cond.value_or(0)); });

		tbl.set_function("tree_node", [](const std::string &label) -> bool
		                 { return ImGui::TreeNode(label.c_str()); });

		tbl.set_function("tree_pop", []() { ImGui::TreePop(); });

		tbl.set_function("collapsing_header",
		                 [](const std::string &label) -> bool
		                 { return ImGui::CollapsingHeader(label.c_str()); });

		tbl.set_function("begin_tab_bar", [](const std::string &id) -> bool
		                 { return ImGui::BeginTabBar(id.c_str()); });

		tbl.set_function("end_tab_bar", []() { ImGui::EndTabBar(); });

		tbl.set_function("begin_tab_item", [](const std::string &label) -> bool
		                 { return ImGui::BeginTabItem(label.c_str()); });

		tbl.set_function("end_tab_item", []() { ImGui::EndTabItem(); });

		tbl.set_function("begin_child",
		                 [](const std::string &id, float w, float h,
		                    sol::optional<bool> border) -> bool
		                 {
			                 return ImGui::BeginChild(id.c_str(), ImVec2(w, h),
			                                          border.value_or(false));
		                 });

		tbl.set_function("end_child", []() { ImGui::EndChild(); });

		tbl.set_function("is_item_hovered",
		                 []() -> bool { return ImGui::IsItemHovered(); });

		tbl.set_function("set_tooltip", [](const std::string &s)
		                 { ImGui::SetTooltip("%s", s.c_str()); });

		tbl.set_function("get_framerate",
		                 []() -> float { return ImGui::GetIO().Framerate; });

		tbl.set_function("get_display_size",
		                 []() -> std::tuple<float, float>
		                 {
			                 ImVec2 s = ImGui::GetIO().DisplaySize;
			                 return {s.x, s.y};
		                 });

		tbl.set_function("section_label", [](const std::string &s)
		                 { theme::section_label(s.c_str()); });

		tbl.set_function("badge",
		                 [](const std::string &s) { theme::badge(s.c_str()); });

		tbl.set_function("btn_primary",
		                 [](const std::string &label, sol::optional<float> w,
		                    sol::optional<float> h) -> bool
		                 {
			                 return theme::btn_primary(
			                     label.c_str(),
			                     ImVec2(w.value_or(0), h.value_or(0)));
		                 });

		tbl.set_function("btn_ghost",
		                 [](const std::string &label, sol::optional<float> w,
		                    sol::optional<float> h) -> bool
		                 {
			                 return theme::btn_ghost(
			                     label.c_str(),
			                     ImVec2(w.value_or(0), h.value_or(0)));
		                 });

		tbl.set_function(
		    "toggle_switch",
		    [](const std::string &id, bool v,
		       sol::optional<float> height) -> std::tuple<bool, bool>
		    {
			    bool pressed =
			        theme::toggle(id.c_str(), &v, height.value_or(0));
			    return {pressed, v};
		    });

		tbl["WindowFlags_None"] = 0;
		tbl["WindowFlags_NoTitleBar"] = ImGuiWindowFlags_NoTitleBar;
		tbl["WindowFlags_NoResize"] = ImGuiWindowFlags_NoResize;
		tbl["WindowFlags_NoMove"] = ImGuiWindowFlags_NoMove;
		tbl["WindowFlags_NoScrollbar"] = ImGuiWindowFlags_NoScrollbar;
		tbl["WindowFlags_NoCollapse"] = ImGuiWindowFlags_NoCollapse;
		tbl["WindowFlags_AlwaysAutoResize"] = ImGuiWindowFlags_AlwaysAutoResize;
		tbl["WindowFlags_NoBackground"] = ImGuiWindowFlags_NoBackground;
		tbl["WindowFlags_NoNav"] = ImGuiWindowFlags_NoNav;

		tbl["Cond_Always"] = ImGuiCond_Always;
		tbl["Cond_Once"] = ImGuiCond_Once;
		tbl["Cond_FirstUseEver"] = ImGuiCond_FirstUseEver;

		log_info("overlay: lua bindings registered");
	}

}  // namespace overlay
