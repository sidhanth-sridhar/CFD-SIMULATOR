#include "Panels.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <string_view>
#include <utility>
#include <vector>

#include "cfd/core/BuildInfo.hpp"
#include "cfd/geom/Naca4.hpp"

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

// --- geometry drawing -----------------------------------------------------

/// Straight line broken into dashes of a fixed pixel length. Used for the
/// chord and camber lines so they read as construction references rather than
/// as part of the surface.
void addDashedPolyline(ImDrawList* draw, const std::vector<ImVec2>& points, ImU32 colour,
                       float thickness, float dashPx = 7.0f, float gapPx = 5.0f) {
  const float period = dashPx + gapPx;
  float travelled = 0.0f;  // arc length carried across segment boundaries

  for (std::size_t i = 0; i + 1 < points.size(); ++i) {
    const ImVec2 a = points[i];
    const ImVec2 b = points[i + 1];
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float segmentLength = std::sqrt(dx * dx + dy * dy);
    if (segmentLength <= 0.0f) {
      continue;
    }

    float position = 0.0f;
    while (position < segmentLength) {
      // Where we are within the current dash/gap cycle.
      const float phase = std::fmod(travelled + position, period);
      const float remaining = (phase < dashPx) ? (dashPx - phase) : (period - phase);
      const float step = std::min(remaining, segmentLength - position);

      if (phase < dashPx) {
        const float t0 = position / segmentLength;
        const float t1 = (position + step) / segmentLength;
        draw->AddLine(ImVec2(a.x + dx * t0, a.y + dy * t0),
                      ImVec2(a.x + dx * t1, a.y + dy * t1), colour, thickness);
      }
      position += step;
    }
    travelled += segmentLength;
  }
}

/// Roughly how many line segments we are willing to submit for the mesh in one
/// frame. Above this the view is decimated; the panel says so.
constexpr int kMeshSegmentBudget = 60000;

/// Draw the grid.
///
/// Two costs have to be kept under control. A fine grid is ~830 x 128 nodes,
/// which is over 200,000 line segments - more than is either fast to submit or
/// meaningful to look at when the whole 25-chord domain is on screen.
///
///   * Culling. Segments outside the visible world rectangle are skipped, and
///     runs of consecutive visible ones are batched into a single polyline.
///     This is what makes zooming into the leading edge cheap: almost the
///     entire grid is off screen.
///   * Decimation. If what survives culling is still over budget, every n-th
///     grid line is drawn. That is a lie about the mesh, so the stride is
///     reported back and shown in the panel rather than hidden.
void drawMeshLines(ImDrawList* draw, UiState& ui, ImVec2 origin) {
  const mesh::Mesh& grid = *ui.meshing.mesh;
  if (!grid.isStructured()) {
    return;
  }
  const int ni = grid.nodesI();
  const int nj = grid.nodesJ();
  const std::vector<Vec2>& nodes = grid.nodes();

  const auto [worldMin, worldMax] = ui.camera.visibleBounds();
  const auto segmentVisible = [&](const Vec2& a, const Vec2& b) {
    return !(std::max(a.x, b.x) < worldMin.x || std::min(a.x, b.x) > worldMax.x ||
             std::max(a.y, b.y) < worldMin.y || std::min(a.y, b.y) > worldMax.y);
  };
  const auto toScreen = [&](const Vec2& world) {
    const Vec2 s = ui.camera.worldToScreen(world);
    return ImVec2(static_cast<float>(static_cast<double>(origin.x) + s.x),
                  static_cast<float>(static_cast<double>(origin.y) + s.y));
  };
  const auto nodeAt = [&](int i, int j) -> const Vec2& {
    return nodes[static_cast<std::size_t>(j) * static_cast<std::size_t>(ni) +
                 static_cast<std::size_t>(i)];
  };

  // First pass: how much of the grid actually lands on screen?
  long long visible = 0;
  for (int j = 0; j < nj; ++j) {
    for (int i = 0; i + 1 < ni; ++i) {
      if (segmentVisible(nodeAt(i, j), nodeAt(i + 1, j))) {
        ++visible;
      }
    }
  }
  for (int i = 0; i < ni; ++i) {
    for (int j = 0; j + 1 < nj; ++j) {
      if (segmentVisible(nodeAt(i, j), nodeAt(i, j + 1))) {
        ++visible;
      }
    }
  }

  const int stride =
      (visible > kMeshSegmentBudget)
          ? static_cast<int>((visible + kMeshSegmentBudget - 1) / kMeshSegmentBudget)
          : 1;
  ui.meshing.drawStride = stride;

  const ImU32 colour = ImGui::GetColorU32(theme::kMeshLine);
  std::vector<ImVec2> run;
  run.reserve(static_cast<std::size_t>(std::max(ni, nj)));

  const auto flush = [&]() {
    if (run.size() >= 2) {
      draw->AddPolyline(run.data(), static_cast<int>(run.size()), colour, 1.0f, 0);
    }
    run.clear();
  };

  // Lines of constant j, running along the surface and out into the wake.
  for (int j = 0; j < nj; j += stride) {
    for (int i = 0; i + 1 < ni; ++i) {
      const Vec2& a = nodeAt(i, j);
      const Vec2& b = nodeAt(i + 1, j);
      if (!segmentVisible(a, b)) {
        flush();
        continue;
      }
      if (run.empty()) {
        run.push_back(toScreen(a));
      }
      run.push_back(toScreen(b));
    }
    flush();
  }

  // Lines of constant i, running from the wall out to the far field.
  for (int i = 0; i < ni; i += stride) {
    for (int j = 0; j + 1 < nj; ++j) {
      const Vec2& a = nodeAt(i, j);
      const Vec2& b = nodeAt(i, j + 1);
      if (!segmentVisible(a, b)) {
        flush();
        continue;
      }
      if (run.empty()) {
        run.push_back(toScreen(a));
      }
      run.push_back(toScreen(b));
    }
    flush();
  }
}

/// Value the field view asks for, in cell `c`.
double fieldValue(const UiState& ui, FieldView view, std::size_t c) {
  const flow::FlowField& field = *ui.flow.field;
  switch (view) {
    case FieldView::VelocityMagnitude: return length(field.velocity[c]);
    case FieldView::VelocityX:         return field.velocity[c].x;
    case FieldView::VelocityY:         return field.velocity[c].y;
    case FieldView::Pressure:          return field.pressure[c];
    case FieldView::Divergence:
      return (c < ui.flow.divergence.size()) ? ui.flow.divergence[c] : 0.0;
  }
  return 0.0;
}

