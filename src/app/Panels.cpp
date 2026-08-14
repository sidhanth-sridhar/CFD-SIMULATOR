#include "Panels.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <format>
#include <string_view>
#include <utility>

#include "cfd/core/BuildInfo.hpp"

namespace cfd::app {
namespace {

constexpr std::string_view kLogCategory = "ui";

/// Roughly how far apart labelled grid lines should sit, in pixels. Chosen so
/// there is always a labelled reference nearby without the grid becoming a
/// texture.
constexpr double kTargetMajorSpacingPx = 118.0;
constexpr int kMinorPerMajor = 5;

/// Guard against pathological loop counts if the camera ever ends up in a
/// degenerate state; a viewport is never more than a few thousand pixels.
constexpr int kMaxGridLines = 4096;

/// Round a raw spacing up to the nearest "human" value: 1, 2 or 5 times a
/// power of ten. Arbitrary steps like 0.3718 m are unreadable as axis labels.
double niceStep(double raw) {
  if (!std::isfinite(raw) || raw <= 0.0) {
    return 1.0;
  }
  const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
  const double normalized = raw / magnitude;

  double multiple = 10.0;
  if (normalized < 1.5) {
    multiple = 1.0;
  } else if (normalized < 3.5) {
    multiple = 2.0;
  } else if (normalized < 7.5) {
    multiple = 5.0;
  }
  return multiple * magnitude;
}

/// Enough decimals to distinguish adjacent grid lines, and no more.
std::string formatWorld(double value, double step) {
  const int decimals =
      std::clamp(static_cast<int>(std::ceil(-std::log10(step))), 0, 6);
  // Kill "-0.00" for values that are zero to within the grid resolution.
  if (std::abs(value) < step * 1e-6) {
    value = 0.0;
  }
  return std::format("{:.{}f}", value, decimals);
}

// --- small layout helpers --------------------------------------------------

/// Two-column key/value table. Returns false if the table is not visible, in
/// which case EndTable must NOT be called - the caller has to honour that,
/// hence the bool rather than a void helper.
[[nodiscard]] bool beginInfoTable(const char* id, float keyWidth = 104.0f) {
  if (!ImGui::BeginTable(id, 2,
                         ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
    return false;
  }
  ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, keyWidth);
  ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
  return true;
}

void infoRow(const UiState& ui, const char* key, std::string_view value,
             bool monospace = true) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextColored(theme::kTextDim, "%s", key);

