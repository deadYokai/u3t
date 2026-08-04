#include "theme.hpp"
#include "imgui_internal.h"
#include "util.hpp"
#include <cmath>

namespace theme
{
	Fonts fonts{};

	ImU32 accent(float alpha)
	{
		return IM_COL32(255, 106, 0, (int)(alpha * 255.0f + 0.5f));
	}

	void apply()
	{
		ImGuiStyle &s = ImGui::GetStyle();

		s.WindowRounding = 10.0f;
		s.ChildRounding = 8.0f;
		s.FrameRounding = 4.0f;
		s.GrabRounding = 4.0f;
		s.PopupRounding = 8.0f;
		s.ScrollbarRounding = 2.0f;
		s.TabRounding = 4.0f;

		s.WindowBorderSize = 1.0f;
		s.FrameBorderSize = 1.0f;
		s.ChildBorderSize = 1.0f;
		s.PopupBorderSize = 1.0f;

		s.WindowPadding = ImVec2(28, 28);
		s.FramePadding = ImVec2(11, 9);
		s.ItemSpacing = ImVec2(9, 8);
		s.ItemInnerSpacing = ImVec2(7, 6);
		s.ScrollbarSize = 8.0f;
		s.GrabMinSize = 18.0f;

		ImVec4 *c = s.Colors;
		c[ImGuiCol_WindowBg] = ImVec4(0.047f, 0.027f, 0.012f, 0.82f);
		c[ImGuiCol_ChildBg] = ImVec4(0.082f, 0.051f, 0.024f, 1.00f);
		c[ImGuiCol_PopupBg] = ImVec4(0.063f, 0.035f, 0.012f, 0.97f);

		c[ImGuiCol_Border] = ImVec4(1.000f, 0.416f, 0.000f, 0.16f);
		c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_Separator] = ImVec4(1.000f, 0.416f, 0.000f, 0.07f);
		c[ImGuiCol_SeparatorHovered] = ImVec4(1.000f, 0.416f, 0.000f, 0.30f);
		c[ImGuiCol_SeparatorActive] = col::o;

		c[ImGuiCol_Text] = col::hi;
		c[ImGuiCol_TextDisabled] = col::dim;

		c[ImGuiCol_FrameBg] = ImVec4(1.000f, 0.416f, 0.000f, 0.02f);
		c[ImGuiCol_FrameBgHovered] = ImVec4(1.000f, 0.416f, 0.000f, 0.07f);
		c[ImGuiCol_FrameBgActive] = ImVec4(1.000f, 0.416f, 0.000f, 0.10f);

		c[ImGuiCol_Button] = ImVec4(1.000f, 0.416f, 0.000f, 0.02f);
		c[ImGuiCol_ButtonHovered] = ImVec4(1.000f, 0.416f, 0.000f, 0.07f);
		c[ImGuiCol_ButtonActive] = ImVec4(1.000f, 0.565f, 0.251f, 0.20f);

		c[ImGuiCol_Header] = ImVec4(1.000f, 0.416f, 0.000f, 0.05f);
		c[ImGuiCol_HeaderHovered] = ImVec4(1.000f, 0.416f, 0.000f, 0.08f);
		c[ImGuiCol_HeaderActive] = ImVec4(1.000f, 0.416f, 0.000f, 0.12f);

		c[ImGuiCol_CheckMark] = col::o;
		c[ImGuiCol_SliderGrab] = col::o;
		c[ImGuiCol_SliderGrabActive] = col::o2;

