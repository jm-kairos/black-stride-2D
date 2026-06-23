// bs_ui.cpp — implementation of the bs_ui.h immediate-mode widget facade.
//
// This TU includes <imgui.h> (exposed to engine TUs via -isystem in engine/build.bat) but never
// SDL or the GPU backend. It drives only ImGui's backend-agnostic core, which runs on the single
// global context created by bs_imgui_initialize() inside the GPU backend TU. Both TUs link into
// engine.dll and therefore share that one ImGui context — so building widgets here lands on the
// same draw list the backend records in end_frame. This keeps the "only the backend touches
// SDL/GPU" seam intact while exposing a clean, engine-native widget API to gameplay code.

#include "renderer/bs_ui.h"
#include "renderer/bs_imgui.h"

#include <imgui.h>

b8 bs_ui_begin_panel(const char* title, BsUiAnchor anchor, f32 margin, BsUiType ui_type)
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();

    ImGuiWindowFlags ui_panel_flags = ImGuiWindowFlags_None;

    switch (ui_type)
    {
    case BS_UI_TYPE_GAME:{
        ui_panel_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNav;

        // work_pos/work_size exclude OS decorations; for our borderless window they equal the full
        // framebuffer. Anchor by pinning a window-pivot corner to the matching screen corner.
        ImVec2 base = vp->WorkPos;
        ImVec2 size = vp->WorkSize;
        ImVec2 pos, pivot;
        switch (anchor)
        {
        case BS_UI_ANCHOR_TOP_RIGHT:
            pos   = ImVec2(base.x + size.x - margin, base.y + margin);
            pivot = ImVec2(1.0f, 0.0f); // top-right corner of the window sits at pos
            break;
        case BS_UI_ANCHOR_BOTTOM_LEFT:
            pos   = ImVec2(base.x + margin, base.y + size.y - margin);
            pivot = ImVec2(0.0f, 1.0f); // bottom-left corner of the window sits at pos
            break;
        case BS_UI_ANCHOR_BOTTOM_RIGHT:
            pos   = ImVec2(base.x + size.x - margin, base.y + size.y - margin);
            pivot = ImVec2(1.0f, 1.0f); // bottom-right corner of the window sits at pos
            break;
        case BS_UI_ANCHOR_TOP_CENTER:
            pos   = ImVec2(base.x + size.x * 0.5f, base.y + margin);
            pivot = ImVec2(0.5f, 0.0f); // top edge center sits at pos
            break;
        case BS_UI_ANCHOR_CENTER:
            pos   = ImVec2(base.x + size.x * 0.5f, base.y + size.y * 0.5f);
            pivot = ImVec2(0.5f, 0.5f); // window center sits at pos
            break;
        case BS_UI_ANCHOR_TOP_LEFT:
        default:
            pos   = ImVec2(base.x + margin, base.y + margin);
            pivot = ImVec2(0.0f, 0.0f);
            break;
        }
        ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
        break;
    }
    default:
        break;
    }
    // The title doubles as ImGui's window id; gameplay panels are singletons so this is fine.
    return ImGui::Begin(title, NULL, ui_panel_flags) ? TRUE : FALSE;
}

void bs_ui_end_panel(void)
{
    ImGui::End();
}

void bs_ui_text(const char* text)
{
    ImGui::TextUnformatted(text ? text : "");
}

void bs_ui_text_colored(f32 r, f32 g, f32 b, f32 a, const char* text)
{
    ImGui::TextColored(ImVec4(r, g, b, a), "%s", text ? text : "");
}

void bs_ui_progress(f32 fraction, const char* overlay)
{
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;
    // ImVec2(-FLT_MIN, 0) => full available content width, default height. NULL overlay lets ImGui
    // draw its own "NN%"; pass an explicit overlay (e.g. "Performing 50%") to override or blank it.
    ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), overlay);
}