  ImGui::TableSetColumnIndex(1);
  if (monospace && ui.fonts.mono != nullptr) {
    ImGui::PushFont(ui.fonts.mono, 0.0f);
  }
  ImGui::TextWrapped("%.*s", static_cast<int>(value.size()), value.data());
  if (monospace && ui.fonts.mono != nullptr) {
    ImGui::PopFont();
  }
}

const ImVec4& colourForLevel(LogLevel level) {
  switch (level) {
    case LogLevel::Trace:    return theme::kLevelTrace;
    case LogLevel::Debug:    return theme::kLevelDebug;
    case LogLevel::Info:     return theme::kLevelInfo;
    case LogLevel::Warning:  return theme::kLevelWarning;
    case LogLevel::Error:    return theme::kLevelError;
    case LogLevel::Critical: return theme::kLevelCritical;
    case LogLevel::Off:      break;
  }
  return theme::kTextDim;
}

// --- viewport drawing ------------------------------------------------------

void drawGridAndAxes(ImDrawList* draw, const UiState& ui, ImVec2 origin, ImVec2 size) {
  const Camera2D& camera = ui.camera;
  const double pixelsPerUnit = camera.pixelsPerUnit();
  const auto [worldMin, worldMax] = camera.visibleBounds();

  const double majorStep = niceStep(kTargetMajorSpacingPx / pixelsPerUnit);
  const double minorStep = majorStep / kMinorPerMajor;

  const ImU32 minorColour = ImGui::GetColorU32(theme::kGridMinor);
  const ImU32 majorColour = ImGui::GetColorU32(theme::kGridMajor);

  // Screen x for a world x, and vice versa. The camera does the real work;
  // these just avoid building a Vec2 at every call site.
  // The explicit widening of origin keeps the whole sum in double; letting a
  // float operand promote implicitly is exactly what -Wdouble-promotion warns
  // about, and in numeric code that habit is worth keeping out.
  const auto screenX = [&](double wx) {
    return static_cast<float>(static_cast<double>(origin.x) +
                              camera.worldToScreen(Vec2{wx, 0.0}).x);
  };
  const auto screenY = [&](double wy) {
    return static_cast<float>(static_cast<double>(origin.y) +
                              camera.worldToScreen(Vec2{0.0, wy}).y);
  };

  const auto drawLinesForStep = [&](double step, ImU32 colour) {
    if (step <= 0.0) {
      return;
    }
    const double firstX = std::floor(worldMin.x / step) * step;
    const double firstY = std::floor(worldMin.y / step) * step;

    const int countX = static_cast<int>((worldMax.x - firstX) / step) + 1;
    const int countY = static_cast<int>((worldMax.y - firstY) / step) + 1;
    if (countX > kMaxGridLines || countY > kMaxGridLines || countX < 0 || countY < 0) {
      return;
    }

    for (int i = 0; i <= countX; ++i) {
      const float sx = screenX(firstX + step * i);
      draw->AddLine(ImVec2(sx, origin.y), ImVec2(sx, origin.y + size.y), colour, 1.0f);
    }
    for (int i = 0; i <= countY; ++i) {
      const float sy = screenY(firstY + step * i);
      draw->AddLine(ImVec2(origin.x, sy), ImVec2(origin.x + size.x, sy), colour, 1.0f);
    }
  };

  if (ui.showGrid) {
    // Only draw the fine grid while it is still resolvable; below ~7 px it
    // stops reading as lines and turns into flicker.
    if (minorStep * pixelsPerUnit >= 7.0) {
      drawLinesForStep(minorStep, minorColour);
    }
    drawLinesForStep(majorStep, majorColour);
  }

  // Axes: x = 0 and y = 0, drawn only when actually on screen.
  if (ui.showAxes) {
    if (worldMin.y <= 0.0 && worldMax.y >= 0.0) {
      const float sy = screenY(0.0);
      draw->AddLine(ImVec2(origin.x, sy), ImVec2(origin.x + size.x, sy),
                    ImGui::GetColorU32(theme::kAxisX), 1.0f);
    }
    if (worldMin.x <= 0.0 && worldMax.x >= 0.0) {
      const float sx = screenX(0.0);
      draw->AddLine(ImVec2(sx, origin.y), ImVec2(sx, origin.y + size.y),
                    ImGui::GetColorU32(theme::kAxisY), 1.0f);
    }
  }

  // Tick labels along the bottom and left edges.
  if (ui.showGrid) {
    const ImU32 labelColour = ImGui::GetColorU32(theme::kTextDim);
    const float labelY = origin.y + size.y - ImGui::GetTextLineHeight() - 4.0f;

    const double firstX = std::floor(worldMin.x / majorStep) * majorStep;
    const int countX = static_cast<int>((worldMax.x - firstX) / majorStep) + 1;
    if (countX >= 0 && countX <= kMaxGridLines) {
      for (int i = 0; i <= countX; ++i) {
        const double wx = firstX + majorStep * i;
        const std::string label = formatWorld(wx, majorStep);
        const float sx = screenX(wx);
        if (sx < origin.x + 2.0f || sx > origin.x + size.x - 30.0f) {
          continue;
        }
        draw->AddText(ImVec2(sx + 3.0f, labelY), labelColour, label.c_str());
      }
    }

    const double firstY = std::floor(worldMin.y / majorStep) * majorStep;
    const int countY = static_cast<int>((worldMax.y - firstY) / majorStep) + 1;
    if (countY >= 0 && countY <= kMaxGridLines) {
      for (int i = 0; i <= countY; ++i) {
        const double wy = firstY + majorStep * i;
        const std::string label = formatWorld(wy, majorStep);
        const float sy = screenY(wy);
        if (sy < origin.y + 2.0f || sy > origin.y + size.y - 18.0f) {
          continue;
        }
        draw->AddText(ImVec2(origin.x + 5.0f, sy + 2.0f), labelColour, label.c_str());
      }
    }
  }
}

/// A conventional CAD scale bar: a segment of known physical length with its
/// value printed. It stays meaningful regardless of zoom or window size, which
/// a bare "zoom %" number does not.
void drawScaleBar(ImDrawList* draw, const UiState& ui, ImVec2 origin, ImVec2 size) {
  const double pixelsPerUnit = ui.camera.pixelsPerUnit();
  const double step = niceStep(kTargetMajorSpacingPx / pixelsPerUnit);
  const float lengthPx = static_cast<float>(step * pixelsPerUnit);
  if (!std::isfinite(lengthPx) || lengthPx < 8.0f || lengthPx > size.x * 0.6f) {
    return;
  }

  const std::string label = std::format("{} m", formatWorld(step, step * 0.1));
  const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());