/// Shade every visible cell by the selected scalar.
///
/// Cells are drawn as two triangles each with anti-aliasing off, so
/// neighbouring cells tile exactly instead of blending along every shared
/// edge - the same reason the section's fill is built that way.
void drawScalarField(ImDrawList* draw, UiState& ui, ImVec2 origin) {
  const mesh::Mesh& grid = *ui.meshing.mesh;
  const flow::FlowField& field = *ui.flow.field;
  if (field.size() != grid.cellCount()) {
    return;
  }

  const auto [worldMin, worldMax] = ui.camera.visibleBounds();
  const auto toScreen = [&](const Vec2& world) {
    const Vec2 s = ui.camera.worldToScreen(world);
    return ImVec2(static_cast<float>(static_cast<double>(origin.x) + s.x),
                  static_cast<float>(static_cast<double>(origin.y) + s.y));
  };

  const bool signedField = isSignedField(ui.flow.view);
  double low = ui.flow.rangeMin;
  double high = ui.flow.rangeMax;
  if (high <= low) {
    // A uniform field has no range at all. Widen it a touch so the map returns
    // its midpoint rather than dividing by zero.
    const double pad = std::max(std::abs(high), 1.0) * 1e-6;
    low -= pad;
    high += pad;
  }
  const double span = high - low;

  const ImDrawListFlags savedFlags = draw->Flags;
  draw->Flags &= ~ImDrawListFlags_AntiAliasedFill;

  std::size_t shaded = 0;
  for (std::size_t c = 0; c < grid.cellCount(); ++c) {
    const std::array<int, 4>& corners = grid.cellNodes()[c];
    const Vec2& p0 = grid.nodes()[static_cast<std::size_t>(corners[0])];
    const Vec2& p1 = grid.nodes()[static_cast<std::size_t>(corners[1])];
    const Vec2& p2 = grid.nodes()[static_cast<std::size_t>(corners[2])];
    const Vec2& p3 = grid.nodes()[static_cast<std::size_t>(corners[3])];

    const double cellMinX = std::min({p0.x, p1.x, p2.x, p3.x});
    const double cellMaxX = std::max({p0.x, p1.x, p2.x, p3.x});
    const double cellMinY = std::min({p0.y, p1.y, p2.y, p3.y});
    const double cellMaxY = std::max({p0.y, p1.y, p2.y, p3.y});
    if (cellMaxX < worldMin.x || cellMinX > worldMax.x || cellMaxY < worldMin.y ||
        cellMinY > worldMax.y) {
      continue;
    }

    const double t = (fieldValue(ui, ui.flow.view, c) - low) / span;
    const ImU32 colour =
        signedField ? theme::divergingColour(t) : theme::sequentialColour(t);

    const ImVec2 a = toScreen(p0);
    const ImVec2 b = toScreen(p1);
    const ImVec2 d = toScreen(p2);
    const ImVec2 e = toScreen(p3);
    draw->AddTriangleFilled(a, b, d, colour);
    draw->AddTriangleFilled(a, d, e, colour);
    ++shaded;
  }

  draw->Flags = savedFlags;
  ui.flow.shadedCells = shaded;
  ui.flow.shadingComplete = true;
}

/// Colour bar for the shaded field, bottom-left of the canvas.
void drawFieldLegend(ImDrawList* draw, const UiState& ui, ImVec2 origin, ImVec2 size) {
  constexpr float kBarWidth = 16.0f;
  const float barHeight = std::min(180.0f, size.y * 0.4f);
  if (barHeight < 40.0f) {
    return;
  }

  const float left = origin.x + 14.0f;
  const float bottom = origin.y + size.y - 42.0f;
  const float top = bottom - barHeight;

  const bool signedField = isSignedField(ui.flow.view);
  constexpr int kBands = 48;
  for (int i = 0; i < kBands; ++i) {
    const double t0 = static_cast<double>(i) / kBands;
    const double t1 = static_cast<double>(i + 1) / kBands;
    // Drawn bottom-up so the top of the bar is the maximum.
    const float y0 = bottom - static_cast<float>(t1) * barHeight;
    const float y1 = bottom - static_cast<float>(t0) * barHeight;
    const ImU32 colour = signedField ? theme::divergingColour(0.5 * (t0 + t1))
                                     : theme::sequentialColour(0.5 * (t0 + t1));
    draw->AddRectFilled(ImVec2(left, y0), ImVec2(left + kBarWidth, y1), colour);
  }
  draw->AddRect(ImVec2(left, top), ImVec2(left + kBarWidth, bottom),
                ImGui::GetColorU32(theme::kBorder));

  const ImU32 textColour = ImGui::GetColorU32(theme::kTextDim);
  const std::string title{toString(ui.flow.view)};
  draw->AddText(ImVec2(left, top - ImGui::GetTextLineHeight() - 4.0f), textColour,
                title.c_str());

  // A uniform field has no range to label. Printing the same number at both
  // ends of the bar would suggest a variation that is not there, so say what
  // is actually true instead.
  const double spread = ui.flow.rangeMax - ui.flow.rangeMin;
  const double magnitude = std::max(std::abs(ui.flow.rangeMax), std::abs(ui.flow.rangeMin));
  if (spread <= magnitude * 1e-12) {
    draw->AddText(ImVec2(left + kBarWidth + 6.0f, top - 2.0f), textColour,
                  std::format("uniform {:.4g}", ui.flow.rangeMax).c_str());
    return;
  }

  draw->AddText(ImVec2(left + kBarWidth + 6.0f, top - 2.0f), textColour,
                std::format("{:.4g}", ui.flow.rangeMax).c_str());
  draw->AddText(ImVec2(left + kBarWidth + 6.0f, bottom - ImGui::GetTextLineHeight() + 2.0f),
                textColour, std::format("{:.4g}", ui.flow.rangeMin).c_str());
}

/// Draw boundary faces coloured by the physical condition applied to them,
/// rather than by the mesh patch they belong to. The distinction matters on
/// the far field, where the same patch acts as an inlet at the front and an
/// outlet behind.
void drawBoundaryKinds(ImDrawList* draw, const UiState& ui, ImVec2 origin) {
  const mesh::Mesh& grid = *ui.meshing.mesh;
  const flow::FaceState& faces = *ui.flow.faces;
  if (faces.size() != grid.faceCount()) {
    return;
  }

  const auto toScreen = [&](const Vec2& world) {
    const Vec2 s = ui.camera.worldToScreen(world);
    return ImVec2(static_cast<float>(static_cast<double>(origin.x) + s.x),
                  static_cast<float>(static_cast<double>(origin.y) + s.y));
  };
  const auto [worldMin, worldMax] = ui.camera.visibleBounds();

  for (std::size_t f = 0; f < grid.faceCount(); ++f) {
    const mesh::Face& face = grid.faces()[f];
    if (face.isInterior()) {
      continue;
    }

    const Vec2& a = grid.nodes()[static_cast<std::size_t>(face.nodes[0])];
    const Vec2& b = grid.nodes()[static_cast<std::size_t>(face.nodes[1])];
    if (std::max(a.x, b.x) < worldMin.x || std::min(a.x, b.x) > worldMax.x ||
        std::max(a.y, b.y) < worldMin.y || std::min(a.y, b.y) > worldMax.y) {
      continue;
    }

    const ImVec4* colour = &theme::kBcInternal;
    switch (faces.kind[f]) {
      case flow::BoundaryKind::NoSlipWall: colour = &theme::kBcWall; break;
      case flow::BoundaryKind::Inlet:      colour = &theme::kBcInlet; break;
      case flow::BoundaryKind::Outlet:     colour = &theme::kBcOutlet; break;
      case flow::BoundaryKind::FarField:
        colour = (faces.inflow[f] != 0) ? &theme::kBcFarFieldIn : &theme::kBcFarFieldOut;
        break;
      case flow::BoundaryKind::Internal:   colour = &theme::kBcInternal; break;
    }
    draw->AddLine(toScreen(a), toScreen(b), ImGui::GetColorU32(*colour), 1.8f);
  }
}

