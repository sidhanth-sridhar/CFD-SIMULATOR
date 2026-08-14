#include "Theme.hpp"

#include <array>
#include <filesystem>

#include "cfd/core/Log.hpp"

namespace cfd::app::theme {
namespace {

constexpr std::string_view kLogCategory = "ui";

/// Candidate font files, most preferred first, across the platforms this is
/// likely to be built on. Missing entries are skipped silently; only a total
/// failure is worth a log line.
const std::array<const char*, 9> kUiFontCandidates{
    "/System/Library/Fonts/SFNS.ttf",
    "/System/Library/Fonts/HelveticaNeue.ttc",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "C:/Windows/Fonts/segoeui.ttf",
    "C:/Windows/Fonts/arial.ttf",
    nullptr,
};

const std::array<const char*, 9> kMonoFontCandidates{
    "/System/Library/Fonts/SFNSMono.ttf",
    "/System/Library/Fonts/Menlo.ttc",
    "/System/Library/Fonts/Monaco.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    "C:/Windows/Fonts/consola.ttf",
    "C:/Windows/Fonts/cour.ttf",
    nullptr,
};

ImFont* loadFirstAvailable(ImGuiIO& io, const std::array<const char*, 9>& candidates,
                           float sizePixels) {
  std::error_code ec;
  for (const char* path : candidates) {
    if (path == nullptr) {
      break;
    }
    if (!std::filesystem::exists(path, ec) || ec) {
      ec.clear();
      continue;
    }
    if (ImFont* font = io.Fonts->AddFontFromFileTTF(path, sizePixels); font != nullptr) {
      CFD_LOG_DEBUG(kLogCategory, "loaded font {}", path);
      return font;
    }
  }
  return nullptr;
}

}  // namespace

void apply(ImGuiStyle& style) {
  // --- geometry -----------------------------------------------------------
  // Every rounding radius is zero. Rounded panels read as consumer software;
  // technical tools are built out of rectangles, and square corners also make
  // aligned edges across panels visibly aligned.
  style.WindowRounding = 0.0f;
  style.ChildRounding = 0.0f;
  style.FrameRounding = 0.0f;
  style.PopupRounding = 0.0f;
  style.ScrollbarRounding = 0.0f;
  style.GrabRounding = 0.0f;
  style.TabRounding = 0.0f;

  // Hairline borders everywhere: regions are separated by a line rather than
  // by shadows, gaps or elevation.
  style.WindowBorderSize = 1.0f;
  style.ChildBorderSize = 1.0f;
  style.PopupBorderSize = 1.0f;
  style.FrameBorderSize = 1.0f;
  style.TabBorderSize = 0.0f;

  // Compact spacing. The aim is information density: many labelled values
  // visible at once without scrolling.
  style.WindowPadding = ImVec2(8.0f, 6.0f);
  style.FramePadding = ImVec2(6.0f, 3.0f);
  style.CellPadding = ImVec2(6.0f, 2.0f);
  style.ItemSpacing = ImVec2(7.0f, 4.0f);
  style.ItemInnerSpacing = ImVec2(5.0f, 4.0f);
  style.IndentSpacing = 16.0f;
  style.ScrollbarSize = 11.0f;
  style.GrabMinSize = 9.0f;

  style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
  style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
  style.SeparatorTextBorderSize = 1.0f;
  style.SeparatorTextPadding = ImVec2(14.0f, 2.0f);

  // --- colours ------------------------------------------------------------
  ImVec4* c = style.Colors;

  c[ImGuiCol_Text] = kText;
  c[ImGuiCol_TextDisabled] = kTextDisabled;
  c[ImGuiCol_WindowBg] = kPanel;
  c[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
  c[ImGuiCol_PopupBg] = kPanelHeader;
  c[ImGuiCol_Border] = kBorder;
  c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);  // no fake depth

  c[ImGuiCol_FrameBg] = kControl;
  c[ImGuiCol_FrameBgHovered] = kControlHovered;
  c[ImGuiCol_FrameBgActive] = kControlActive;

  c[ImGuiCol_TitleBg] = kPanelHeader;
  c[ImGuiCol_TitleBgActive] = kPanelHeader;
  c[ImGuiCol_TitleBgCollapsed] = kPanelHeader;
  c[ImGuiCol_MenuBarBg] = kPanelHeader;

  c[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
  c[ImGuiCol_ScrollbarGrab] = kControlActive;
  c[ImGuiCol_ScrollbarGrabHovered] = kBorder;
  c[ImGuiCol_ScrollbarGrabActive] = kAccentDim;

  c[ImGuiCol_CheckMark] = kAccent;
  c[ImGuiCol_SliderGrab] = kAccentDim;
  c[ImGuiCol_SliderGrabActive] = kAccent;

  c[ImGuiCol_Button] = kControlHovered;
  c[ImGuiCol_ButtonHovered] = kControlActive;
  c[ImGuiCol_ButtonActive] = kAccentDim;

  c[ImGuiCol_Header] = kControlHovered;
  c[ImGuiCol_HeaderHovered] = kControlActive;
  c[ImGuiCol_HeaderActive] = kAccentDim;

  c[ImGuiCol_Separator] = kBorder;
  c[ImGuiCol_SeparatorHovered] = kAccentDim;
  c[ImGuiCol_SeparatorActive] = kAccent;

  c[ImGuiCol_ResizeGrip] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
  c[ImGuiCol_ResizeGripHovered] = kAccentDim;
  c[ImGuiCol_ResizeGripActive] = kAccent;

  c[ImGuiCol_Tab] = kPanel;
  c[ImGuiCol_TabHovered] = kControlActive;
  c[ImGuiCol_TabSelected] = kPanelHeader;
  c[ImGuiCol_TabSelectedOverline] = kAccent;
  c[ImGuiCol_TabDimmed] = kPanel;
  c[ImGuiCol_TabDimmedSelected] = kPanelHeader;

  c[ImGuiCol_TableHeaderBg] = kPanelHeader;
  c[ImGuiCol_TableBorderStrong] = kBorder;
  c[ImGuiCol_TableBorderLight] = ImVec4(0.176f, 0.192f, 0.208f, 1.0f);
  c[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
  c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.016f);

  c[ImGuiCol_TextSelectedBg] = kAccentDim;
  c[ImGuiCol_NavCursor] = kAccent;
  c[ImGuiCol_DragDropTarget] = kAccent;
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.45f);
}

Fonts loadFonts(ImGuiIO& io, float sizePixels) {
  Fonts fonts;
  fonts.ui = loadFirstAvailable(io, kUiFontCandidates, sizePixels);

  // Slightly smaller: monospace faces have a larger apparent size than
  // proportional ones at equal nominal point size.
  fonts.mono = loadFirstAvailable(io, kMonoFontCandidates, sizePixels - 1.0f);

  if (fonts.ui == nullptr && fonts.mono == nullptr) {
    CFD_LOG_WARN(kLogCategory,
                 "no system fonts found; falling back to the built-in bitmap font");
    io.Fonts->AddFontDefault();
  }
  return fonts;
}

}  // namespace cfd::app::theme