  const float right = origin.x + size.x - 14.0f;
  const float left = right - lengthPx;
  // Sits clear above the row of axis tick labels along the bottom edge.
  const float baseline = origin.y + size.y - (ImGui::GetTextLineHeight() + 24.0f);

  const ImU32 colour = ImGui::GetColorU32(theme::kTextDim);
  draw->AddLine(ImVec2(left, baseline), ImVec2(right, baseline), colour, 1.0f);
  draw->AddLine(ImVec2(left, baseline - 4.0f), ImVec2(left, baseline + 4.0f), colour, 1.0f);
  draw->AddLine(ImVec2(right, baseline - 4.0f), ImVec2(right, baseline + 4.0f), colour, 1.0f);
  draw->AddText(ImVec2((left + right) * 0.5f - textSize.x * 0.5f,
                       baseline - textSize.y - 5.0f),
                colour, label.c_str());
}

}  // namespace

// ---------------------------------------------------------------------------
// View / layout defaults
// ---------------------------------------------------------------------------

void resetView(UiState& ui) {
  // Frame roughly two chord lengths around the origin. A NACA section is
  // conventionally defined on a unit chord from x = 0 to x = 1, so this is the
  // region the geometry will occupy in Phase 1.
  ui.camera.frameBox(Vec2{-0.35, -0.55}, Vec2{1.35, 0.55}, 0.06);
}

void resetLayout(UiState& ui) {
  ui.leftPanelWidth = 330.0f;
  ui.consoleHeight = 210.0f;
  ui.showLeftPanel = true;
  ui.showConsole = true;
}

// ---------------------------------------------------------------------------
// Menu bar
// ---------------------------------------------------------------------------

void drawMenuBar(UiState& ui) {
  // Only actions that actually do something appear here. Greyed-out menu
  // entries for unimplemented features would be noise.
  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("Quit", "Cmd+Q")) {
      ui.quitRequested = true;
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("View")) {
    ImGui::MenuItem("Session Panel", nullptr, &ui.showLeftPanel);
    ImGui::MenuItem("Log Console", nullptr, &ui.showConsole);
    ImGui::Separator();
    ImGui::MenuItem("Grid", nullptr, &ui.showGrid);
    ImGui::MenuItem("Axes", nullptr, &ui.showAxes);
    ImGui::MenuItem("Scale Bar", nullptr, &ui.showScaleBar);
    ImGui::Separator();
    if (ImGui::MenuItem("Reset View", "F")) {
      resetView(ui);
    }
    if (ImGui::MenuItem("Reset Layout")) {
      resetLayout(ui);
    }
    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("Help")) {
    if (ImGui::MenuItem("About")) {
      ui.aboutRequested = true;
    }
    ImGui::EndMenu();
  }
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------

void drawToolbar(UiState& ui) {
  const float y = ImGui::GetCursorPosY();
  ImGui::SetCursorPosY(y + 2.0f);

  if (ImGui::Button("Reset View")) {
    resetView(ui);
  }
  ImGui::SameLine();
  ImGui::Checkbox("Grid", &ui.showGrid);
  ImGui::SameLine();
  ImGui::Checkbox("Axes", &ui.showAxes);
  ImGui::SameLine();
  ImGui::Checkbox("Scale", &ui.showScaleBar);

  ImGui::SameLine();
  ImGui::TextColored(theme::kTextDisabled, "|");
  ImGui::SameLine();

  ImGui::TextColored(theme::kTextDim, "Zoom");
  ImGui::SameLine();
  if (ui.fonts.mono != nullptr) {
    ImGui::PushFont(ui.fonts.mono, 0.0f);
  }
  ImGui::Text("%.4g px/m", ui.camera.pixelsPerUnit());
  if (ui.fonts.mono != nullptr) {
    ImGui::PopFont();
  }

  // Right-aligned reminder of what the mouse does, in place of a tooltip the
  // user has to hunt for.
  const char* hint = "drag: pan    wheel: zoom";
  const float hintWidth = ImGui::CalcTextSize(hint).x;
  ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - hintWidth);
  ImGui::TextColored(theme::kTextDisabled, "%s", hint);
}