/// Arrow showing the direction the stream arrives from, drawn in a fixed
/// corner of the canvas rather than in world space so it stays legible at any
/// zoom.
void drawFreestreamIndicator(ImDrawList* draw, const UiState& ui, ImVec2 origin,
                             ImVec2 size) {
  const double alpha = ui.flow.freestream.angleOfAttackRad();
  // Screen y grows downward, so a positive angle of attack points up-screen.
  const auto dx = static_cast<float>(std::cos(alpha));
  const auto dy = static_cast<float>(-std::sin(alpha));

  constexpr float kLength = 46.0f;
  const ImVec2 centre(origin.x + size.x - 78.0f, origin.y + 42.0f);
  const ImVec2 tail(centre.x - dx * kLength * 0.5f, centre.y - dy * kLength * 0.5f);
  const ImVec2 head(centre.x + dx * kLength * 0.5f, centre.y + dy * kLength * 0.5f);

  const ImU32 colour = ImGui::GetColorU32(theme::kBcInlet);
  draw->AddLine(tail, head, colour, 1.8f);
  // Arrow head: two short strokes swept back from the tip.
  const float back = 9.0f;
  const float side = 5.0f;
  draw->AddLine(head, ImVec2(head.x - dx * back - dy * side, head.y - dy * back + dx * side),
                colour, 1.8f);
  draw->AddLine(head, ImVec2(head.x - dx * back + dy * side, head.y - dy * back - dx * side),
                colour, 1.8f);

  const std::string label = std::format("U {:.4g} m/s   a {:.2f} deg",
                                        ui.flow.freestream.speed,
                                        ui.flow.freestream.angleOfAttackDeg);
  const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
  draw->AddText(ImVec2(centre.x - textSize.x * 0.5f, centre.y + 22.0f),
                ImGui::GetColorU32(theme::kTextDim), label.c_str());
}

/// Draw boundary faces coloured by the condition they will carry.
void drawMeshBoundaries(ImDrawList* draw, const UiState& ui, ImVec2 origin) {
  const mesh::Mesh& grid = *ui.meshing.mesh;

  const auto toScreen = [&](const Vec2& world) {
    const Vec2 s = ui.camera.worldToScreen(world);
    return ImVec2(static_cast<float>(static_cast<double>(origin.x) + s.x),
                  static_cast<float>(static_cast<double>(origin.y) + s.y));
  };

  const auto [worldMin, worldMax] = ui.camera.visibleBounds();

  for (std::size_t f = 0; f < grid.faceCount(); ++f) {
    const mesh::Face& face = grid.faces()[f];
    if (face.isInterior()) {
      continue;
    }

    const Vec2& a = grid.nodes()[static_cast<std::size_t>(face.nodes[0])];
    const Vec2& b = grid.nodes()[static_cast<std::size_t>(face.nodes[1])];
    if (std::max(a.x, b.x) < worldMin.x || std::min(a.x, b.x) > worldMax.x ||
        std::max(a.y, b.y) < worldMin.y || std::min(a.y, b.y) > worldMax.y) {
      continue;
    }

    const ImVec4* colour = &theme::kBoundaryWall;
    switch (face.boundary) {
      case mesh::BoundaryType::Wall:     colour = &theme::kBoundaryWall; break;
      case mesh::BoundaryType::Farfield: colour = &theme::kBoundaryFarfield; break;
      case mesh::BoundaryType::Outlet:   colour = &theme::kBoundaryOutlet; break;
      case mesh::BoundaryType::WakeCut:  colour = &theme::kBoundaryWakeCut; break;
      case mesh::BoundaryType::Interior: continue;
    }
    draw->AddLine(toScreen(a), toScreen(b), ImGui::GetColorU32(*colour), 1.6f);
  }
}

