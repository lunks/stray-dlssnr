// overlay_calls_probe.cpp - compiles and LINKS every ImGui entry point the overlay is allowed to
// use, under both toolchains.
//
// Compiling is not the interesting half. reshade_overlay.hpp only provides inline definitions for
// the functions that exist in imgui_function_table_19250; imgui.h DECLARES hundreds more. So a
// call to something the table does not carry compiles fine and fails at LINK time with an
// undefined reference. That is why this probe is linked into a DLL rather than just compiled.
//
// Nothing here is ever executed. It exists so that CI answers "does the whitelist actually exist
// and is it callable from both compilers" without anyone having to launch the game.

#include "../src/overlay_imgui.hpp"

extern "C" __declspec(dllexport) void overlay_whitelist_probe()
{
	if (!overlay_imgui::available())
		return;

	static bool  b     = false;
	static int   i     = 0;
	static float f     = 0.0f;
	static char  buf[64] = {};
	static float col3[3] = {};
	static float col4[4] = {};

	// --- layout / structure -------------------------------------------------------------------
	ImGui::Separator();
	ImGui::SeparatorText("section");
	ImGui::Spacing();
	ImGui::SameLine(0.0f, -1.0f);
	ImGui::NewLine();
	ImGui::Indent(0.0f);
	ImGui::Unindent(0.0f);
	ImGui::BeginGroup();
	ImGui::EndGroup();
	ImGui::PushID(1);
	ImGui::PopID();
	ImGui::PushItemWidth(120.0f);
	ImGui::PopItemWidth();
	ImGui::SetNextItemWidth(120.0f);
	ImGui::AlignTextToFramePadding();

	// --- text ---------------------------------------------------------------------------------
	ImGui::TextUnformatted("plain");
	ImGui::TextUnformatted("plain", nullptr);
	ImGui::Text("fmt %d", 1);
	ImGui::TextDisabled("dim");
	ImGui::TextWrapped("wrapped");
	ImGui::BulletText("bullet");
	ImGui::LabelText("label", "value");
	ImGui::TextColored(ImVec4(1, 0, 0, 1), "red");

	// --- controls -----------------------------------------------------------------------------
	ImGui::Checkbox("chk", &b);
	ImGui::SliderFloat("sf", &f, 0.0f, 1.0f, "%.3f", 0);
	ImGui::SliderInt("si", &i, 0, 4, "%d", 0);
	ImGui::DragFloat("df", &f, 0.01f, 0.0f, 1.0f, "%.3f", 0);
	ImGui::RadioButton("rb", true);
	ImGui::RadioButton("rb2", &i, 1);
	ImGui::Button("btn");
	ImGui::Button("btn2", ImVec2(80, 0));
	ImGui::SmallButton("sb");
	ImGui::InputText("it", buf, sizeof(buf), 0, nullptr, nullptr);
	ImGui::ColorEdit3("c3", col3, 0);
	ImGui::ColorEdit4("c4", col4, 0);

	static const char *items[] = { "a", "b", "c" };
	ImGui::Combo("cmb", &i, items, 3, -1);
	ImGui::Combo("cmb2", &i, "a\0b\0c\0", -1);
	if (ImGui::BeginCombo("bc", "preview", 0)) {
		ImGui::Selectable("opt", false, 0, ImVec2(0, 0));
		ImGui::EndCombo();
	}

	// --- trees / headers ----------------------------------------------------------------------
	if (ImGui::TreeNode("node")) {
		ImGui::TreePop();
	}
	ImGui::CollapsingHeader("hdr", 0);

	// --- enable/disable + tooltips ------------------------------------------------------------
	ImGui::BeginDisabled(true);
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(0)) {
		ImGui::SetTooltip("tip");
	}
	ImGui::SetItemTooltip("tip2");
	if (ImGui::BeginTooltip()) {
		ImGui::TextUnformatted("inside");
		ImGui::EndTooltip();
	}

	// --- colour ---------------------------------------------------------------------------------
	// GetStyleColorVec4 returns a REFERENCE (a pointer), so it is convention-independent.
	const ImVec4 &sc = ImGui::GetStyleColorVec4(ImGuiCol_Text);
	ImGui::PushStyleColor(ImGuiCol_Text, sc);
	ImGui::PopStyleColor(1);
	ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
	ImGui::PopStyleColor(1);
	// ImVec4 is 16 bytes, so the MS x64 "not 1/2/4/8 bytes -> memory" rule applies with no
	// POD-ness clause and both toolchains agree. Measured in the abi job.
	ImVec4 conv = ImGui::ColorConvertU32ToFloat4(0xFFFFFFFFu);
	(void)conv;

	// --- floats read back from ImGui without returning an aggregate ---------------------------
	const float line_h = ImGui::GetTextLineHeight();
	const float frame_h = ImGui::GetFrameHeight();
	(void)line_h; (void)frame_h;
}