// ---------------------------------------------------------------------------
// Session panel
// ---------------------------------------------------------------------------

void drawSessionPanel(UiState& ui) {
  if (ImGui::CollapsingHeader("Build", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (beginInfoTable("build_info")) {
      infoRow(ui, "Version", BuildInfo::version());
      infoRow(ui, "Config", BuildInfo::buildType());
      infoRow(ui, "Compiler", BuildInfo::compiler());
      infoRow(ui, "Standard", BuildInfo::cxxStandard());
      ImGui::EndTable();
    }
  }

  if (ImGui::CollapsingHeader("Graphics", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (beginInfoTable("gl_info")) {
      infoRow(ui, "Renderer", ui.renderer.renderer);
      infoRow(ui, "Vendor", ui.renderer.vendor);
      infoRow(ui, "OpenGL", ui.renderer.glVersion);
      infoRow(ui, "GLSL", ui.renderer.glslVersion);
      infoRow(ui, "GLFW", ui.renderer.windowSystem);
      ImGui::EndTable();
    }
  }

  if (ImGui::CollapsingHeader("View", ImGuiTreeNodeFlags_DefaultOpen)) {
    const Vec2 centre = ui.camera.center();
    const auto [worldMin, worldMax] = ui.camera.visibleBounds();

    if (beginInfoTable("view_info")) {
      infoRow(ui, "Centre X", std::format("{:+.6g} m", centre.x));
      infoRow(ui, "Centre Y", std::format("{:+.6g} m", centre.y));
      infoRow(ui, "Scale", std::format("{:.6g} px/m", ui.camera.pixelsPerUnit()));
      infoRow(ui, "Span X", std::format("{:.4g} m", worldMax.x - worldMin.x));
      infoRow(ui, "Span Y", std::format("{:.4g} m", worldMax.y - worldMin.y));
      ImGui::EndTable();
    }

    ImGui::Spacing();
    if (ImGui::Button("Reset View", ImVec2(-FLT_MIN, 0.0f))) {
      resetView(ui);
    }
  }

  if (ImGui::CollapsingHeader("Pipeline", ImGuiTreeNodeFlags_DefaultOpen)) {
    // The intended solver chain, shown so the structure of the project is
    // visible from inside it. Every stage is marked as what it is right now:
    // absent. No progress bars, no placeholder numbers.
    ImGui::TextColored(theme::kTextDim, "Planned stages. None are implemented.");
    ImGui::Spacing();

    static constexpr std::array<const char*, 8> kStages{
        "Geometry (NACA)", "Mesh generation", "Navier-Stokes",
        "RANS averaging",  "k-omega SST",     "Force integration",
        "Separation",      "Vortex / stall",
    };

    if (ImGui::BeginTable("pipeline", 2,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_NoSavedSettings)) {
      ImGui::TableSetupColumn("stage", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("status", ImGuiTableColumnFlags_WidthFixed, 96.0f);

      for (const char* stage : kStages) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(theme::kTextDisabled, "%s", stage);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextColored(theme::kTextDisabled, "not implemented");
      }
      ImGui::EndTable();
    }
  }
}

// ---------------------------------------------------------------------------
// Viewport
// ---------------------------------------------------------------------------

void drawViewport(UiState& ui) {
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* draw = ImGui::GetWindowDrawList();

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImVec2 size = ImGui::GetContentRegionAvail();
  size.x = std::max(size.x, 1.0f);
  size.y = std::max(size.y, 1.0f);
  const ImVec2 far_corner(origin.x + size.x, origin.y + size.y);

  ui.camera.setViewportSize(static_cast<double>(size.x), static_cast<double>(size.y));

  if (!ui.viewInitialized) {
    resetView(ui);
    ui.viewInitialized = true;
  }

  // An InvisibleButton the size of the canvas gives us hover and drag state
  // without drawing anything, and lets ImGui arbitrate input so a drag that
  // starts here is not stolen by a panel underneath.
  ImGui::InvisibleButton("viewport_surface", size,
                         ImGuiButtonFlags_MouseButtonLeft |
                             ImGuiButtonFlags_MouseButtonMiddle);
  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();

  if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Left) ||
                 ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
    ui.camera.panByScreenDelta(
        Vec2{static_cast<double>(io.MouseDelta.x), static_cast<double>(io.MouseDelta.y)});
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
  }

  if (hovered && io.MouseWheel != 0.0f) {
    // Geometric steps, so one notch is the same proportional change at every
    // zoom level - the behaviour that feels linear to a user.
    const double factor = std::pow(1.15, static_cast<double>(io.MouseWheel));
    const Vec2 anchor{static_cast<double>(io.MousePos.x - origin.x),
                      static_cast<double>(io.MousePos.y - origin.y)};
    ui.camera.zoomAboutScreenPoint(factor, anchor);
  }

  ui.cursorInViewport = hovered;
  if (hovered) {
    ui.cursorWorld = ui.camera.screenToWorld(
        Vec2{static_cast<double>(io.MousePos.x - origin.x),
             static_cast<double>(io.MousePos.y - origin.y)});
  }

  draw->PushClipRect(origin, far_corner, true);
  draw->AddRectFilled(origin, far_corner, ImGui::GetColorU32(theme::kViewport));

  drawGridAndAxes(draw, ui, origin, size);

  // Empty-state notice. The viewport shows an empty coordinate system because
  // that is genuinely all there is; it must not be dressed up to look like a
  // result.
  {
    const char* primary = "No geometry loaded";
    const char* secondary = "Geometry, meshing and solver stages are not implemented.";

    const ImVec2 primarySize = ImGui::CalcTextSize(primary);
    const ImVec2 secondarySize = ImGui::CalcTextSize(secondary);
    const float centreX = origin.x + size.x * 0.5f;
    // Lifted off the exact centre so the x axis does not run between the two
    // lines and read as a strikethrough.
    const float centreY = origin.y + size.y * 0.5f - 46.0f;

    draw->AddText(ImVec2(centreX - primarySize.x * 0.5f, centreY - primarySize.y - 3.0f),
                  ImGui::GetColorU32(theme::kTextDim), primary);
    draw->AddText(ImVec2(centreX - secondarySize.x * 0.5f, centreY + 4.0f),
                  ImGui::GetColorU32(theme::kTextDisabled), secondary);
  }

  if (ui.showScaleBar) {
    drawScaleBar(draw, ui, origin, size);
  }

  draw->PopClipRect();
  draw->AddRect(origin, far_corner, ImGui::GetColorU32(theme::kBorder));
}