/// Draw the generated section: fill, outline, construction lines and markers.
void drawAirfoil(ImDrawList* draw, const UiState& ui, ImVec2 origin) {
  if (!ui.geometry.airfoil.has_value()) {
    return;
  }
  const geom::Airfoil& foil = *ui.geometry.airfoil;

  const auto toScreen = [&](const Vec2& world) {
    const Vec2 s = ui.camera.worldToScreen(world);
    return ImVec2(static_cast<float>(static_cast<double>(origin.x) + s.x),
                  static_cast<float>(static_cast<double>(origin.y) + s.y));
  };

  // The contour repeats its first point to close the loop; ImGui closes the
  // shape itself, so hand it only the distinct points.
  const std::vector<Vec2>& contour = foil.contour();
  std::vector<ImVec2> screen;
  screen.reserve(contour.size());
  for (std::size_t i = 0; i + 1 < contour.size(); ++i) {
    screen.push_back(toScreen(contour[i]));
  }
  if (screen.size() < 3) {
    return;
  }

  if (ui.geometry.fillSection) {
    // Fill as a strip of quads between corresponding upper and lower surface
    // points, rather than by handing the outline to a general polygon filler.
    //
    // Dear ImGui's AddConcavePolyFilled ear-clips an arbitrary polygon. That
    // is O(n^2) and numerically fragile on a shape as thin and as finely
    // sampled as an aerofoil: at a few hundred points per surface, and
    // especially on strongly cambered sections such as NACA 9410, it emits
    // triangles that spill well outside the outline.
    //
    // No general triangulation is needed here. The section is *defined* as two
    // surfaces evaluated at the same chordwise stations, so the region between
    // them tiles exactly into quads - no search, no heuristics, and linear in
    // the point count. Anti-aliasing is switched off for the fill so that
    // adjacent triangles meet exactly instead of blending against each other
    // along every shared edge; the outline drawn afterwards keeps the visible
    // silhouette smooth.
    const std::vector<Vec2>& upper = foil.upper();
    const std::vector<Vec2>& lower = foil.lower();
    const ImU32 fill = ImGui::GetColorU32(theme::kAirfoilFill);

    const ImDrawListFlags savedFlags = draw->Flags;
    draw->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    for (std::size_t i = 0; i + 1 < upper.size(); ++i) {
      const ImVec2 a = toScreen(upper[i]);
      const ImVec2 b = toScreen(upper[i + 1]);
      const ImVec2 c = toScreen(lower[i + 1]);
      const ImVec2 d = toScreen(lower[i]);
      draw->AddTriangleFilled(a, b, c, fill);
      draw->AddTriangleFilled(a, c, d, fill);
    }
    draw->Flags = savedFlags;
  }

  if (ui.geometry.showChordLine) {
    const std::vector<ImVec2> chord{toScreen(Vec2{0.0, 0.0}),
                                    toScreen(Vec2{foil.chord(), 0.0})};
    addDashedPolyline(draw, chord, ImGui::GetColorU32(theme::kChordLine), 1.0f);
  }

  if (ui.geometry.showCamberLine && !foil.designation().isSymmetric()) {
    // Omitted for symmetric sections, where it would sit exactly on the chord
    // line and only produce a shimmering overlap.
    std::vector<ImVec2> camber;
    camber.reserve(foil.camberLine().size());
    for (const Vec2& point : foil.camberLine()) {
      camber.push_back(toScreen(point));
    }
    addDashedPolyline(draw, camber, ImGui::GetColorU32(theme::kCamberLine), 1.4f);
  }

  // Argument order is (colour, thickness, flags) as of ImGui 1.92.8.
  draw->AddPolyline(screen.data(), static_cast<int>(screen.size()),
                    ImGui::GetColorU32(theme::kAirfoilOutline), 1.6f, ImDrawFlags_Closed);

  if (ui.geometry.showSurfacePoints) {
    const ImU32 colour = ImGui::GetColorU32(theme::kSurfacePoint);
    for (const ImVec2& point : screen) {
      draw->AddRectFilled(ImVec2(point.x - 1.5f, point.y - 1.5f),
                          ImVec2(point.x + 1.5f, point.y + 1.5f), colour);
    }
  }

  // Leading and trailing edge markers, as small crosses.
  const ImU32 markerColour = ImGui::GetColorU32(theme::kMarker);
  for (const Vec2& world : {foil.leadingEdge(), foil.trailingEdge()}) {
    const ImVec2 p = toScreen(world);
    constexpr float r = 4.0f;
    draw->AddLine(ImVec2(p.x - r, p.y), ImVec2(p.x + r, p.y), markerColour, 1.0f);
    draw->AddLine(ImVec2(p.x, p.y - r), ImVec2(p.x, p.y + r), markerColour, 1.0f);
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
  if (ui.geometry.airfoil.has_value()) {
    // Fit the section itself. frameBox preserves the aspect ratio, so a thin
    // profile ends up limited by its chord and centred vertically - which is
    // how airfoils are conventionally presented.
    const auto [minimum, maximum] = ui.geometry.airfoil->bounds();
    ui.camera.frameBox(minimum, maximum, 0.08);
    return;
  }

  // Nothing generated yet: frame the region a unit-chord section would occupy.
  ui.camera.frameBox(Vec2{-0.35, -0.55}, Vec2{1.35, 0.55}, 0.06);
}

void updateGeometry(UiState& ui) {
  GeometryState& state = ui.geometry;
  if (!state.dirty) {
    return;
  }
  state.dirty = false;

  const geom::AirfoilOptions options{
      .pointsPerSurface = state.pointsPerSurface,
      .chord = state.chord,
      .trailingEdge = state.trailingEdge,
  };

  Result<geom::Airfoil> generated =
      geom::makeNaca4Digit(std::string_view{state.designation.data()}, options);

  if (!generated) {
    // Report the problem but keep the previous shape on screen. Regeneration
    // runs on every keystroke, and "2412" passes through "2", "24" and "241"
    // on the way - blanking the viewport for each would be unusable.
    state.errorMessage = generated.error().message();
    return;
  }

  state.errorMessage.clear();
  const bool isFirst = !state.airfoil.has_value();
  state.airfoil = std::move(generated).value();

  CFD_LOG_INFO(kLogCategory,
               "{}: max thickness {:.4f} c at {:.3f} c, max camber {:.4f} c, {} points",
               state.airfoil->designation().name(),
               state.airfoil->maxThickness() / state.airfoil->chord(),
               state.airfoil->maxThicknessPosition(),
               state.airfoil->maxCamber() / state.airfoil->chord(),
               state.airfoil->contour().size() - 1);

  if (isFirst && ui.viewInitialized) {
    resetView(ui);
  }

  // The grid is built around the section, so a new section invalidates it.
  ui.meshing.dirty = true;
}

std::string_view toString(FieldView view) noexcept {
  switch (view) {
    case FieldView::VelocityMagnitude: return "Velocity magnitude";
    case FieldView::VelocityX:         return "Velocity x";
    case FieldView::VelocityY:         return "Velocity y";
    case FieldView::Pressure:          return "Pressure";
    case FieldView::Divergence:        return "Divergence";
  }
  return "Unknown";
}

bool isSignedField(FieldView view) noexcept {
  switch (view) {
    case FieldView::VelocityX:
    case FieldView::VelocityY:
    case FieldView::Divergence:
      return true;
    case FieldView::VelocityMagnitude:
    case FieldView::Pressure:
      return false;
  }
  return false;
}

void updateFlow(UiState& ui) {
  FlowState& state = ui.flow;
  if (!state.dirty) {
    return;
  }
  state.dirty = false;

  const auto discard = [&state]() {
    state.field.reset();
    state.faces.reset();
    state.divergence.clear();
    state.residuals = flow::ResidualSet{};
    state.history.clear();
    state.clock.reset();
  };

  if (!state.enabled || !ui.meshing.mesh.has_value()) {
    discard();
    state.errorMessage.clear();
    return;
  }

  const mesh::Mesh& grid = *ui.meshing.mesh;
  const double chord = ui.geometry.airfoil.has_value() ? ui.geometry.airfoil->chord() : 1.0;

  Result<flow::FlowField> field =
      flow::FlowField::uniform(grid.cellCount(), state.freestream, chord);
  if (!field) {
    discard();
    state.errorMessage = field.error().message();
    return;
  }

  Result<flow::FaceState> faces =
      flow::evaluateFaces(grid, field.value(), state.conditions, state.freestream);
  if (!faces) {
    discard();
    state.errorMessage = faces.error().message();
    return;
  }

  Result<std::vector<double>> div = flow::divergence(grid, faces.value());
  if (!div) {
    discard();
    state.errorMessage = div.error().message();
    return;
  }

  state.errorMessage.clear();
  state.field = std::move(field).value();
  state.faces = std::move(faces).value();
  state.divergence = std::move(div).value();
  state.residuals = flow::continuityResidual(grid, state.divergence);

  state.counts = FlowState::BoundaryCounts{};
  for (std::size_t f = 0; f < grid.faceCount(); ++f) {
    if (grid.faces()[f].isInterior()) {
      continue;
    }
    switch (state.faces->kind[f]) {
      case flow::BoundaryKind::NoSlipWall: ++state.counts.wall; break;
      case flow::BoundaryKind::Inlet:      ++state.counts.inlet; break;
      case flow::BoundaryKind::Outlet:     ++state.counts.outlet; break;
      case flow::BoundaryKind::FarField:
        if (state.faces->inflow[f] != 0) {
          ++state.counts.farFieldIn;
        } else {
          ++state.counts.farFieldOut;
        }
        break;
      case flow::BoundaryKind::Internal:   ++state.counts.internalCut; break;
    }
  }

  if (state.autoRange) {
    double low = std::numeric_limits<double>::max();
    double high = std::numeric_limits<double>::lowest();
    for (std::size_t c = 0; c < state.field->size(); ++c) {
      const double value = fieldValue(ui, state.view, c);
      low = std::min(low, value);
      high = std::max(high, value);
    }
    if (isSignedField(state.view)) {
      // Centre a signed map on zero so the neutral colour means zero.
      const double extent = std::max(std::abs(low), std::abs(high));
      low = -extent;
      high = extent;
    }
    state.rangeMin = low;
    state.rangeMax = high;
  }

  // The state is an initialisation, not the result of a step, so the clock
  // stays at zero. The residual is recorded at iteration 0 all the same: it is
  // the starting point any convergence history will be measured against.
  state.clock.reset();
  state.history.clear();
  state.history.record(0, state.residuals);

  CFD_LOG_INFO(kLogCategory,
               "flow initialised: U {:.4g} m/s at {:.2f} deg, Re {:.3g}, mu {:.3e} Pa.s, "
               "continuity residual {:.3e}, max |div u| {:.3e} 1/s",
               state.freestream.speed, state.freestream.angleOfAttackDeg,
               state.freestream.reynoldsNumber, state.freestream.dynamicViscosity(chord),
               state.residuals.continuity, flow::maxAbsDivergence(state.divergence));
}

void fitDomain(UiState& ui) {
  if (!ui.meshing.mesh.has_value()) {
    return;
  }
  const std::vector<Vec2>& nodes = ui.meshing.mesh->nodes();
  if (nodes.empty()) {
    return;
  }

  Vec2 minimum = nodes.front();
  Vec2 maximum = nodes.front();
  for (const Vec2& node : nodes) {
    minimum.x = std::min(minimum.x, node.x);
    minimum.y = std::min(minimum.y, node.y);
    maximum.x = std::max(maximum.x, node.x);
    maximum.y = std::max(maximum.y, node.y);
  }
  ui.camera.frameBox(minimum, maximum, 0.04);
}

void updateMesh(UiState& ui) {
  MeshState& state = ui.meshing;
  if (!state.dirty) {
    return;
  }
  state.dirty = false;

  if (!state.enabled || !ui.geometry.airfoil.has_value()) {
    state.mesh.reset();
    state.errorMessage.clear();
    return;
  }

  const auto started = std::chrono::steady_clock::now();
  Result<mesh::Mesh> generated =
      mesh::generateCGrid(*ui.geometry.airfoil, state.options());
  const std::chrono::duration<double, std::milli> elapsed =
      std::chrono::steady_clock::now() - started;

  if (!generated) {
    // Drop the stale grid: unlike the section, a mesh that no longer matches
    // its inputs would be actively misleading to leave on screen.
    state.mesh.reset();
    state.errorMessage = generated.error().message();
    return;
  }

  state.errorMessage.clear();
  state.lastGenerationMs = elapsed.count();
  state.mesh = std::move(generated).value();

  // The flow lives on this mesh, so a new grid invalidates it.
  ui.flow.dirty = true;

  const mesh::MeshQuality& quality = state.mesh->quality();
  CFD_LOG_INFO(kLogCategory,
               "{} C-grid: {} cells, {} faces, {} inverted, aspect {:.0f}, "
               "non-orthogonality {:.1f} deg, {:.1f} ms",
               toString(state.resolution), state.mesh->cellCount(),
               state.mesh->faceCount(), quality.invertedCells, quality.maxAspectRatio,
               quality.maxNonOrthogonalityDeg, state.lastGenerationMs);
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
  const char* hint = "drag: pan    wheel or pinch: zoom";
  const float hintWidth = ImGui::CalcTextSize(hint).x;
  ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - hintWidth);
  ImGui::TextColored(theme::kTextDisabled, "%s", hint);
}