// Deliberately NOT called anywhere, and enforced by a CI grep over src/:
//   GetWindowPos GetWindowSize GetFontTexUvWhitePixel GetCursorScreenPos GetContentRegionAvail
//   GetCursorPos GetCursorStartPos GetItemRectMin GetItemRectMax GetItemRectSize CalcTextSize
//   GetMousePos GetMousePosOnOpeningCurrentPopup GetMouseDragDelta
// Each returns ImVec2 BY VALUE. MSVC returns that in memory (ImVec2 has user-declared
// constructors, so it is not POD to MSVC); GCC returns it in RAX (it is trivially copyable and 8
// bytes). Calling one from a mingw build is an 8-byte store through an uninitialised RCX.

extern "C" __declspec(dllexport) bool overlay_bind_probe(HMODULE m)
{
	overlay_imgui::textf("%d", 1);
	overlay_imgui::textf_colored(ImVec4(1, 1, 0, 1), "%s", "warn");
	return overlay_imgui::bind_table(m);
}

// The escape hatch, type-checked under both toolchains. Never executed. Its RUNTIME correctness
// is established separately, against real MSVC by-value exports, by the thunk_ret_vec2 /
// thunk_ret_vec2_args / thunk_stack_canary cases in abi/imvec_caller.cpp.
extern "C" __declspec(dllexport) void overlay_thunk_probe()
{
	const imgui_function_table *const t = overlay_imgui::table();
	if (t == nullptr)
		return;
	const ImVec2 p = overlay_imgui::by_value_ret(t->GetCursorScreenPos);
	(void)p;
}

// ---------------------------------------------------------------------------------------------
// The whitelist AGAIN, written the way the UI will actually be written: leaning on default
// arguments.
//
// This is not redundant. reshade_overlay.hpp's inline wrappers declare NO default arguments -
// `inline bool Button(const char* label, const ImVec2& size)` - so it looks as though every call
// must pass every parameter. It does not: those inlines REDECLARE functions that <imgui.h> has
// already declared in the same namespace WITH defaults (`Button(const char*, const ImVec2& size =
// ImVec2(0,0))`), and a redeclaration inherits the earlier default arguments. Compiling this
// under both toolchains is what keeps that true, so the UI can be written in ordinary ImGui style
// instead of padding every call with explicit defaults.
// ---------------------------------------------------------------------------------------------
extern "C" __declspec(dllexport) void overlay_default_args_probe()
{
	if (!overlay_imgui::available())
		return;

	static bool  b = false;
	static int   i = 0;
	static float f = 0.0f;
	static const char *const items[] = { "a", "b" };

	ImGui::SeparatorText("DLSS 5 Neural Rendering");
	ImGui::Checkbox("Enable DLSS Neural Rendering", &b);
	ImGui::SliderFloat("NR Intensity", &f, 0.0f, 1.0f);
	ImGui::SliderInt("NR Preset", &i, 0, 4);
	ImGui::Combo("NR Style", &i, items, 2);
	ImGui::RadioButton("Force inverted depth", true);
	ImGui::Button("Reset");
	ImGui::SameLine();
	ImGui::SmallButton("Defaults");
	ImGui::TextUnformatted("WAITING FOR NGX");
	ImGui::Text("%d", 1);
	ImGui::TextColored(ImVec4(1, 1, 0, 1), "%s", "WAITING FOR GAME DLSS");
	ImGui::TextDisabled("%s", "n/a");
	ImGui::TextWrapped("%s", "wrapped");
	ImGui::SetTooltip("%s", "tip");
	if (ImGui::IsItemHovered()) { }
	if (ImGui::TreeNodeEx("Advanced", ImGuiTreeNodeFlags_DefaultOpen)) { ImGui::TreePop(); }
	if (ImGui::CollapsingHeader("Diagnostics")) { }
	ImGui::Selectable("row", false);
	ImGui::ProgressBar(0.5f);
	ImGui::Indent();
	ImGui::Unindent();
	ImGui::BeginDisabled(true);
	ImGui::EndDisabled();
	ImGui::Separator();
	ImGui::Spacing();
	ImGui::Bullet();
}