// ---------------------------------------------------------------------------
// Log console
// ---------------------------------------------------------------------------

void drawConsole(UiState& ui) {
  static constexpr std::array<std::pair<const char*, LogLevel>, 7> kLevels{{
      {"Trace", LogLevel::Trace},
      {"Debug", LogLevel::Debug},
      {"Info", LogLevel::Info},
      {"Warning", LogLevel::Warning},
      {"Error", LogLevel::Error},
      {"Critical", LogLevel::Critical},
      {"Off", LogLevel::Off},
  }};

  Logger& logger = Logger::instance();

  ImGui::TextColored(theme::kTextDim, "Level");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(104.0f);
  if (ImGui::BeginCombo("##log_level", std::string{toString(logger.level())}.c_str())) {
    for (const auto& [name, level] : kLevels) {
      const bool selected = (logger.level() == level);
      if (ImGui::Selectable(name, selected)) {
        // Changing this genuinely changes what the whole program records,
        // terminal output included - not just what this panel displays.
        logger.setLevel(level);
        CFD_LOG_INFO(kLogCategory, "log level set to {}", toString(level));
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }

  ImGui::SameLine();
  if (ImGui::Button("Clear") && ui.logBuffer) {
    ui.logBuffer->clear();
    ui.consoleCache.clear();
    ui.cachedRecordCount = 0;
    ui.cachedDroppedCount = 0;
  }
  ImGui::SameLine();
  ImGui::Checkbox("Auto-scroll", &ui.autoScroll);

  // Refresh the displayed copy only when the buffer actually changed. Copying
  // several thousand records every frame would be wasteful for no benefit.
  std::size_t dropped = 0;
  if (ui.logBuffer) {
    const std::size_t count = ui.logBuffer->size();
    dropped = ui.logBuffer->droppedCount();
    if (count != ui.cachedRecordCount || dropped != ui.cachedDroppedCount) {
      ui.consoleCache = ui.logBuffer->snapshot();
      ui.cachedRecordCount = count;
      ui.cachedDroppedCount = dropped;
    }
  }

  {
    const std::string counts =
        dropped > 0
            ? std::format("{} records ({} dropped)", ui.consoleCache.size(), dropped)
            : std::format("{} records", ui.consoleCache.size());
    const float width = ImGui::CalcTextSize(counts.c_str()).x;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - width);
    ImGui::TextColored(theme::kTextDisabled, "%s", counts.c_str());
  }

  ImGui::Separator();

  if (ImGui::BeginChild("log_records", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                        ImGuiWindowFlags_HorizontalScrollbar)) {
    if (ui.fonts.mono != nullptr) {
      ImGui::PushFont(ui.fonts.mono, 0.0f);
    }

    // ImGuiListClipper renders only the rows actually on screen, so a full
    // 4096-record buffer costs the same as a handful of visible lines.
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(ui.consoleCache.size()));
    while (clipper.Step()) {
      for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
        const LogRecord& record = ui.consoleCache[static_cast<std::size_t>(i)];

        ImGui::TextColored(theme::kTextDisabled, "%s",
                           formatTimestamp(record.timestamp).c_str());
        ImGui::SameLine();
        ImGui::TextColored(colourForLevel(record.level), "%-5.*s",
                           static_cast<int>(toString(record.level).size()),
                           toString(record.level).data());
        ImGui::SameLine();
        ImGui::TextColored(theme::kTextDim, "%-8s", record.category.c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted(record.message.c_str());
      }
    }
    clipper.End();

    if (ui.fonts.mono != nullptr) {
      ImGui::PopFont();
    }

    // Only stick to the bottom if the user has not scrolled away.
    if (ui.autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
      ImGui::SetScrollHereY(1.0f);
    }
  }
  ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------

void drawStatusBar(UiState& ui) {
  ImGui::TextColored(theme::kTextDim, "Ready");
  ImGui::SameLine();
  ImGui::TextColored(theme::kTextDisabled, "|");
  ImGui::SameLine();

  if (ui.fonts.mono != nullptr) {
    ImGui::PushFont(ui.fonts.mono, 0.0f);
  }
  if (ui.cursorInViewport) {
    ImGui::Text("x %+9.5f m   y %+9.5f m", ui.cursorWorld.x, ui.cursorWorld.y);
  } else {
    ImGui::TextColored(theme::kTextDisabled, "x %9s     y %9s", "-", "-");
  }

  const std::string right =
      std::format("{:5.2f} ms cpu    {} {}", static_cast<double>(ui.lastFrameCpuMs),
                  BuildInfo::projectName(), BuildInfo::version());
  const float width = ImGui::CalcTextSize(right.c_str()).x;
  ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - width);
  ImGui::TextColored(theme::kTextDim, "%s", right.c_str());

  if (ui.fonts.mono != nullptr) {
    ImGui::PopFont();
  }
}