// ---------------------------------------------------------------------------
// Session panel
// ---------------------------------------------------------------------------

void drawGeometryPanel(UiState& ui) {
  GeometryState& state = ui.geometry;

  if (!ImGui::CollapsingHeader("Geometry", ImGuiTreeNodeFlags_DefaultOpen)) {
    return;
  }

  // --- designation ---
  ImGui::TextColored(theme::kTextDim, "Section");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ui.fonts.mono != nullptr) {
    ImGui::PushFont(ui.fonts.mono, 0.0f);
  }
  if (ImGui::InputTextWithHint("##designation", "NACA 2412", state.designation.data(),
                               state.designation.size())) {
    state.dirty = true;
  }
  if (ui.fonts.mono != nullptr) {
    ImGui::PopFont();
  }

  if (!state.errorMessage.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kLevelError);
    ImGui::TextWrapped("%s", state.errorMessage.c_str());
    ImGui::PopStyleColor();
  }

  ImGui::Spacing();

  // --- discretisation ---
  if (beginInfoTable("geometry_inputs", 104.0f)) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(theme::kTextDim, "Points");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::DragInt("##points", &state.pointsPerSurface, 1.0f, 3, 2001, "%d /surface")) {
      state.pointsPerSurface = std::clamp(state.pointsPerSurface, 3, 2001);
      state.dirty = true;
    }

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(theme::kTextDim, "Chord");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::DragScalar("##chord", ImGuiDataType_Double, &state.chord, 0.005f, nullptr,
                          nullptr, "%.4g m")) {
      state.chord = std::clamp(state.chord, 1e-3, 1e4);
      state.dirty = true;
    }

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(theme::kTextDim, "Trailing edge");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    const bool closed = state.trailingEdge == geom::TrailingEdge::Closed;
    if (ImGui::BeginCombo("##te", closed ? "Closed" : "Open (standard)")) {
      if (ImGui::Selectable("Open (standard)", !closed)) {
        state.trailingEdge = geom::TrailingEdge::Open;
        state.dirty = true;
      }
      if (ImGui::Selectable("Closed", closed)) {
        state.trailingEdge = geom::TrailingEdge::Closed;
        state.dirty = true;
      }
      ImGui::EndCombo();
    }
    ImGui::EndTable();
  }

  ImGui::Spacing();
  ImGui::SeparatorText("Display");
  ImGui::Checkbox("Fill", &state.fillSection);
  ImGui::SameLine();
  ImGui::Checkbox("Chord", &state.showChordLine);
  ImGui::Checkbox("Camber", &state.showCamberLine);
  ImGui::SameLine();
  ImGui::Checkbox("Points", &state.showSurfacePoints);

  // --- measured properties ---
  if (!state.airfoil.has_value()) {
    return;
  }
  const geom::Airfoil& foil = *state.airfoil;
  const double c = foil.chord();

  ImGui::Spacing();
  ImGui::SeparatorText("Measured");
  ImGui::TextColored(theme::kTextDisabled, "From the generated points, per chord.");
  ImGui::Spacing();

  if (beginInfoTable("geometry_measured", 104.0f)) {
    infoRow(ui, "Section", foil.designation().name());
    infoRow(ui, "Thickness", std::format("{:.4f} c at {:.3f} c", foil.maxThickness() / c,
                                         foil.maxThicknessPosition()));
    if (foil.designation().isSymmetric()) {
      infoRow(ui, "Camber", "none (symmetric)");
    } else {
      infoRow(ui, "Camber", std::format("{:.4f} c at {:.3f} c", foil.maxCamber() / c,
                                        foil.maxCamberPosition()));
    }
    infoRow(ui, "TE gap", std::format("{:.5f} c", foil.trailingEdgeGap() / c));
    infoRow(ui, "Area", std::format("{:.5f} c²", foil.area() / (c * c)));
    infoRow(ui, "Perimeter", std::format("{:.4f} c", foil.perimeter() / c));
    infoRow(ui, "Points", std::format("{} on contour", foil.contour().size() - 1));
    ImGui::EndTable();
  }
}