		c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_ScrollbarGrab] = ImVec4(1.000f, 0.416f, 0.000f, 0.14f);
		c[ImGuiCol_ScrollbarGrabHovered] =
		    ImVec4(1.000f, 0.416f, 0.000f, 0.28f);
		c[ImGuiCol_ScrollbarGrabActive] = ImVec4(1.000f, 0.416f, 0.000f, 0.40f);

		c[ImGuiCol_TitleBg] = ImVec4(0.047f, 0.027f, 0.012f, 1.00f);
		c[ImGuiCol_TitleBgActive] = ImVec4(0.063f, 0.035f, 0.012f, 1.00f);
	}

	void load_fonts()
	{
		ImGuiIO &io = ImGui::GetIO();

		ImFontConfig def;
		def.SizePixels = 16.0f;

		ImFontConfig display;
		display.SizePixels = 32.0f;

		ImFontConfig small;
		small.SizePixels = 14.0f;

		if (!fonts.sans)
			fonts.sans = io.Fonts->AddFontDefault(&def);
		if (!fonts.mono)
			fonts.mono = io.Fonts->AddFontDefault(&def);
		if (!fonts.display)
			fonts.display = io.Fonts->AddFontDefault(&display);
		if (!fonts.small_)
			fonts.small_ = io.Fonts->AddFontDefault(&small);

		io.FontDefault = fonts.sans;
	}

	float flicker_text_alpha(float time_sec)
	{
		float p = fmodf(time_sec, 4.0f) / 4.0f;
		if ((p >= 0.52f && p < 0.53f) || (p >= 0.92f && p < 0.93f) ||
		    (p >= 0.96f && p < 0.97f))
			return 0.0f;
		return 1.0f;
	}

	void draw_radial_glow(ImDrawList *dl, ImVec2 center, float rx, float ry,
	                      ImU32 inner, int segments)
	{
		if (segments < 3)
			return;
		const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
		const ImU32 outer = inner & ~IM_COL32_A_MASK;

		dl->PrimReserve(segments * 3, segments + 1);
		unsigned int base = dl->_VtxCurrentIdx;

		dl->PrimWriteVtx(center, uv, inner);
		for (int i = 0; i < segments; ++i)
		{
			float a = (float)i / (float)segments * 6.28318530718f;
			dl->PrimWriteVtx(
			    ImVec2(center.x + cosf(a) * rx, center.y + sinf(a) * ry), uv,
			    outer);
		}
		for (int i = 0; i < segments; ++i)
		{
			dl->PrimWriteIdx((ImDrawIdx)base);
			dl->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
			dl->PrimWriteIdx((ImDrawIdx)(base + 1 + ((i + 1) % segments)));
		}
	}

	void draw_top_edge(ImDrawList *dl, ImVec2 a, ImVec2 b, float alpha)
	{
		float mid = (a.x + b.x) * 0.5f;
		ImU32 clear = accent(0.0f);
		ImU32 full = accent(alpha);
		dl->AddRectFilledMultiColor(a, ImVec2(mid, b.y), clear, full, full,
		                            clear);
		dl->AddRectFilledMultiColor(ImVec2(mid, a.y), b, full, clear, clear,
		                            full);
	}

	void section_label(const char *text)
	{
		ImDrawList *dl = ImGui::GetWindowDrawList();
		ImVec2 p = ImGui::GetCursorScreenPos();

		float cy = p.y + ImGui::GetTextLineHeight() * 0.5f;
		dl->AddLine(ImVec2(p.x, cy), ImVec2(p.x + 10.0f, cy), accent(0.4f),
		            1.0f);

		ImGui::Dummy(ImVec2(17.0f, 0));
		ImGui::SameLine(0, 0);

		ImGui::PushFont(theme::fonts.small_);
		ImGui::PushStyleColor(ImGuiCol_Text, col::dim);
		for (const char *c = text; *c; ++c)
		{
			char up[2] = {(char)toupper((unsigned char)*c), 0};
			ImGui::TextUnformatted(up);
			if (c[1])
				ImGui::SameLine(0, 3.0f);
		}
		ImGui::PopStyleColor();
		ImGui::PopFont();
		ImGui::Dummy(ImVec2(0, 4));
	}

	void badge(const char *text)
	{
		ImGui::PushFont(fonts.small_);
		ImGui::PushStyleColor(ImGuiCol_Text, col::o3);
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0.416f, 0, 0.05f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
		                      ImVec4(1.0f, 0.416f, 0, 0.05f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive,
		                      ImVec4(1.0f, 0.416f, 0, 0.05f));
		ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.416f, 0, 0.18f));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 2));
		ImGui::Button(text);
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(5);
		ImGui::PopFont();
	}

	namespace
	{
		ImU32 brightness(ImU32 c, float f)
		{
			ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
			v.x = ImMin(v.x * f, 1.0f);
			v.y = ImMin(v.y * f, 1.0f);
			v.z = ImMin(v.z * f, 1.0f);
			return ImGui::ColorConvertFloat4ToU32(v);
		}

		constexpr float kBtnRounding = 8.0f;
		const ImVec2 kBtnPad(20.0f, 11.0f);

		ImVec2 btn_size(const char *label, const ImVec2 &want)
		{
			const char *end = ImGui::FindRenderedTextEnd(label);
			ImVec2 ts = ImGui::CalcTextSize(label, end);
			return ImVec2(want.x > 0.0f ? want.x : ts.x + kBtnPad.x * 2.0f,
			              want.y > 0.0f ? want.y : ts.y + kBtnPad.y * 2.0f);
		}

		void btn_label(ImDrawList *dl, const ImVec2 &p0, const ImVec2 &p1,
		               const char *label, ImU32 col)
		{
			const char *end = ImGui::FindRenderedTextEnd(label);
			ImVec2 ts = ImGui::CalcTextSize(label, end);
			ImVec2 at((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f);
			dl->AddText(at, col, label, end);
		}
	}  // namespace

	bool btn_primary(const char *label, const ImVec2 &size)
	{
		ImVec2 sz = btn_size(label, size);
		ImVec2 p0 = ImGui::GetCursorScreenPos();

		bool pressed = ImGui::InvisibleButton(label, sz);
		bool hovered = ImGui::IsItemHovered();
		bool held = ImGui::IsItemActive();

		ImVec2 p1 = ImGui::GetItemRectMax();
		ImDrawList *dl = ImGui::GetWindowDrawList();

		ImU32 c0 = ImGui::ColorConvertFloat4ToU32(col::o);
		ImU32 c1 = ImGui::ColorConvertFloat4ToU32(col::o2);

		float f = held ? 0.94f : (hovered ? 1.08f : 1.0f);
		c0 = brightness(c0, f);
		c1 = brightness(c1, f);

		int vtx0 = dl->VtxBuffer.Size;
		dl->AddRectFilled(p0, p1, c0, kBtnRounding);
		int vtx1 = dl->VtxBuffer.Size;
		ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, vtx0, vtx1, p0, p1,
		                                              c0, c1);

		btn_label(dl, p0, p1, label, IM_COL32(0x1a, 0x09, 0x00, 0xff));
		return pressed;
	}

	bool btn_ghost(const char *label, const ImVec2 &size)
	{
		ImVec2 sz = btn_size(label, size);
		ImVec2 p0 = ImGui::GetCursorScreenPos();

		bool pressed = ImGui::InvisibleButton(label, sz);
		bool hovered = ImGui::IsItemHovered();
		bool held = ImGui::IsItemActive();

		ImVec2 p1 = ImGui::GetItemRectMax();
		ImDrawList *dl = ImGui::GetWindowDrawList();

		ImU32 bg = ImGui::ColorConvertFloat4ToU32(col::bg_soft);
		if (held)
			bg = ImGui::ColorConvertFloat4ToU32(
			    ImVec4(1.000f, 0.416f, 0.000f, 0.10f));
		dl->AddRectFilled(p0, p1, bg, kBtnRounding);

		dl->AddRect(p0, p1, hovered ? accent(1.0f) : accent(0.16f),
		            kBtnRounding, 0, 1.0f);

		btn_label(dl, p0, p1, label,
		          ImGui::ColorConvertFloat4ToU32(hovered ? col::o2 : col::hi));
		return pressed;
	}

	bool toggle(const char *id, bool *v, float height)
	{
		float h = height > 0.0f ? height : ImGui::GetFrameHeight() * 0.62f;
		float w = h * 1.9f;
		float r = h * 0.5f;

		ImVec2 p0 = ImGui::GetCursorScreenPos();
		bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
		if (pressed && v)
			*v = !*v;

		bool on = v && *v;
		bool hovered = ImGui::IsItemHovered();
		bool held = ImGui::IsItemActive();

		ImVec2 p1 = ImGui::GetItemRectMax();
		ImDrawList *dl = ImGui::GetWindowDrawList();

		if (on)
			dl->AddRectFilled(
			    p0, p1, accent(held ? 0.60f : (hovered ? 0.85f : 0.72f)), r);
		else
			dl->AddRectFilled(p0, p1,
			                  ImGui::ColorConvertFloat4ToU32(col::bg_soft), r);

		dl->AddRect(p0, p1, on ? accent(1.0f) : accent(hovered ? 0.45f : 0.18f),
		            r, 0, 1.0f);

		float knob = r - 2.5f;
		float cx = on ? (p1.x - r) : (p0.x + r);
		dl->AddCircleFilled(
		    ImVec2(cx, p0.y + r), knob,
		    on ? IM_COL32(0x1a, 0x09, 0x00, 0xff)
		       : ImGui::ColorConvertFloat4ToU32(hovered ? col::txt : col::dim));
		return pressed;
	}
}  // namespace theme