// ---------------------------------------------------------------------------
// About
// ---------------------------------------------------------------------------

void drawAboutModal(UiState& ui) {
  if (ui.aboutRequested) {
    ImGui::OpenPopup("About");
    ui.aboutRequested = false;
  }

  const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_Appearing);

  if (ImGui::BeginPopupModal("About", nullptr,
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
    // string_view::data() is not guaranteed null-terminated in general; use a
    // precision-limited conversion rather than relying on where these happen
    // to come from.
    const std::string_view name = BuildInfo::projectName();
    const std::string_view versionText = BuildInfo::version();
    const std::string_view description = BuildInfo::description();

    ImGui::Text("%.*s %.*s", static_cast<int>(name.size()), name.data(),
                static_cast<int>(versionText.size()), versionText.data());
    ImGui::TextColored(theme::kTextDim, "%.*s", static_cast<int>(description.size()),
                       description.data());
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (beginInfoTable("about_build", 86.0f)) {
      infoRow(ui, "Config", BuildInfo::buildType());
      infoRow(ui, "Compiler", BuildInfo::compiler());
      infoRow(ui, "Standard", BuildInfo::cxxStandard());
      ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Third-party components");
    if (beginInfoTable("about_deps", 86.0f)) {
      infoRow(ui, "Dear ImGui", IMGUI_VERSION);
      infoRow(ui, "GLFW", ui.renderer.windowSystem);
      infoRow(ui, "OpenGL", ui.renderer.glVersion);
      ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextColored(theme::kTextDisabled,
                       "Phase 0: application foundation. No CFD implemented.");
    ImGui::Spacing();

    if (ImGui::Button("Close", ImVec2(96.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

}  // namespace cfd::app