void drawMeshPanel(UiState& ui) {
  MeshState& state = ui.meshing;

  if (!ImGui::CollapsingHeader("Mesh", ImGuiTreeNodeFlags_DefaultOpen)) {
    return;
  }

  if (ImGui::Checkbox("Generate mesh", &state.enabled)) {
    state.dirty = true;
  }

  if (!state.enabled) {
    ImGui::TextColored(theme::kTextDisabled, "No computational domain.");
    return;
  }

  // --- resolution ---
  if (beginInfoTable("mesh_inputs", 104.0f)) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(theme::kTextDim, "Resolution");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##resolution",
                          std::string{toString(state.resolution)}.c_str())) {
      for (const mesh::MeshResolution level :
           {mesh::MeshResolution::Coarse, mesh::MeshResolution::Medium,
            mesh::MeshResolution::Fine}) {
        const bool selected = (state.resolution == level);
        if (ImGui::Selectable(std::string{toString(level)}.c_str(), selected)) {
          state.resolution = level;
          // The preset owns the near-wall spacing; adopt it so switching
          // levels actually changes the boundary-layer resolution.
          state.firstLayerHeight = mesh::optionsFor(level).firstLayerHeight;
          state.dirty = true;
        }
        if (selected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }

    const auto extentRow = [&](const char* label, const char* id, double* value,
                               double low, double high) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextColored(theme::kTextDim, "%s", label);
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (ImGui::DragScalar(id, ImGuiDataType_Double, value, 0.1f, nullptr, nullptr,
                            "%.1f c")) {
        *value = std::clamp(*value, low, high);
        state.dirty = true;
      }
    };
    extentRow("Upstream", "##upstream", &state.upstreamChords, 1.0, 60.0);
    extentRow("Downstream", "##downstream", &state.downstreamChords, 1.0, 100.0);
    extentRow("Vertical", "##vertical", &state.verticalChords, 1.0, 60.0);

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(theme::kTextDim, "First layer");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::DragScalar("##firstlayer", ImGuiDataType_Double, &state.firstLayerHeight,
                          1e-5f, nullptr, nullptr, "%.1e c")) {
      state.firstLayerHeight = std::clamp(state.firstLayerHeight, 1e-6, 0.1);
      state.dirty = true;
    }
    ImGui::EndTable();
  }

  ImGui::Spacing();
  ImGui::SeparatorText("Display");
  ImGui::Checkbox("Grid", &state.showInterior);
  ImGui::SameLine();
  ImGui::Checkbox("Boundaries", &state.showBoundaries);
  if (ImGui::Button("Fit domain", ImVec2(-FLT_MIN, 0.0f))) {
    fitDomain(ui);
  }

  if (!state.errorMessage.empty()) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kLevelError);
    ImGui::TextWrapped("%s", state.errorMessage.c_str());
    ImGui::PopStyleColor();
    return;
  }
  if (!state.mesh.has_value()) {
    return;
  }

  // --- statistics ---
  const mesh::Mesh& grid = *state.mesh;
  const mesh::MeshQuality& quality = grid.quality();

  ImGui::Spacing();
  ImGui::SeparatorText("Domain");
  if (beginInfoTable("mesh_stats", 104.0f)) {
    infoRow(ui, "Block", std::format("{} x {} nodes", grid.nodesI(), grid.nodesJ()));
    infoRow(ui, "Cells", std::format("{}", grid.cellCount()));
    infoRow(ui, "Faces", std::format("{}", grid.faceCount()));
    infoRow(ui, "Area", std::format("{:.4g} c²", grid.totalArea()));
    infoRow(ui, "Built in", std::format("{:.1f} ms", state.lastGenerationMs));
    ImGui::EndTable();
  }

  ImGui::Spacing();
  ImGui::SeparatorText("Boundaries");
  if (beginInfoTable("mesh_boundaries", 104.0f)) {
    const auto boundaryRow = [&](const char* label, mesh::BoundaryType type,
                                 const ImVec4& swatch) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextColored(swatch, "%s", label);
      ImGui::TableSetColumnIndex(1);
      if (ui.fonts.mono != nullptr) {
        ImGui::PushFont(ui.fonts.mono, 0.0f);
      }
      ImGui::Text("%zu faces", grid.countFaces(type));
      if (ui.fonts.mono != nullptr) {
        ImGui::PopFont();
      }
    };
    boundaryRow("Wall", mesh::BoundaryType::Wall, theme::kBoundaryWall);
    boundaryRow("Wake cut", mesh::BoundaryType::WakeCut, theme::kBoundaryWakeCut);
    boundaryRow("Far field", mesh::BoundaryType::Farfield, theme::kBoundaryFarfield);
    boundaryRow("Outlet", mesh::BoundaryType::Outlet, theme::kBoundaryOutlet);
    ImGui::EndTable();
  }

  ImGui::Spacing();
  ImGui::SeparatorText("Quality");
  if (beginInfoTable("mesh_quality", 104.0f)) {
    infoRow(ui, "Cell area", std::format("{:.2e} to {:.2e}", quality.minCellArea,
                                         quality.maxCellArea));
    infoRow(ui, "Wall step", std::format("{:.2e} c", quality.minWallSpacing));
    infoRow(ui, "Max aspect", std::format("{:.0f}", quality.maxAspectRatio));
    infoRow(ui, "Max non-orth", std::format("{:.1f} deg", quality.maxNonOrthogonalityDeg));
    ImGui::EndTable();
  }

  // Inverted cells would make the mesh unusable, so they get stated plainly
  // either way rather than only when something is wrong.
  if (quality.invertedCells > 0) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kLevelError);
    ImGui::Text("%zu inverted cells", quality.invertedCells);
    ImGui::PopStyleColor();
  } else {
    ImGui::TextColored(theme::kTextDim, "No inverted cells.");
  }

  if (state.drawStride > 1) {
    ImGui::TextColored(theme::kLevelWarning, "Drawing every %d%s grid line.",
                       state.drawStride, state.drawStride == 2 ? "nd" : "th");
    ImGui::TextColored(theme::kTextDisabled, "Zoom in to see all of them.");
  }
}

