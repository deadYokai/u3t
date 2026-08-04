#pragma once
#include "imgui.h"

namespace theme
{
	namespace col
	{
		constexpr ImVec4 o{1.000f, 0.416f, 0.000f, 1.0f};
		constexpr ImVec4 o2{1.000f, 0.565f, 0.251f, 1.0f};
		constexpr ImVec4 o3{1.000f, 0.722f, 0.439f, 1.0f};
		constexpr ImVec4 hi{0.961f, 0.816f, 0.627f, 1.0f};
		constexpr ImVec4 txt{0.769f, 0.471f, 0.290f, 1.0f};
		constexpr ImVec4 dim{0.478f, 0.357f, 0.165f, 1.0f};
		constexpr ImVec4 bg_soft{0.082f, 0.051f, 0.024f, 1.0f};
	}  // namespace col

	ImU32 accent(float alpha);

	struct Fonts
	{
		ImFont *sans = nullptr;
		ImFont *mono = nullptr;
		ImFont *display = nullptr;
		ImFont *small_ = nullptr;
	};

	extern Fonts fonts;

	void apply();
	void load_fonts();

	float flicker_text_alpha(float time_sec);

	void draw_radial_glow(ImDrawList *dl, ImVec2 center, float rx, float ry,
	                      ImU32 inner, int segments = 48);

	void draw_top_edge(ImDrawList *dl, ImVec2 a, ImVec2 b, float alpha = 0.7f);

	void section_label(const char *text);

	void badge(const char *text);

	bool btn_primary(const char *label, const ImVec2 &size = ImVec2(0, 0));

	bool btn_ghost(const char *label, const ImVec2 &size = ImVec2(0, 0));

	bool toggle(const char *id, bool *v, float height = 0.0f);
}  // namespace theme
