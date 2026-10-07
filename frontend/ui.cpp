#include "ui.h"
#include "config.h"

namespace ui {
ImFont* body = nullptr;
ImFont* bold = nullptr;
ImFont* title = nullptr;

static ImFont* loadFont(const char* file, float size) {
    const std::string candidates[] = {
        baseDir() + "fonts/" + file,
        baseDir() + "../assets/fonts/" + file,                  // running from build/
        baseDir() + "../../assets/fonts/" + file,               // running from build/
        std::string("/usr/share/fonts/truetype/roboto/unhinted/RobotoTTF/") + file,
    };
    for (auto& p : candidates)
        if (fileExists(p)) {
            ImFontConfig fc;
            fc.OversampleH = 2;
            return ImGui::GetIO().Fonts->AddFontFromFileTTF(p.c_str(), size, &fc);
        }
    return nullptr;
}

void setup(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    body = loadFont("Roboto-Medium.ttf", 17.0f * scale);
    bold = loadFont("Roboto-Bold.ttf", 17.0f * scale);
    title = loadFont("Roboto-Bold.ttf", 30.0f * scale);
    if (!body) {   // fall back to the built-in font
        ImFontConfig fc;
        fc.SizePixels = 13.0f * scale;
        body = io.Fonts->AddFontDefault(&fc);
        bold = title = body;
    }
    io.FontDefault = body;

    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 10; s.ChildRounding = 8; s.FrameRounding = 6; s.PopupRounding = 8;
    s.GrabRounding = 6; s.TabRounding = 6; s.ScrollbarRounding = 8;
    s.WindowPadding = ImVec2(18, 16); s.FramePadding = ImVec2(10, 7);
    s.ItemSpacing = ImVec2(10, 9); s.ItemInnerSpacing = ImVec2(8, 6);
    s.WindowBorderSize = 0; s.FrameBorderSize = 0; s.TabBorderSize = 0;
    s.ScrollbarSize = 14; s.GrabMinSize = 12;
    s.ScaleAllSizes(scale);

    ImVec4* c = s.Colors;
    const ImVec4 bg(0.055f, 0.071f, 0.098f, 1), panel(0.086f, 0.110f, 0.149f, 1), frame(0.125f, 0.157f, 0.208f, 1);
    const ImVec4 frameHi(0.165f, 0.212f, 0.282f, 1), acc = accent(), accHi(0.38f, 0.70f, 1.0f, 1);
    c[ImGuiCol_Text] = ImVec4(0.92f, 0.94f, 0.97f, 1);
    c[ImGuiCol_TextDisabled] = dim();
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = panel;
    c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.13f, 0.18f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.07f);
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = frameHi;
    c[ImGuiCol_FrameBgActive] = frameHi;
    c[ImGuiCol_Button] = frame;
    c[ImGuiCol_ButtonHovered] = frameHi;
    c[ImGuiCol_ButtonActive] = ImVec4(acc.x * 0.6f, acc.y * 0.6f, acc.z * 0.6f, 1);
    c[ImGuiCol_Header] = ImVec4(acc.x, acc.y, acc.z, 0.25f);
    c[ImGuiCol_HeaderHovered] = ImVec4(acc.x, acc.y, acc.z, 0.35f);
    c[ImGuiCol_HeaderActive] = ImVec4(acc.x, acc.y, acc.z, 0.45f);
    c[ImGuiCol_CheckMark] = accHi;
    c[ImGuiCol_SliderGrab] = acc;
    c[ImGuiCol_SliderGrabActive] = accHi;
    c[ImGuiCol_Tab] = panel;
    c[ImGuiCol_TabHovered] = frameHi;
    c[ImGuiCol_TabActive] = ImVec4(acc.x * 0.55f, acc.y * 0.55f, acc.z * 0.55f, 1);
    c[ImGuiCol_TabUnfocusedActive] = c[ImGuiCol_TabActive];
    c[ImGuiCol_Separator] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
    c[ImGuiCol_TableBorderLight] = ImVec4(1, 1, 1, 0.05f);
    c[ImGuiCol_TableHeaderBg] = panel;
    c[ImGuiCol_NavHighlight] = accHi;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
}

void help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void section(const char* text) {
    ImGui::Spacing();
    ImGui::PushFont(bold);
    ImGui::TextColored(accent(), "%s", text);
    ImGui::PopFont();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y - 4), ImVec2(p.x + w, p.y - 4), IM_COL32(64, 156, 255, 60), 1.0f);
    ImGui::Spacing();
}
}