void drawFlowPanel(UiState& ui) {
  FlowState& state = ui.flow;

  if (!ImGui::CollapsingHeader("Flow", ImGuiTreeNodeFlags_DefaultOpen)) {
    return;
  }

  if (ImGui::Checkbox("Initialise flow", &state.enabled)) {
    state.dirty = true;
  }
  if (!state.enabled) {
    ImGui::TextColored(theme::kTextDisabled, "No flow state.");
    return;
  }
  if (!ui.meshing.mesh.has_value()) {
    ImGui::TextColored(theme::kLevelWarning, "A mesh is needed first.");
    return;
  }

  // --- freestream ---
  ImGui::Spacing();
  ImGui::SeparatorText("Freestream");
  if (beginInfoTable("flow_inputs", 104.0f)) {
    const auto scalarRow = [&](const char* label, const char* id, double* value, float step,
                               const char* fmt, double low, double high) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextColored(theme::kTextDim, "%s", label);
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (ImGui::DragScalar(id, ImGuiDataType_Double, value, step, nullptr, nullptr, fmt)) {
        *value = std::clamp(*value, low, high);
        state.dirty = true;
      }
    };
    scalarRow("Speed", "##speed", &state.freestream.speed, 0.25f, "%.4g m/s", 1e-3, 1e4);
    scalarRow("Incidence", "##alpha", &state.freestream.angleOfAttackDeg, 0.05f, "%.2f deg",
              -90.0, 90.0);
    scalarRow("Density", "##rho", &state.freestream.density, 0.005f, "%.4g kg/m3", 1e-4, 1e4);
    scalarRow("Reynolds", "##re", &state.freestream.reynoldsNumber, 5000.0f, "%.4g", 1.0,
              1e9);
    scalarRow("Pressure", "##pref", &state.freestream.referencePressure, 1.0f, "%.4g Pa",
              -1e9, 1e9);
    ImGui::EndTable();
  }

  if (!state.errorMessage.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kLevelError);
    ImGui::TextWrapped("%s", state.errorMessage.c_str());
    ImGui::PopStyleColor();
    return;
  }

  const double chord = ui.geometry.airfoil.has_value() ? ui.geometry.airfoil->chord() : 1.0;
  const Vec2 streamVelocity = state.freestream.velocity();

  ImGui::Spacing();
  ImGui::SeparatorText("Derived");
  ImGui::TextColored(theme::kTextDisabled, "Viscosity follows from the Reynolds number.");
  if (beginInfoTable("flow_derived", 104.0f)) {
    infoRow(ui, "Velocity", std::format("({:+.4g}, {:+.4g}) m/s", streamVelocity.x,
                                        streamVelocity.y));
    infoRow(ui, "Dyn. press", std::format("{:.5g} Pa", state.freestream.dynamicPressure()));
    infoRow(ui, "Viscosity", std::format("{:.4e} Pa.s",
                                         state.freestream.dynamicViscosity(chord)));
    infoRow(ui, "Kinematic", std::format("{:.4e} m2/s",
                                         state.freestream.kinematicViscosity(chord)));
    ImGui::EndTable();
  }

  // --- boundary conditions ---
  ImGui::Spacing();
  ImGui::SeparatorText("Boundary conditions");

  const auto kindCombo = [&](const char* label, const char* id, flow::BoundaryKind* kind,
                             std::initializer_list<flow::BoundaryKind> choices) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(theme::kTextDim, "%s", label);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo(id, std::string{toString(*kind)}.c_str())) {
      for (const flow::BoundaryKind option : choices) {
        const bool selected = (*kind == option);
        if (ImGui::Selectable(std::string{toString(option)}.c_str(), selected)) {
          *kind = option;
          state.dirty = true;
        }
        if (selected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }
  };

  if (beginInfoTable("flow_bc", 104.0f)) {
    // The airfoil is not offered as a choice: anything but a wall would let
    // fluid through the surface.
    infoRow(ui, "Airfoil", toString(state.conditions.wall));
    kindCombo("Far field", "##bcfar", &state.conditions.farField,
              {flow::BoundaryKind::FarField, flow::BoundaryKind::Inlet});
    kindCombo("Outlet", "##bcout", &state.conditions.outlet,
              {flow::BoundaryKind::Outlet, flow::BoundaryKind::FarField});
    infoRow(ui, "Wake cut", "Internal (not a boundary)");
    ImGui::EndTable();
  }

  ImGui::Spacing();
  ImGui::TextColored(theme::kTextDim, "Faces by condition");
  if (beginInfoTable("flow_bc_counts", 104.0f)) {
    const auto countRow = [&](const char* label, std::size_t count, const ImVec4& swatch) {
      if (count == 0) {
        return;
      }
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextColored(swatch, "%s", label);
      ImGui::TableSetColumnIndex(1);
      if (ui.fonts.mono != nullptr) {
        ImGui::PushFont(ui.fonts.mono, 0.0f);
      }
      ImGui::Text("%zu", count);
      if (ui.fonts.mono != nullptr) {
        ImGui::PopFont();
      }
    };
    countRow("No-slip wall", state.counts.wall, theme::kBcWall);
    countRow("Inlet", state.counts.inlet, theme::kBcInlet);
    countRow("Outlet", state.counts.outlet, theme::kBcOutlet);
    countRow("Far field in", state.counts.farFieldIn, theme::kBcFarFieldIn);
    countRow("Far field out", state.counts.farFieldOut, theme::kBcFarFieldOut);
    countRow("Wake cut", state.counts.internalCut, theme::kBcInternal);
    ImGui::EndTable();
  }

  // --- state ---
  ImGui::Spacing();
  ImGui::SeparatorText("State");
  if (beginInfoTable("flow_state", 104.0f)) {
    infoRow(ui, "Time", std::format("{:.5g} s", state.clock.time));
    infoRow(ui, "Iteration", std::format("{}", state.clock.iteration));
    infoRow(ui, "Continuity", std::format("{:.4e} m2/s", state.residuals.continuity));
    infoRow(ui, "Max |div u|", std::format("{:.4e} 1/s",
                                           flow::maxAbsDivergence(state.divergence)));
    ImGui::EndTable();
  }

  // Stating this plainly matters more than anything else in the panel. The
  // field is an initial guess that satisfies the far field everywhere and the
  // wall nowhere; the divergence next to the surface is the measure of that,
  // and removing it is exactly what a solver would do.
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Text, theme::kLevelWarning);
  ImGui::TextWrapped("Initialised field only. No equations have been solved, and the "
                     "momentum residuals are zero because there is no momentum equation "
                     "yet.");
  ImGui::PopStyleColor();

  // --- display ---
  ImGui::Spacing();
  ImGui::SeparatorText("Display");
  ImGui::Checkbox("Field", &state.showField);
  ImGui::SameLine();
  ImGui::Checkbox("Conditions", &state.showBoundaryKinds);

  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ImGui::BeginCombo("##fieldview", std::string{toString(state.view)}.c_str())) {
    for (const FieldView option :
         {FieldView::VelocityMagnitude, FieldView::VelocityX, FieldView::VelocityY,
          FieldView::Pressure, FieldView::Divergence}) {
      const bool selected = (state.view == option);
      if (ImGui::Selectable(std::string{toString(option)}.c_str(), selected)) {
        state.view = option;
        state.dirty = true;  // the auto range depends on which field is shown
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }

  if (ImGui::Checkbox("Auto range", &state.autoRange)) {
    state.dirty = true;
  }
  if (!state.autoRange) {
    if (beginInfoTable("flow_range", 104.0f)) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextColored(theme::kTextDim, "Min");
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-FLT_MIN);
      ImGui::DragScalar("##rangemin", ImGuiDataType_Double, &state.rangeMin, 0.01f, nullptr,
                        nullptr, "%.4g");

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextColored(theme::kTextDim, "Max");
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-FLT_MIN);
      ImGui::DragScalar("##rangemax", ImGuiDataType_Double, &state.rangeMax, 0.01f, nullptr,
                        nullptr, "%.4g");
      ImGui::EndTable();
    }
  }
}