b8 bs_ui_button(const char* label, b8 enabled)
{
    b8 clicked = FALSE;
    if (!enabled) ImGui::BeginDisabled(true);
    const char* lbl = label ? label : "";
    // A width of -1 stretches the button to the content region but does NOT enlarge an
    // AlwaysAutoResize window — so a button whose label is wider than the panel's other content
    // clips its own text (e.g. "Cancel Current Job" -> "Cancel Current Jo"). Request
    // max(available, natural label width) instead: the widest button then grows the auto-sized
    // panel by one frame and afterwards fills it exactly. `natural` mirrors ImGui's own button-size
    // formula (CalcTextSize with ## hidden + horizontal frame padding on both sides), so the value
    // is stable and the layout converges in a single frame with no oscillation.
    const f32 avail   = ImGui::GetContentRegionAvail().x;
    const f32 natural = ImGui::CalcTextSize(lbl, NULL, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const f32 w       = (natural > avail) ? natural : avail;
    if (ImGui::Button(lbl, ImVec2(w, 0.0f))) clicked = TRUE;
    if (!enabled) ImGui::EndDisabled();
    return clicked;
}

b8 bs_ui_button_sized(const char* label, f32 width, b8 enabled)
{
    b8 clicked = FALSE;
    if (!enabled) ImGui::BeginDisabled(true);
    if (ImGui::Button(label ? label : "", ImVec2(width, 0.0f))) clicked = TRUE;
    if (!enabled) ImGui::EndDisabled();
    return clicked;
}

void bs_ui_checkbox(const char* label, bool* enabled){
    ImGui::Checkbox(label, enabled);
}

b8 bs_ui_slider_float(const char* label, f32* value, f32 min, f32 max)
{
    if (!value) return FALSE;
    return ImGui::SliderFloat(label ? label : "", value, min, max) ? TRUE : FALSE;
}

b8 bs_ui_color_edit3(const char* label, f32* rgb)
{
    if (!rgb) return FALSE;
    return ImGui::ColorEdit3(label ? label : "", rgb) ? TRUE : FALSE;
}

void bs_ui_same_line(void)
{
    ImGui::SameLine();
}

void bs_ui_set_cursor_pos_x(f32 x)
{
    ImGui::SetCursorPosX(x);
}

b8 bs_ui_combo(const char* label, i32* current, const char* items)
{
    if (!current || !items) return FALSE;
    return ImGui::Combo(label ? label : "", current, items) ? TRUE : FALSE;
}

void bs_ui_separator(void)
{
    ImGui::Separator();
}

b8 bs_ui_selectable(const char* label, b8 selected)
{
    return ImGui::Selectable(label ? label : "", selected ? true : false) ? TRUE : FALSE;
}

b8 bs_ui_is_window_hovered(void)
{
    return ImGui::IsWindowHovered() ? TRUE : FALSE;
}

void bs_ui_push_alpha(f32 alpha)
{
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
}

void bs_ui_pop_alpha(void)
{
    ImGui::PopStyleVar();
}

void bs_ui_color_button(f32 r, f32 g, f32 b, f32 a, f32 size)
{
    ImGui::ColorButton("##color", ImVec4(r, g, b, a),
                       ImGuiColorEditFlags_NoPicker
                     | ImGuiColorEditFlags_NoTooltip
                     | ImGuiColorEditFlags_NoDragDrop,
                       ImVec2(size, size));
}

void bs_ui_tooltip_at(f32 screen_x, f32 screen_y, const char* text)
{
    if (!text || !text[0]) return;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
                           | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_AlwaysAutoResize
                           | ImGuiWindowFlags_NoSavedSettings
                           | ImGuiWindowFlags_NoNav
                           | ImGuiWindowFlags_NoFocusOnAppearing
                           | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::SetNextWindowPos(ImVec2(screen_x + 16.0f, screen_y + 16.0f), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.05f, 0.08f, 0.85f));
    ImGui::Begin("##tooltip", NULL, flags);
    ImGui::TextUnformatted(text);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void bs_ui_label_at(const char* id, f32 screen_x, f32 screen_y, f32 font_scale, bs_color tint, const char* text)
{
    if (!text || !text[0]) return;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
                           | ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_AlwaysAutoResize
                           | ImGuiWindowFlags_NoSavedSettings
                           | ImGuiWindowFlags_NoNav
                           | ImGuiWindowFlags_NoFocusOnAppearing
                           | ImGuiWindowFlags_NoBringToFrontOnFocus
                           | ImGuiWindowFlags_NoBackground;
    ImGui::SetNextWindowPos(ImVec2(screen_x, screen_y), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin(id ? id : "##label", NULL, flags);
    ImGui::SetWindowFontScale(font_scale);
    ImGui::TextColored(ImVec4(tint.r, tint.g, tint.b, tint.a), "%s", text);
    ImGui::End();
    ImGui::PopStyleVar();
}

b8 bs_ui_begin_hud_panel(const char* title, BsUiAnchor anchor, f32 margin)
{
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.04f, 0.06f, 0.75f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 3.0f);
    b8 result = bs_ui_begin_panel(title, anchor, margin, BsUiType::BS_UI_TYPE_GAME);
    if (result) {
        void* hud_font = bs_imgui_get_hud_font();
        if (hud_font) ImGui::PushFont((ImFont*)hud_font);
    }
    return result;
}

void bs_ui_end_hud_panel(void)
{
    void* hud_font = bs_imgui_get_hud_font();
    if (hud_font) ImGui::PopFont();
    bs_ui_end_panel();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}