void drawSessionPanel(UiState& ui) {
  if (ImGui::CollapsingHeader("Build")) {
    if (beginInfoTable("build_info")) {
      infoRow(ui, "Version", BuildInfo::version());
      infoRow(ui, "Config", BuildInfo::buildType());
      infoRow(ui, "Compiler", BuildInfo::compiler());
      infoRow(ui, "Standard", BuildInfo::cxxStandard());
      ImGui::EndTable();
    }
  }

  if (ImGui::CollapsingHeader("Graphics")) {
    if (beginInfoTable("gl_info")) {
      infoRow(ui, "Renderer", ui.renderer.renderer);
      infoRow(ui, "Vendor", ui.renderer.vendor);
      infoRow(ui, "OpenGL", ui.renderer.glVersion);
      infoRow(ui, "GLSL", ui.renderer.glslVersion);
      infoRow(ui, "GLFW", ui.renderer.windowSystem);
      ImGui::EndTable();
    }
  }

  if (ImGui::CollapsingHeader("Viewport")) {
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

  if (ImGui::CollapsingHeader("Pipeline")) {
    // The intended solver chain, shown so the structure of the project is
    // visible from inside it. Stages carry their real status: no progress
    // bars, no placeholder numbers for anything that does not exist.
    struct Stage {
      const char* name;
      bool implemented;
    };
    static constexpr std::array<Stage, 8> kStages{{
        {"Geometry (NACA)", true},
        {"Mesh generation", true},
        {"Navier-Stokes", false},
        {"RANS averaging", false},
        {"k-omega SST", false},
        {"Force integration", false},
        {"Separation", false},
        {"Vortex / stall", false},
    }};

    if (ImGui::BeginTable("pipeline", 2,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                              ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_NoSavedSettings)) {
      ImGui::TableSetupColumn("stage", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("status", ImGuiTableColumnFlags_WidthFixed, 96.0f);

      for (const Stage& stage : kStages) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(stage.implemented ? theme::kText : theme::kTextDisabled, "%s",
                           stage.name);
        ImGui::TableSetColumnIndex(1);
        if (stage.implemented) {
          ImGui::TextColored(theme::kTextDim, "ready");
        } else {
          ImGui::TextColored(theme::kTextDisabled, "not implemented");
        }
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
  // Deferred until the viewport has a real size, since framing depends on it.
  if (ui.meshing.pendingDomainFit && ui.meshing.mesh.has_value()) {
    fitDomain(ui);
    ui.meshing.pendingDomainFit = false;
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

  if (hovered) {
    const Vec2 anchor{static_cast<double>(io.MousePos.x - origin.x),
                      static_cast<double>(io.MousePos.y - origin.y)};

    if (io.MouseWheel != 0.0f) {
      // Geometric steps, so one notch is the same proportional change at every
      // zoom level - the behaviour that feels linear to a user.
      const double factor = std::pow(1.15, static_cast<double>(io.MouseWheel));
      ui.camera.zoomAboutScreenPoint(factor, anchor);
    }

    if (ui.pinchMagnification != 0.0) {
      // The platform reports a pinch as a relative size change, so the factor
      // it asks for is 1 + magnification. Clamped because a fast gesture can
      // deliver a large accumulated value in a single frame.
      const double factor = std::clamp(1.0 + ui.pinchMagnification, 0.2, 5.0);
      ui.camera.zoomAboutScreenPoint(factor, anchor);
    }
  }
  // Consumed whether or not the cursor was over the canvas, so a pinch that
  // began elsewhere cannot be applied later.
  ui.pinchMagnification = 0.0;

  ui.cursorInViewport = hovered;
  if (hovered) {
    ui.cursorWorld = ui.camera.screenToWorld(
        Vec2{static_cast<double>(io.MousePos.x - origin.x),
             static_cast<double>(io.MousePos.y - origin.y)});
  }

  draw->PushClipRect(origin, far_corner, true);
  draw->AddRectFilled(origin, far_corner, ImGui::GetColorU32(theme::kViewport));

  drawGridAndAxes(draw, ui, origin, size);

  // Painted back to front: shaded cells, then the grid over them, then the
  // boundaries, then the section, which must win every overlap.
  const bool haveFlow = ui.flow.enabled && ui.flow.field.has_value() &&
                        ui.meshing.mesh.has_value();
  if (haveFlow && ui.flow.showField) {
    drawScalarField(draw, ui, origin);
  }

  if (ui.meshing.mesh.has_value()) {
    if (ui.meshing.showInterior) {
      drawMeshLines(draw, ui, origin);
    }
    // Boundary conditions supersede the plain mesh patch colouring once the
    // flow exists, because the condition is the more informative thing.
    if (haveFlow && ui.flow.showBoundaryKinds && ui.flow.faces.has_value()) {
      drawBoundaryKinds(draw, ui, origin);
    } else if (ui.meshing.showBoundaries) {
      drawMeshBoundaries(draw, ui, origin);
    }
  }

  drawAirfoil(draw, ui, origin);

  if (haveFlow) {
    if (ui.flow.showField) {
      drawFieldLegend(draw, ui, origin, size);
    }
    drawFreestreamIndicator(draw, ui, origin, size);
  }

  // Empty-state notice, shown only when there is genuinely nothing to draw.
  if (!ui.geometry.airfoil.has_value()) {
    const char* primary = "No geometry generated";
    const char* secondary = "Enter a NACA four-digit designation in the Geometry panel.";

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
  if (ui.geometry.airfoil.has_value()) {
    ImGui::TextUnformatted(ui.geometry.airfoil->designation().name().c_str());
  } else {
    ImGui::TextColored(theme::kTextDim, "No geometry");
  }
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
