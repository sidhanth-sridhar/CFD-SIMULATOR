#include "Panels.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
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
/// Project the grid lines to the screen and store them in the cache.
///
/// Split out from the drawing so the expensive half - two passes over every
/// node, a visibility test and a transform each - happens only when the view
/// it was computed for has changed.
void buildMeshLines(UiState& ui, ImVec2 origin, const ViewKey& key) {
  GridLineCache& cache = ui.meshing.lines;
  cache.points.clear();
  cache.runs.clear();
  cache.key = key;
  cache.valid = true;

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
  cache.stride = stride;
  ui.meshing.drawStride = stride;

  int runLength = 0;
  const auto flush = [&]() {
    if (runLength >= 2) {
      cache.runs.push_back(runLength);
    } else if (runLength == 1) {
      cache.points.pop_back();
    }
    runLength = 0;
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
      if (runLength == 0) {
        cache.points.push_back(toScreen(a));
        ++runLength;
      }
      cache.points.push_back(toScreen(b));
      ++runLength;
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
      if (runLength == 0) {
        cache.points.push_back(toScreen(a));
        ++runLength;
      }
      cache.points.push_back(toScreen(b));
      ++runLength;
    }
    flush();
  }
}

void drawMeshLines(ImDrawList* draw, UiState& ui, ImVec2 origin, ImVec2 size) {
  ViewKey key;
  key.meshRevision = ui.meshing.revision;
  key.cameraCentre = ui.camera.center();
  key.pixelsPerUnit = ui.camera.pixelsPerUnit();
  key.originX = origin.x;
  key.originY = origin.y;
  key.width = size.x;
  key.height = size.y;

  GridLineCache& cache = ui.meshing.lines;
  if (!cache.valid || !(cache.key == key)) {
    buildMeshLines(ui, origin, key);
  }
  ui.meshing.drawStride = cache.stride;

  const ImU32 colour = ImGui::GetColorU32(theme::kMeshLine);
  std::size_t offset = 0;
  for (const int run : cache.runs) {
    draw->AddPolyline(cache.points.data() + offset, run, colour, 1.0f, 0);
    offset += static_cast<std::size_t>(run);
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
void drawScalarField(ImDrawList* draw, UiState& ui, ImVec2 origin, ImVec2 size) {
  const mesh::Mesh& grid = *ui.meshing.mesh;
  const flow::FlowField& field = *ui.flow.field;
  if (field.size() != grid.cellCount()) {
    return;
  }

  const auto [worldMin, worldMax] = ui.camera.visibleBounds();

  // The offscreen target is sized in framebuffer pixels, not ImGui points: on
  // a Retina display those differ by the backing scale, and using points would
  // produce a texture at half resolution that reads as a blurry field.
  const ImVec2 fbScale = ImGui::GetIO().DisplayFramebufferScale;
  const float scaleX = std::max(fbScale.x, 1.0f);
  const float scaleY = std::max(fbScale.y, 1.0f);
  const int widthPx = static_cast<int>(size.x * scaleX);
  const int heightPx = static_cast<int>(size.y * scaleY);

  FieldKey key;
  key.meshRevision = ui.meshing.revision;
  key.fieldRevision = ui.flow.revision;
  key.view = static_cast<int>(ui.flow.view);
  key.signedMap = isSignedField(ui.flow.view);
  key.rangeMin = ui.flow.rangeMin;
  key.rangeMax = ui.flow.rangeMax;
  key.cameraCentre = ui.camera.center();
  key.pixelsPerUnit = ui.camera.pixelsPerUnit() * static_cast<double>(scaleX);
  key.widthPx = widthPx;
  key.heightPx = heightPx;

  const ImTextureID texture = ui.flow.renderer.texture(key, grid, field, ui.flow.divergence,
                                                       worldMin, worldMax);
  ui.flow.shadedCells = ui.flow.renderer.shadedCells();
  ui.flow.shadingComplete = true;
  ui.flow.shadingRebuilt = ui.flow.renderer.rebuiltLast();

  if (texture == 0) {
    return;
  }

  // OpenGL puts the first row of a texture at the bottom and ImGui puts the
  // origin at the top, so the vertical coordinate is handed over flipped.
  const ImVec2 corner(origin.x + size.x, origin.y + size.y);
  draw->AddImage(texture, origin, corner, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
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

void drawStreamlines(ImDrawList* draw, const UiState& ui, ImVec2 origin) {
  if (!ui.surface.showStreamlines || ui.surface.streamlines.empty()) {
    return;
  }
  const ImU32 colour = ImGui::GetColorU32(theme::kStreamline);

  std::vector<ImVec2> screen;
  for (const post::Streamline& line : ui.surface.streamlines) {
    if (line.size() < 2) {
      continue;
    }
    screen.clear();
    screen.reserve(line.size());
    for (const Vec2& world : line) {
      const Vec2 s = ui.camera.worldToScreen(world);
      screen.push_back(ImVec2(static_cast<float>(static_cast<double>(origin.x) + s.x),
                              static_cast<float>(static_cast<double>(origin.y) + s.y)));
    }
    draw->AddPolyline(screen.data(), static_cast<int>(screen.size()), colour, 1.1f, 0);
  }
}

/// The surface, coloured by the sign and size of the computed wall shear.
///
/// This is the picture the separation number comes from, drawn directly from
/// the same wall shear rather than from anything imposed: green where the fluid
/// next to the wall still runs downstream, red where it has turned back, and
/// brighter where the shear is larger. A separation point is exactly where the
/// colour crosses over, so the marker and the colouring can be checked against
/// one another by eye.
void drawWallShear(ImDrawList* draw, const UiState& ui, ImVec2 origin) {
  const SurfaceState& state = ui.surface;
  if (!state.showWallShear || !state.distribution.has_value()) {
    return;
  }
  const post::SurfaceDistribution& surface = *state.distribution;

  const auto toScreen = [&](const Vec2& world) {
    const Vec2 s = ui.camera.worldToScreen(world);
    return ImVec2(static_cast<float>(static_cast<double>(origin.x) + s.x),
                  static_cast<float>(static_cast<double>(origin.y) + s.y));
  };

  // Scale by the largest magnitude away from the nose, so the colouring stays
  // readable whatever the Reynolds number. The nose is excluded for the same
  // reason as in the Cf plot: skin friction is singular where the boundary
  // layer starts, and scaling to that peak leaves the whole chord washed out.
  double scale = 0.0;
  for (const std::vector<post::SurfacePoint>* side : {&surface.upper, &surface.lower}) {
    for (const post::SurfacePoint& point : *side) {
      if (point.chordFraction >= 0.05) {
        scale = std::max(scale, std::abs(point.skinFriction));
      }
    }
  }
  if (!(scale > 0.0)) {
    scale = std::max(std::abs(surface.minSkinFriction), std::abs(surface.maxSkinFriction));
  }
  const auto colourFor = [&](const post::SurfacePoint& point) {
    const ImVec4& base = point.reversed ? theme::kReversedFlow : theme::kAttachedFlow;
    const double strength =
        (scale > 0.0) ? std::clamp(std::abs(point.skinFriction) / scale, 0.0, 1.0) : 0.0;
    // Never fully transparent: a station with almost no shear is precisely the
    // interesting one, and must not vanish. Magnitude is shown by darkening
    // towards the background rather than by fading out, so that what varies is
    // the strength of the colour and not how much of the bright outline
    // underneath shows through.
    const float t = static_cast<float>(0.35 + 0.65 * std::sqrt(strength));
    const ImVec4& ground = theme::kViewport;
    return ImGui::GetColorU32(ImVec4(ground.x + (base.x - ground.x) * t,
                                     ground.y + (base.y - ground.y) * t,
                                     ground.z + (base.z - ground.z) * t, 1.0f));
  };

  for (const std::vector<post::SurfacePoint>* side : {&surface.upper, &surface.lower}) {
    for (std::size_t i = 0; i + 1 < side->size(); ++i) {
      const post::SurfacePoint& a = (*side)[i];
      const post::SurfacePoint& b = (*side)[i + 1];
      draw->AddLine(toScreen(a.position), toScreen(b.position), colourFor(a), 3.0f);
    }
  }

  if (state.showSeparation) {
    const ImU32 marker = ImGui::GetColorU32(theme::kSeparation);
    for (const post::SeparationPoint* point :
         {&surface.upperSeparation, &surface.lowerSeparation}) {
      if (!point->found) {
        continue;
      }
      const ImVec2 at = toScreen(point->position);
      draw->AddCircle(at, 5.0f, marker, 0, 1.6f);
      draw->AddLine(ImVec2(at.x, at.y - 12.0f), ImVec2(at.x, at.y - 5.0f), marker, 1.4f);
      draw->AddText(ImVec2(at.x + 7.0f, at.y - 16.0f), marker,
                    std::format("sep x/c {:.3f}", point->chordFraction).c_str());
    }
  }

  // Where the oncoming stream divides. Its position is a solved result too, and
  // seeing it move aft with incidence is the clearest confirmation of that.
  const ImVec2 stagnation = toScreen(surface.stagnationPosition);
  const ImU32 stagnationColour = ImGui::GetColorU32(theme::kStagnation);
  draw->AddCircleFilled(stagnation, 2.6f, stagnationColour);
  draw->AddCircle(stagnation, 5.5f, stagnationColour, 0, 1.0f);
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

void refreshFieldRange(UiState& ui) {
  FlowState& state = ui.flow;
  if (!state.autoRange || !state.field.has_value()) {
    return;
  }

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

  if (!state.enabled || ui.meshing.mesh == nullptr) {
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

  // Continue from the field already on screen where that was asked for. The
  // uniform field is still built first, because it is what sets density and
  // viscosity from the new freestream - only the two solved quantities are
  // carried across. A mesh change alters the cell count, so the size check is
  // also what stops a stale field being copied onto a different grid.
  const bool continued = state.warmStart && state.field.has_value() &&
                         state.field->size() == field.value().size();
  if (continued) {
    field.value().velocity = state.field->velocity;
    field.value().pressure = state.field->pressure;
  }
  state.warmStart = false;
  state.continuedFromPrevious = continued;

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
  ++state.revision;
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

  refreshFieldRange(ui);

  // The state is an initialisation, not the result of a step, so the clock
  // stays at zero. The residual is recorded at iteration 0 all the same: it is
  // the starting point any convergence history will be measured against.
  state.clock.reset();
  state.history.clear();
  state.history.record(0, state.residuals);

  // A converged run that is continued into a new freestream picks itself back
  // up. Sweeping incidence is the whole reason the slider exists, and a sweep
  // that leaves an unsolved field on screen after every nudge - until the user
  // remembers to press Run - answers the wrong question. A run the user paused
  // on purpose stays paused.
  const bool resume = continued && ui.solving.converged;

  // The solver is built from this field, so it has to start again, and the
  // surface quantities are read from it, so they have to be taken again.
  ui.solving.dirty = true;
  ui.surface.dirty = true;
  if (!continued) {
    // A cold start is a new run, so the coefficient traces start again with it.
    // A continued one keeps its history: that is the same run carrying on, and
    // seeing the coefficients move to their new values is the point of it.
    ui.surface.liftHistory.clear();
    ui.surface.dragHistory.clear();
  }
  if (resume) {
    ui.solving.running = true;
  }

  CFD_LOG_INFO(kLogCategory,
               "flow {}: U {:.4g} m/s at {:.2f} deg, Re {:.3g}, mu {:.3e} Pa.s, "
               "continuity residual {:.3e}, max |div u| {:.3e} 1/s",
               continued ? "continued from the previous solution" : "initialised",
               state.freestream.speed, state.freestream.angleOfAttackDeg,
               state.freestream.reynoldsNumber, state.freestream.dynamicViscosity(chord),
               state.residuals.continuity, flow::maxAbsDivergence(state.divergence));
}

bool updateSolver(UiState& ui) {
  SolverState& state = ui.solving;

  if (state.dirty) {
    state.dirty = false;
    state.errorMessage.clear();
    state.iteration = 0;
    state.converged = false;
    state.hitIterationLimit = false;
    state.monitor = solver::SolverMonitor{};
    state.continuityHistory.clear();
    state.momentumHistory.clear();

    // Whatever the worker was doing belongs to the previous mesh or field.
    // Stopping here also joins its thread, which is what makes it safe for the
    // grid underneath it to have been replaced.
    // Either the worker was mid-run, or something asked for one before a
    // worker existed - the command line's --solve, or a converged run being
    // resumed by an incidence change. Both mean "keep going once rebuilt".
    const bool wantRunning = state.worker.isRunning() || state.running;
    state.worker.stop();
    state.running = false;

    // The solver needs a mesh to live on and a field to start from, so it can
    // only exist once both stages ahead of it have produced something.
    if (ui.meshing.mesh == nullptr || !ui.flow.field.has_value()) {
      return false;
    }

    Result<flow::FaceConditions> conditions = flow::buildFaceConditions(
        *ui.meshing.mesh, ui.flow.conditions, ui.flow.freestream);
    if (!conditions) {
      state.errorMessage = conditions.error().message();
      return false;
    }

    Result<solver::SimpleSolver> created = solver::SimpleSolver::create(
        *ui.meshing.mesh, std::move(conditions).value(), state.settings);
    if (!created) {
      state.errorMessage = created.error().message();
      return false;
    }
    if (const Status started = created.value().initialise(*ui.flow.field); !started) {
      state.errorMessage = started.error().message();
      return false;
    }

    // The worker holds the grid, not just a pointer into it, so a later
    // regeneration on this thread cannot pull it out from under the solve.
    state.worker.adopt(ui.meshing.mesh, std::move(created).value());
    state.worker.setSettings(state.settings);
    state.worker.setLimits(state.convergenceTolerance, state.maxIterations,
                           state.iterationsPerFrame);
    if (wantRunning) {
      state.worker.setRunning(true);
      state.running = true;
    }
  }

  if (!state.worker.hasSolver()) {
    state.running = false;
    return false;
  }

  state.worker.setSettings(state.settings);
  state.worker.setLimits(state.convergenceTolerance, state.maxIterations,
                         state.iterationsPerFrame);

  // Collect whatever the worker has finished since the last frame. Everything
  // below this point runs on data the worker is no longer touching.
  SolverUpdate update;
  if (state.worker.poll(update)) {
    state.monitor = update.monitor;
    state.iteration = update.iteration;
    state.converged = update.converged;
    state.hitIterationLimit = update.hitIterationLimit;

    state.continuityHistory.insert(state.continuityHistory.end(),
                                   update.continuityHistory.begin(),
                                   update.continuityHistory.end());
    state.momentumHistory.insert(state.momentumHistory.end(),
                                 update.momentumHistory.begin(),
                                 update.momentumHistory.end());

    if (update.diverged) {
      state.errorMessage =
          "the solve diverged; lower the relaxation factors and start again";
    }

    // The viewport reads this field, so publish it.
    ui.flow.field = std::move(update.field);
    ui.flow.divergence = std::move(update.divergence);
    ui.flow.residuals = update.monitor.residuals;
    ++ui.flow.revision;
    // The field has moved, so the colour map has to move with it. Leaving the
    // range from the uniform starting state makes every solved value saturate,
    // which reads as a broken solution rather than a stale legend.
    refreshFieldRange(ui);
    // Likewise the surface quantities, which are read off this field.
    ui.surface.dirty = true;
  }

  // The worker decides when a run is over, so read the answer from it rather
  // than keeping a second copy here that could disagree.
  state.running = state.worker.isRunning();
  return state.running;
}

void updateSurface(UiState& ui) {
  SurfaceState& state = ui.surface;
  if (!state.dirty) {
    return;
  }
  state.dirty = false;

  state.distribution.reset();
  state.forces.reset();
  state.upperX.clear();
  state.upperCp.clear();
  state.upperCf.clear();
  state.lowerX.clear();
  state.lowerCp.clear();
  state.lowerCf.clear();
  state.errorMessage.clear();

  if (ui.meshing.mesh == nullptr || !ui.flow.field.has_value() ||
      !ui.geometry.airfoil.has_value()) {
    return;
  }

  const double chord = ui.geometry.airfoil->chord();
  Result<post::SurfaceDistribution> extracted = post::extractSurface(
      *ui.meshing.mesh, *ui.flow.field, ui.flow.freestream, chord);
  if (!extracted) {
    state.errorMessage = extracted.error().message();
    return;
  }
  state.distribution = std::move(extracted).value();

  const auto fill = [](const std::vector<post::SurfacePoint>& points,
                       std::vector<float>& xs, std::vector<float>& cps,
                       std::vector<float>& cfs) {
    xs.reserve(points.size());
    cps.reserve(points.size());
    cfs.reserve(points.size());
    for (const post::SurfacePoint& point : points) {
      xs.push_back(static_cast<float>(point.chordFraction));
      cps.push_back(static_cast<float>(point.pressureCoefficient));
      cfs.push_back(static_cast<float>(point.skinFriction));
    }
  };
  fill(state.distribution->upper, state.upperX, state.upperCp, state.upperCf);
  fill(state.distribution->lower, state.lowerX, state.lowerCp, state.lowerCf);

  // Forces come straight off the distribution that was just extracted, so the
  // coefficients and the plots above them can never be one step out of step.
  const Vec2 reference{state.momentReferenceFraction * chord, 0.0};
  Result<post::AerodynamicForces> integrated =
      post::integrateForces(*state.distribution, ui.flow.freestream, reference);
  if (integrated) {
    state.forces = std::move(integrated).value();

    // Bounded history: the sparkline only has a couple of hundred pixels, and
    // an unbounded vector on a long run is a leak by another name.
    constexpr std::size_t kMaxHistory = 4000;
    state.liftHistory.push_back(static_cast<float>(state.forces->liftCoefficient));
    state.dragHistory.push_back(static_cast<float>(state.forces->dragCoefficient));
    if (state.liftHistory.size() > kMaxHistory) {
      state.liftHistory.erase(state.liftHistory.begin(),
                              state.liftHistory.begin() +
                                  static_cast<std::ptrdiff_t>(kMaxHistory / 4));
      state.dragHistory.erase(state.dragHistory.begin(),
                              state.dragHistory.begin() +
                                  static_cast<std::ptrdiff_t>(kMaxHistory / 4));
    }
  }

  // Streamlines are the expensive part of this function by a wide margin, so
  // they are refreshed on their own schedule: always when the solve is not
  // advancing, and at a bounded rate while it is. The picture stays live
  // without the tracer being run once per published field.
  constexpr double kStreamlineIntervalMs = 250.0;
  const auto tracedAt = std::chrono::steady_clock::now();
  const double sinceTraceMs =
      state.hasTracedStreamlines
          ? std::chrono::duration<double, std::milli>(tracedAt - state.lastStreamlineTrace)
                .count()
          : kStreamlineIntervalMs;
  const bool retrace = state.showStreamlines &&
                       (!ui.solving.running || !state.hasTracedStreamlines ||
                        sinceTraceMs >= kStreamlineIntervalMs);

  if (!state.showStreamlines) {
    state.streamlines.clear();
    state.hasTracedStreamlines = false;
  } else if (retrace) {
    // Seed a rake upstream of the section, spread over rather more than its
    // thickness so some lines pass close to the surface and some well clear.
    const auto [lower, upper] = ui.geometry.airfoil->bounds();
    const double span = std::max(upper.y - lower.y, 0.05 * chord) * 6.0;
    const double centre = 0.5 * (upper.y + lower.y);

    post::StreamlineOptions options;
    options.referenceSpeed = ui.flow.freestream.speed;
    options.maxSteps = 1500;
    options.seeds.reserve(static_cast<std::size_t>(state.streamlineSeeds));
    for (int i = 0; i < state.streamlineSeeds; ++i) {
      const double t = (state.streamlineSeeds > 1)
                           ? static_cast<double>(i) / (state.streamlineSeeds - 1)
                           : 0.5;
      options.seeds.push_back(Vec2{lower.x - 0.4 * chord, centre + span * (t - 0.5)});
    }

    Result<std::vector<post::Streamline>> traced =
        post::traceStreamlines(*ui.meshing.mesh, *ui.flow.field, options);
    if (traced) {
      state.streamlines = std::move(traced).value();
    }
    state.lastStreamlineTrace = tracedAt;
    state.hasTracedStreamlines = true;
  }

  const post::SurfaceDistribution& surface = *state.distribution;
  const auto report = [&](const char* side, const post::SeparationPoint& point,
                          double& reported) {
    const double now = point.found ? point.chordFraction : -1.0;
    // Only when it appears, disappears, or moves by a noticeable amount.
    if (std::abs(now - reported) < 0.005) {
      return;
    }
    if (point.found) {
      CFD_LOG_INFO(kLogCategory,
                   "{} surface separates at x/c = {:.4f} (wall shear reverses there)", side,
                   point.chordFraction);
    } else if (reported >= 0.0) {
      CFD_LOG_INFO(kLogCategory, "{} surface is attached again", side);
    }
    reported = now;
  };
  report("upper", surface.upperSeparation, state.reportedUpperSeparation);
  report("lower", surface.lowerSeparation, state.reportedLowerSeparation);

  // A finished run states its answer. This is the number the whole pipeline
  // exists to produce, and it should not only live in a panel that may not be
  // on screen.
  const bool settled = ui.solving.converged || ui.solving.hitIterationLimit;
  if (settled && state.forces.has_value() &&
      state.forcesReportedAt != ui.solving.iteration) {
    const post::AerodynamicForces& forces = *state.forces;
    CFD_LOG_INFO(kLogCategory,
                 "forces at {:.2f} deg: Cl {:+.5f}, Cd {:+.5f} (pressure {:+.5f}, "
                 "friction {:+.5f}), Cm {:+.5f} about {:.3f} c, L/D {:.3f}",
                 forces.angleOfAttackDeg, forces.liftCoefficient, forces.dragCoefficient,
                 forces.pressureDragCoefficient, forces.frictionDragCoefficient,
                 forces.momentCoefficient, state.momentReferenceFraction,
                 forces.hasLiftToDrag() ? forces.liftToDrag() : 0.0);
    state.forcesReportedAt = ui.solving.iteration;
  }
}

namespace {

/// Frames a sweep waits for the solver to take up a new angle before giving up.
/// Generous: the request goes through a flow rebuild and a solver rebuild, and
/// a couple of frames is normal. Anything approaching this means it never took.
constexpr int kSweepStartupFrameBudget = 240;

/// Rebuild the plot-ready copies from the recorded points.
void refreshPolarSeries(PolarState& state) {
  state.alphaAxis.clear();
  state.clSeries.clear();
  state.cdSeries.clear();
  state.cmSeries.clear();
  state.ldSeries.clear();

  for (const post::PolarPoint& point : state.polar.points) {
    state.alphaAxis.push_back(static_cast<float>(point.angleOfAttackDeg));
    state.clSeries.push_back(static_cast<float>(point.liftCoefficient));
    state.cdSeries.push_back(static_cast<float>(point.dragCoefficient));
    state.cmSeries.push_back(static_cast<float>(point.momentCoefficient));
    state.ldSeries.push_back(static_cast<float>(point.liftToDrag));
  }
}

/// Point the session at one incidence and ask the solver for it.
///
/// `cold` forces a start from the undisturbed stream even when the sweep is
/// otherwise continuing between points. Used to retry an angle whose continued
/// solve blew up: a diverged field is the worst possible initial guess, and
/// carrying it into the next attempt only spreads the damage.
void requestAngle(UiState& ui, double angleDeg, bool cold = false) {
  ui.flow.freestream.angleOfAttackDeg = angleDeg;
  // Continuation is what makes a sweep take minutes rather than an hour: a
  // degree of incidence is a small perturbation on a converged field.
  ui.flow.warmStart = ui.polar.continueBetweenPoints && !cold;
  ui.flow.dirty = true;
  // updateFlow rebuilds the solver, and the rebuild carries this request
  // through as "keep going once rebuilt".
  ui.solving.running = true;
  ui.solving.converged = false;
  ui.solving.hitIterationLimit = false;
  ui.solving.errorMessage.clear();

  ui.polar.phase = post::SweepPhase::Starting;
  ui.polar.startupFrames = 0;
}

}  // namespace

void startPolarSweep(UiState& ui) {
  PolarState& state = ui.polar;
  state.errorMessage.clear();
  state.savedPath.clear();

  if (ui.meshing.mesh == nullptr || !ui.flow.field.has_value()) {
    state.errorMessage = "a meshed section with an initialised flow is needed first";
    return;
  }

  Result<std::vector<double>> angles =
      post::sweepAngles(state.startDeg, state.endDeg, state.stepDeg);
  if (!angles) {
    state.errorMessage = angles.error().message();
    return;
  }

  state.angles = std::move(angles).value();
  state.index = 0;
  state.running = true;

  state.polar = post::Polar{};
  state.polar.section = ui.geometry.airfoil.has_value()
                            ? ui.geometry.airfoil->designation().name()
                            : std::string{"unknown"};
  state.polar.meshResolution = std::string{toString(ui.meshing.resolution)};
  state.polar.reynoldsNumber = ui.flow.freestream.reynoldsNumber;
  state.polar.machEquivalentSpeed = ui.flow.freestream.speed;
  state.polar.chord = ui.geometry.airfoil.has_value() ? ui.geometry.airfoil->chord() : 1.0;
  state.polar.momentReferenceFraction = ui.surface.momentReferenceFraction;
  state.polar.continuedBetweenPoints = state.continueBetweenPoints;
  refreshPolarSeries(state);

  // Where to put the session back when this is over.
  state.restoreAngleDeg = ui.flow.freestream.angleOfAttackDeg;
  state.hasRestoreAngle = true;

  CFD_LOG_INFO(kLogCategory,
               "polar sweep: {} points from {:.2f} to {:.2f} deg in steps of {:.2f}, {}",
               state.angles.size(), state.startDeg, state.endDeg, state.stepDeg,
               state.continueBetweenPoints ? "continued between points" : "cold at each point");

  state.statusMessage = std::format("solving 1 of {}", state.angles.size());
  requestAngle(ui, state.angles.front());
}

void stopPolarSweep(UiState& ui, std::string_view reason) {
  PolarState& state = ui.polar;
  if (!state.running) {
    return;
  }
  state.running = false;
  state.phase = post::SweepPhase::Idle;
  ui.solving.running = false;
  ui.solving.worker.setRunning(false);

  state.statusMessage =
      std::format("{} - {} of {} points", reason, state.polar.size(), state.angles.size());
  CFD_LOG_INFO(kLogCategory, "polar sweep {}: {} of {} points recorded", reason,
               state.polar.size(), state.angles.size());

  if (state.hasRestoreAngle) {
    ui.flow.freestream.angleOfAttackDeg = state.restoreAngleDeg;
    ui.flow.warmStart = true;
    // Clearing this first matters: a converged run that has its freestream
    // changed picks itself back up, which is right for a nudge of the incidence
    // slider and wrong here. A sweep that has just finished should stop, not
    // quietly start a further solve at the angle it is putting back.
    ui.solving.converged = false;
    ui.flow.dirty = true;
    state.hasRestoreAngle = false;
  }
}

void updatePolar(UiState& ui) {
  PolarState& state = ui.polar;
  if (!state.running) {
    return;
  }

  // The sequencing decision itself lives in cfd_post, where it can be tested
  // without a window. This function only carries it out.
  post::SweepObservation observation;
  observation.solverRunning = ui.solving.running;
  observation.solverFailed = !ui.solving.errorMessage.empty();
  observation.framesWaiting = state.startupFrames;

  const post::SweepAction action =
      post::nextSweepAction(state.phase, observation, kSweepStartupFrameBudget);

  switch (action) {
    case post::SweepAction::Wait:
      if (state.phase == post::SweepPhase::Starting) {
        ++state.startupFrames;
      }
      return;

    case post::SweepAction::BeginSolving:
      state.phase = post::SweepPhase::Solving;
      return;

    case post::SweepAction::AbortFailed:
      state.errorMessage = ui.solving.errorMessage;
      stopPolarSweep(ui, "stopped: the solver could not start");
      return;

    case post::SweepAction::AbortNotStarted:
      state.errorMessage = "the solver did not start for this angle";
      stopPolarSweep(ui, "stopped: the solver never started");
      return;

    case post::SweepAction::RecordPoint:
      break;
  }

  // The solve for this angle has stopped, one way or another.
  //
  // A blow-up gets one retry from the undisturbed stream first. Continuation is
  // what makes a sweep affordable, but it is also what makes one bad point
  // poison the rest: the diverged field would be carried into the next angle as
  // its initial guess. Sweeping a NACA 0012 downwards through the onset of
  // separation is a real case where this happens.
  const bool diverged = !ui.solving.errorMessage.empty();
  if (diverged && !state.retryingCold) {
    state.retryingCold = true;
    CFD_LOG_WARN(kLogCategory,
                 "polar point {} at alpha {:.2f} deg diverged while continuing; "
                 "retrying from the undisturbed stream",
                 state.polar.size() + 1, ui.flow.freestream.angleOfAttackDeg);
    requestAngle(ui, state.angles[state.index], /*cold=*/true);
    return;
  }

  post::PolarPoint point;
  point.angleOfAttackDeg = ui.flow.freestream.angleOfAttackDeg;
  point.converged = ui.solving.converged;
  point.diverged = diverged;
  point.retriedCold = state.retryingCold;
  point.iterations = ui.solving.iteration;
  point.continuityResidual = ui.solving.monitor.residuals.continuity;

  if (ui.surface.forces.has_value()) {
    const post::AerodynamicForces& forces = *ui.surface.forces;
    point.liftCoefficient = forces.liftCoefficient;
    point.dragCoefficient = forces.dragCoefficient;
    point.pressureDragCoefficient = forces.pressureDragCoefficient;
    point.frictionDragCoefficient = forces.frictionDragCoefficient;
    point.momentCoefficient = forces.momentCoefficient;
    point.liftToDrag = forces.hasLiftToDrag() ? forces.liftToDrag() : 0.0;
  }
  if (ui.surface.distribution.has_value()) {
    const post::SurfaceDistribution& surface = *ui.surface.distribution;
    point.upperSeparation =
        surface.upperSeparation.found ? surface.upperSeparation.chordFraction : -1.0;
    point.lowerSeparation =
        surface.lowerSeparation.found ? surface.lowerSeparation.chordFraction : -1.0;
  }

  state.polar.points.push_back(point);
  refreshPolarSeries(state);

  CFD_LOG_INFO(kLogCategory,
               "polar point {} of {}: alpha {:.2f} deg, Cl {:+.5f}, Cd {:+.5f}, "
               "Cm {:+.5f}, L/D {:.3f}{}",
               state.polar.size(), state.angles.size(), point.angleOfAttackDeg,
               point.liftCoefficient, point.dragCoefficient, point.momentCoefficient,
               point.liftToDrag,
               point.converged ? "" : std::format(" ({})", post::pointStatus(point)));

  // A point that had to be retried cold leaves a field that is a fine starting
  // guess for the next angle, so continuation resumes normally from here.
  const bool wasRetry = state.retryingCold;
  state.retryingCold = false;
  ++state.index;
  if (state.index >= state.angles.size()) {
    // Write the file before announcing success, so "saved" always means saved.
    const std::string path{state.csvPath.data()};
    const Status written = post::writeCsv(state.polar, path);
    if (written) {
      state.savedPath = path;
      CFD_LOG_INFO(kLogCategory, "polar written to {}", path);
    } else {
      state.errorMessage = written.error().message();
      CFD_LOG_ERROR(kLogCategory, "could not write the polar: {}",
                    written.error().format());
    }
    stopPolarSweep(ui, state.polar.allConverged() ? "finished"
                                                  : "finished with unconverged points");
    return;
  }

  state.statusMessage =
      std::format("solving {} of {}", state.index + 1, state.angles.size());
  // If even the cold retry diverged, the field left behind is garbage; the next
  // angle starts clean rather than inheriting it.
  requestAngle(ui, state.angles[state.index], /*cold=*/wasRetry && point.diverged);
}

void fitDomain(UiState& ui) {
  if (ui.meshing.mesh == nullptr) {
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
  state.mesh = std::make_shared<const mesh::Mesh>(std::move(generated).value());
  ++state.revision;

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
  if (state.mesh == nullptr) {
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

/// Span of the incidence slider, in degrees.
constexpr double kIncidenceSliderMin = -20.0;
constexpr double kIncidenceSliderMax = 20.0;

/// Range the pitching-moment reference may be placed over, in chords.
constexpr double kMomentReferenceMin = 0.0;
constexpr double kMomentReferenceMax = 1.0;

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
  if (ui.meshing.mesh == nullptr) {
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
        // Only the stream changed, not the domain it flows through, so the
        // field already on screen is a far better starting point than a
        // uniform one.
        state.warmStart = true;
        state.dirty = true;
      }
    };
    scalarRow("Speed", "##speed", &state.freestream.speed, 0.25f, "%.4g m/s", 1e-3, 1e4);

    // Incidence gets a slider rather than a drag box, because it is the one
    // freestream quantity that is *swept*: the whole point of an aerofoil is
    // how it behaves as the angle changes, so the control should invite being
    // moved across a range and show where in that range it currently sits.
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(theme::kTextDim, "Incidence");
    ImGui::TableSetColumnIndex(1);
    {
      // Room for the zero button on the same line.
      const float buttonWidth = ImGui::GetFrameHeight();
      ImGui::SetNextItemWidth(std::max(
          ImGui::GetContentRegionAvail().x - buttonWidth - ImGui::GetStyle().ItemSpacing.x,
          40.0f));
      // Bounded at +/-20 deg: beyond that a steady 2D solution stops meaning
      // anything, and a slider that spans a range the solver cannot answer for
      // is a slider that mostly points at nonsense. Ctrl-click still takes a
      // typed value, which is clamped to +/-90 below.
      if (ImGui::SliderScalar("##alpha", ImGuiDataType_Double,
                              &state.freestream.angleOfAttackDeg, &kIncidenceSliderMin,
                              &kIncidenceSliderMax, "%.2f deg")) {
        state.freestream.angleOfAttackDeg =
            std::clamp(state.freestream.angleOfAttackDeg, -90.0, 90.0);
        state.warmStart = true;
        state.dirty = true;
      }

      // A tick at zero. Without it the control reads as another value box, and
      // there is nothing to tell at a glance which side of level the section
      // is at - which is the first thing you want to know from it.
      const ImVec2 low = ImGui::GetItemRectMin();
      const ImVec2 high = ImGui::GetItemRectMax();
      const float zeroX = low.x + (high.x - low.x) *
                                      static_cast<float>(-kIncidenceSliderMin /
                                                         (kIncidenceSliderMax -
                                                          kIncidenceSliderMin));
      ImGui::GetWindowDrawList()->AddLine(ImVec2(zeroX, high.y - 4.0f),
                                          ImVec2(zeroX, high.y - 1.0f),
                                          ImGui::GetColorU32(theme::kTextDim), 1.0f);
      ImGui::GetWindowDrawList()->AddLine(ImVec2(zeroX, low.y + 1.0f),
                                          ImVec2(zeroX, low.y + 4.0f),
                                          ImGui::GetColorU32(theme::kTextDim), 1.0f);

      ImGui::SameLine();
      if (ImGui::Button("0", ImVec2(buttonWidth, 0.0f)) &&
          state.freestream.angleOfAttackDeg != 0.0) {
        state.freestream.angleOfAttackDeg = 0.0;
        state.warmStart = true;
        state.dirty = true;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Back to zero incidence");
      }
    }

    scalarRow("Density", "##rho", &state.freestream.density, 0.005f, "%.4g kg/m3", 1e-4, 1e4);
    scalarRow("Reynolds", "##re", &state.freestream.reynoldsNumber, 5000.0f, "%.4g", 1.0,
              1e9);
    scalarRow("Pressure", "##pref", &state.freestream.referencePressure, 1.0f, "%.4g Pa",
              -1e9, 1e9);
    ImGui::EndTable();
  }
  ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextDisabled);
  ImGui::TextWrapped("Drag the incidence slider to sweep; ctrl-click to type. A change "
                     "continues from the field already solved rather than starting cold, "
                     "and a converged run picks itself back up.");
  ImGui::PopStyleColor();

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

  // The shading is cached in a texture and redrawn only when something that
  // would change it changes. Saying so matters: a field that is not being
  // redrawn and a field that is being redrawn instantly look identical right
  // up until the cache is wrong about one of them.
  ImGui::Spacing();
  ImGui::TextColored(theme::kTextDisabled, "%zu cells shaded, %s", state.shadedCells,
                     state.shadingRebuilt ? "redrawn this frame" : "cached image reused");
}

void drawSolverPanel(UiState& ui) {
  SolverState& state = ui.solving;

  if (!ImGui::CollapsingHeader("Solve", ImGuiTreeNodeFlags_DefaultOpen)) {
    return;
  }
  if (!ui.flow.enabled || !ui.flow.field.has_value()) {
    ImGui::TextColored(theme::kTextDisabled, "An initialised flow is needed first.");
    return;
  }

  // --- run controls ---
  const float buttonWidth = (ImGui::GetContentRegionAvail().x - 12.0f) / 3.0f;
  // The buttons talk to the worker, not to a local flag: it is the thing that
  // is actually running, and it stops itself on convergence.
  if (ImGui::Button(state.running ? "Pause" : "Run", ImVec2(buttonWidth, 0.0f))) {
    const bool wanted = !state.running;
    if (wanted) {
      state.converged = false;
      state.hitIterationLimit = false;
      state.errorMessage.clear();
    }
    state.worker.setRunning(wanted);
    state.running = wanted;
  }
  ImGui::SameLine();
  if (ImGui::Button("Step", ImVec2(buttonWidth, 0.0f)) && state.worker.hasSolver()) {
    // One iteration, then back to paused. The worker publishes whatever it
    // reaches, so the single step is visible even though it happens off this
    // thread and lands on a later frame.
    state.converged = false;
    state.hitIterationLimit = false;
    state.worker.requestIterations(1);
  }
  ImGui::SameLine();
  if (ImGui::Button("Reset", ImVec2(buttonWidth, 0.0f))) {
    state.worker.setRunning(false);
    state.running = false;
    // Back to the undisturbed stream, not to whatever the field happened to
    // hold. Rebuilding the flow rather than only the solver is what makes that
    // true: a run continued across an incidence change leaves a solved field
    // in place, and resetting onto it would not be a reset.
    ui.flow.warmStart = false;
    ui.flow.dirty = true;
  }

  if (!state.errorMessage.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kLevelError);
    ImGui::TextWrapped("%s", state.errorMessage.c_str());
    ImGui::PopStyleColor();
  }
  if (state.converged) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kBcOutlet);
    ImGui::Text("Converged after %lld iterations.", state.iteration);
    ImGui::PopStyleColor();
  }
  if (state.hitIterationLimit) {
    // Said plainly: a run that stopped on the limit has not converged, and the
    // field on screen is whatever the iteration happened to reach.
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kLevelWarning);
    ImGui::TextWrapped("Stopped at the %lld iteration limit. Not converged - the field "
                       "shown is not a solution.", state.maxIterations);
    ImGui::PopStyleColor();
  }

  // --- progress ---
  ImGui::Spacing();
  ImGui::SeparatorText("Residuals");
  if (beginInfoTable("solver_residuals", 104.0f)) {
    infoRow(ui, "Iteration",
            std::format("{}{}", state.iteration,
                        ui.flow.continuedFromPrevious ? "  (continued)" : ""));
    infoRow(ui, "Continuity", std::format("{:.4e}", state.monitor.residuals.continuity));
    infoRow(ui, "Momentum x", std::format("{:.4e}", state.monitor.residuals.momentumX));
    infoRow(ui, "Momentum y", std::format("{:.4e}", state.monitor.residuals.momentumY));
    infoRow(ui, "Max |div u|", std::format("{:.4e} 1/s", state.monitor.maxDivergence));
    infoRow(ui, "Mass imbal.", std::format("{:.4e}", state.monitor.massImbalance));
    ImGui::EndTable();
  }

  // Convergence history, plotted as log10 so several orders of magnitude are
  // legible at once. A residual that flattens has stalled, not converged, and
  // the shape of this curve is the only way to tell the difference.
  if (state.continuityHistory.size() > 1) {
    ImGui::Spacing();
    const float latest = state.continuityHistory.back();
    ImGui::PlotLines("##continuity", state.continuityHistory.data(),
                     static_cast<int>(state.continuityHistory.size()), 0,
                     std::format("continuity  1e{:.1f}", latest).c_str(), -20.0f, 2.0f,
                     ImVec2(-FLT_MIN, 60.0f));
    ImGui::PlotLines("##momentum", state.momentumHistory.data(),
                     static_cast<int>(state.momentumHistory.size()), 0,
                     std::format("momentum  1e{:.1f}",
                                 state.momentumHistory.back()).c_str(),
                     -20.0f, 2.0f, ImVec2(-FLT_MIN, 60.0f));
  }

  // --- settings ---
  ImGui::Spacing();
  ImGui::SeparatorText("Settings");
  ImGui::TextColored(theme::kTextDisabled,
                     "Changing these restarts from the initialised field.");

  if (beginInfoTable("solver_settings", 104.0f)) {
    const auto relaxRow = [&](const char* label, const char* id, double* value) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextColored(theme::kTextDim, "%s", label);
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (ImGui::DragScalar(id, ImGuiDataType_Double, value, 0.005f, nullptr, nullptr,
                            "%.3f")) {
        *value = std::clamp(*value, 0.01, 1.0);
        state.dirty = true;
      }
    };
    relaxRow("Relax u", "##relaxu", &state.settings.velocityRelaxation);
    relaxRow("Relax p", "##relaxp", &state.settings.pressureRelaxation);

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(theme::kTextDim, "Convection");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##scheme",
                          std::string{toString(state.settings.scheme)}.c_str())) {
      for (const solver::ConvectionScheme option :
           {solver::ConvectionScheme::Upwind, solver::ConvectionScheme::SecondOrderUpwind}) {
        const bool selected = (state.settings.scheme == option);
        if (ImGui::Selectable(std::string{toString(option)}.c_str(), selected)) {
          state.settings.scheme = option;
          state.dirty = true;
        }
      }
      ImGui::EndCombo();
    }

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(theme::kTextDim, "Iters/frame");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::DragInt("##perframe", &state.iterationsPerFrame, 0.2f, 1, 200)) {
      state.iterationsPerFrame = std::clamp(state.iterationsPerFrame, 1, 200);
    }
    ImGui::EndTable();
  }
}

namespace {

/// Vertical range of a pair of series, over the stations at or beyond `fromX`.
///
/// The cut-off exists for the skin friction. Cf is largest right at the nose -
/// it grows like 1/sqrt(x) as the boundary layer starts from nothing - so
/// scaling to the full range squashes the entire rest of the chord into a few
/// pixels and hides the zero crossing, which is the one feature the plot is
/// there to show. Excluding the first few per cent of chord from the *scaling*
/// (never from the data, which is still drawn, and still reported in full in
/// the summary table) puts the interesting part back on screen.
struct PlotRange {
  float low{0.0f};
  float high{1.0f};
};

PlotRange rangeOf(const std::vector<float>& upperX, const std::vector<float>& upperY,
                  const std::vector<float>& lowerX, const std::vector<float>& lowerY,
                  float fromX) {
  float low = std::numeric_limits<float>::max();
  float high = std::numeric_limits<float>::lowest();

  const auto scan = [&](const std::vector<float>& xs, const std::vector<float>& ys) {
    for (std::size_t i = 0; i < ys.size() && i < xs.size(); ++i) {
      if (xs[i] < fromX) {
        continue;
      }
      low = std::min(low, ys[i]);
      high = std::max(high, ys[i]);
    }
  };
  scan(upperX, upperY);
  scan(lowerX, lowerY);

  if (!(high > low)) {
    // Everything was excluded, or the field is flat.
    if (!(low < std::numeric_limits<float>::max())) {
      return PlotRange{-1.0f, 1.0f};
    }
    high = low + std::max(std::abs(low), 1.0f) * 0.1f;
  }
  const float pad = 0.08f * (high - low);
  return PlotRange{low - pad, high + pad};
}

/// A small XY plot with axes, for the surface distributions.
///
/// ImGui's built-in PlotLines draws a bare polyline with no axes and one
/// series. A Cp distribution needs two series on a shared chordwise axis, a
/// marked zero line, and - by long convention - an inverted vertical axis, so
/// that suction points upwards and the plot reads the way every textbook and
/// wind-tunnel report draws it.
///
/// Points outside `range` are clamped to the frame rather than dropped, so a
/// curve that runs off the top is visibly pinned to the edge instead of
/// silently vanishing.
void drawDistributionPlot(const char* id, const char* title, float height,
                          const std::vector<float>& upperX,
                          const std::vector<float>& upperY,
                          const std::vector<float>& lowerX,
                          const std::vector<float>& lowerY, PlotRange range, bool invertY,
                          double upperSeparation, double lowerSeparation,
                          bool showSeparation, float xMin = 0.0f, float xMax = 1.0f) {
  if (upperY.empty() && lowerY.empty()) {
    return;
  }
  const float low = range.low;
  const float high = range.high;

  ImGui::TextColored(theme::kTextDim, "%s", title);

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float width = ImGui::GetContentRegionAvail().x;
  const ImVec2 size(width, height);
  ImGui::InvisibleButton(id, size);

  ImDrawList* draw = ImGui::GetWindowDrawList();
  const ImVec2 corner(origin.x + size.x, origin.y + size.y);
  draw->AddRectFilled(origin, corner, ImGui::GetColorU32(theme::kViewport));

  // Leave room on the left for the value labels.
  constexpr float kLeftGutter = 44.0f;
  constexpr float kBottomGutter = 16.0f;
  const ImVec2 plotMin(origin.x + kLeftGutter, origin.y + 4.0f);
  const ImVec2 plotMax(corner.x - 6.0f, corner.y - kBottomGutter);
  if (plotMax.x <= plotMin.x || plotMax.y <= plotMin.y) {
    return;
  }

  const float xSpan = (xMax > xMin) ? (xMax - xMin) : 1.0f;
  const auto toScreen = [&](float x, float y) {
    const float fx =
        plotMin.x + (plotMax.x - plotMin.x) * std::clamp((x - xMin) / xSpan, 0.0f, 1.0f);
    float t = std::clamp((y - low) / (high - low), 0.0f, 1.0f);
    if (!invertY) {
      t = 1.0f - t;  // screen y grows downwards
    }
    return ImVec2(fx, plotMin.y + (plotMax.y - plotMin.y) * t);
  };

  const ImU32 gridColour = ImGui::GetColorU32(theme::kGridMajor);
  const ImU32 textColour = ImGui::GetColorU32(theme::kTextDisabled);

  // Gridlines on human-readable values rather than on fifths of whatever the
  // range happens to be. A sweep from 2 to 10 degrees labelled 2, 4, 6, 8, 10
  // reads instantly; the same axis labelled 2, 3.6, 5.2, 6.8, 8.4, 10 does not.
  const double tick = niceStep(static_cast<double>(xSpan) / 5.0);
  const double firstTick = std::ceil(static_cast<double>(xMin) / tick) * tick;
  for (int i = 0; i < kMinorPerMajor * 4; ++i) {
    const double value = firstTick + tick * i;
    if (value > static_cast<double>(xMax) + tick * 1e-6) {
      break;
    }
    const ImVec2 top = toScreen(static_cast<float>(value), high);
    draw->AddLine(ImVec2(top.x, plotMin.y), ImVec2(top.x, plotMax.y), gridColour, 1.0f);
    const std::string label = formatWorld(value, tick);
    const float labelWidth = ImGui::CalcTextSize(label.c_str()).x;
    draw->AddText(ImVec2(top.x - labelWidth * 0.5f, plotMax.y + 2.0f), textColour,
                  label.c_str());
  }

  // Zero line, if it is in range - the reference for both quantities.
  if (low < 0.0f && high > 0.0f) {
    const ImVec2 zero = toScreen(0.0f, 0.0f);
    draw->AddLine(ImVec2(plotMin.x, zero.y), ImVec2(plotMax.x, zero.y),
                  ImGui::GetColorU32(theme::kAxisX), 1.0f);
  }

  draw->AddText(ImVec2(origin.x + 4.0f, plotMin.y - 1.0f), textColour,
                std::format("{:.3g}", invertY ? low : high).c_str());
  draw->AddText(ImVec2(origin.x + 4.0f, plotMax.y - ImGui::GetTextLineHeight()), textColour,
                std::format("{:.3g}", invertY ? high : low).c_str());

  const auto plotSeries = [&](const std::vector<float>& xs, const std::vector<float>& ys,
                              const ImVec4& colour) {
    if (xs.size() < 2) {
      return;
    }
    std::vector<ImVec2> screen;
    screen.reserve(xs.size());
    for (std::size_t i = 0; i < xs.size(); ++i) {
      screen.push_back(toScreen(xs[i], ys[i]));
    }
    draw->AddPolyline(screen.data(), static_cast<int>(screen.size()),
                      ImGui::GetColorU32(colour), 1.6f, 0);
  };
  plotSeries(upperX, upperY, theme::kSurfaceUpper);
  plotSeries(lowerX, lowerY, theme::kSurfaceLower);

  // Where the boundary layer detaches.
  if (showSeparation) {
    const ImU32 marker = ImGui::GetColorU32(theme::kSeparation);
    for (const double station : {upperSeparation, lowerSeparation}) {
      if (!(station > static_cast<double>(xMin)) ||
          !(station < static_cast<double>(xMax))) {
        continue;
      }
      const ImVec2 at = toScreen(static_cast<float>(station), high);
      draw->AddLine(ImVec2(at.x, plotMin.y), ImVec2(at.x, plotMax.y), marker, 1.2f);
    }
  }

  draw->AddRect(plotMin, plotMax, ImGui::GetColorU32(theme::kBorder));
}

}  // namespace

void drawSurfacePanel(UiState& ui) {
  SurfaceState& state = ui.surface;

  if (!ImGui::CollapsingHeader("Surface", ImGuiTreeNodeFlags_DefaultOpen)) {
    return;
  }
  if (!state.distribution.has_value()) {
    if (!state.errorMessage.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, theme::kLevelError);
      ImGui::TextWrapped("%s", state.errorMessage.c_str());
      ImGui::PopStyleColor();
    } else {
      ImGui::TextColored(theme::kTextDisabled, "Needs a meshed section with a flow.");
    }
    return;
  }

  const post::SurfaceDistribution& surface = *state.distribution;

  ImGui::Checkbox("Wall shear", &state.showWallShear);
  ImGui::SameLine();
  ImGui::Checkbox("Separation", &state.showSeparation);
  if (ImGui::Checkbox("Streamlines", &state.showStreamlines)) {
    state.dirty = true;
  }
  ImGui::SameLine();
  ImGui::Checkbox("Plots", &state.showSurfacePlots);

  // Ranges of the raw quantities, alongside their coefficients: a coefficient
  // is a ratio, and it is easy to lose track of what it is a ratio of.
  double minPressure = std::numeric_limits<double>::max();
  double maxPressure = std::numeric_limits<double>::lowest();
  double maxSurfaceSpeed = 0.0;
  double maxWallShear = 0.0;
  for (const std::vector<post::SurfacePoint>* side : {&surface.upper, &surface.lower}) {
    for (const post::SurfacePoint& point : *side) {
      minPressure = std::min(minPressure, point.pressure);
      maxPressure = std::max(maxPressure, point.pressure);
      maxSurfaceSpeed = std::max(maxSurfaceSpeed, point.nearWallSpeed);
      maxWallShear = std::max(maxWallShear, std::abs(point.wallShear));
    }
  }

  ImGui::Spacing();
  if (beginInfoTable("surface_summary", 104.0f)) {
    infoRow(ui, "Stations", std::format("{} upper, {} lower", surface.upper.size(),
                                        surface.lower.size()));
    infoRow(ui, "Stagnation", std::format("x/c = {:.4f}", surface.stagnationChordFraction));
    infoRow(ui, "Pressure", std::format("{:+.4g} to {:+.4g} Pa", minPressure, maxPressure));
    infoRow(ui, "Cp range", std::format("{:+.3f} to {:+.3f}", surface.minPressureCoefficient,
                                        surface.maxPressureCoefficient));
    infoRow(ui, "Wall shear", std::format("up to {:.4g} Pa", maxWallShear));
    infoRow(ui, "Cf range", std::format("{:+.4f} to {:+.4f}", surface.minSkinFriction,
                                        surface.maxSkinFriction));
    infoRow(ui, "Near-wall U", std::format("up to {:.4g} m/s", maxSurfaceSpeed));
    ImGui::EndTable();
  }

  // Separation is reported only where the computed wall shear reverses.
  ImGui::Spacing();
  if (surface.hasSeparation()) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kSeparation);
    if (surface.upperSeparation.found) {
      ImGui::Text("Upper surface separates at x/c = %.4f",
                  surface.upperSeparation.chordFraction);
    }
    if (surface.lowerSeparation.found) {
      ImGui::Text("Lower surface separates at x/c = %.4f",
                  surface.lowerSeparation.chordFraction);
    }
    ImGui::PopStyleColor();
  } else {
    ImGui::TextColored(theme::kTextDim, "Attached: the wall shear does not reverse.");
  }
  ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextDisabled);
  ImGui::TextWrapped("Found from the sign of the computed wall shear, not imposed.");
  ImGui::PopStyleColor();

  if (!state.showSurfacePlots) {
    return;
  }

  const double upperSep =
      surface.upperSeparation.found ? surface.upperSeparation.chordFraction : -1.0;
  const double lowerSep =
      surface.lowerSeparation.found ? surface.lowerSeparation.chordFraction : -1.0;

  ImGui::Spacing();
  // Cp is drawn with its axis inverted, the universal convention: suction is
  // up, so the upper surface of a lifting section sits above the lower one.
  // Its whole range is used; unlike Cf it has no singularity to hide.
  drawDistributionPlot("##cp", "Cp - inverted, suction up", 150.0f, state.upperX,
                       state.upperCp, state.lowerX, state.lowerCp,
                       rangeOf(state.upperX, state.upperCp, state.lowerX, state.lowerCp, 0.0f),
                       true, upperSep, lowerSep, state.showSeparation);

  ImGui::Spacing();
  constexpr float kCfScaleFrom = 0.05f;
  drawDistributionPlot(
      "##cf", "Cf - below zero the flow has reversed", 130.0f, state.upperX, state.upperCf,
      state.lowerX, state.lowerCf,
      rangeOf(state.upperX, state.upperCf, state.lowerX, state.lowerCf, kCfScaleFrom), false,
      upperSep, lowerSep, state.showSeparation);
  ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextDisabled);
  ImGui::TextWrapped(
      "Scaled from x/c = %.2f: Cf is singular at the nose, and the full range would "
      "flatten the zero crossing. The table above gives the true extremes.",
      static_cast<double>(kCfScaleFrom));
  ImGui::PopStyleColor();

  ImGui::Spacing();
  ImGui::TextColored(theme::kSurfaceUpper, "upper");
  ImGui::SameLine();
  ImGui::TextColored(theme::kSurfaceLower, "lower");
  ImGui::SameLine();
  ImGui::TextColored(theme::kTextDisabled, " - x/c runs left to right");
}

void drawForcePanel(UiState& ui) {
  SurfaceState& state = ui.surface;

  if (!ImGui::CollapsingHeader("Forces", ImGuiTreeNodeFlags_DefaultOpen)) {
    return;
  }
  if (!state.forces.has_value()) {
    ImGui::TextColored(theme::kTextDisabled,
                       "Needs a surface solution to integrate.");
    return;
  }
  const post::AerodynamicForces& forces = *state.forces;

  // The four headline numbers, in the largest type the panel uses. These are
  // what the whole pipeline exists to produce.
  if (beginInfoTable("force_coefficients", 104.0f)) {
    infoRow(ui, "Cl", std::format("{:+.5f}", forces.liftCoefficient));
    infoRow(ui, "Cd", std::format("{:+.5f}", forces.dragCoefficient));
    infoRow(ui, "Cm", std::format("{:+.5f}", forces.momentCoefficient));
    infoRow(ui, "L/D", forces.hasLiftToDrag()
                          ? std::format("{:+.3f}", forces.liftToDrag())
                          : std::string{"-"});
    ImGui::EndTable();
  }

  ImGui::Spacing();
  ImGui::SeparatorText("Breakdown");
  // Where the drag comes from is the single most diagnostic thing here: form
  // drag climbing away from friction drag is what separation looks like in a
  // number.
  if (beginInfoTable("force_breakdown", 104.0f)) {
    infoRow(ui, "Cd pressure", std::format("{:+.5f}", forces.pressureDragCoefficient));
    infoRow(ui, "Cd friction", std::format("{:+.5f}", forces.frictionDragCoefficient));
    infoRow(ui, "Lift", std::format("{:+.5g} N/m", forces.lift));
    infoRow(ui, "Drag", std::format("{:+.5g} N/m", forces.drag));
    infoRow(ui, "Moment", std::format("{:+.5g} N", forces.pitchingMoment));
    infoRow(ui, "Stations", std::format("{}", forces.stations));
    ImGui::EndTable();
  }

  ImGui::Spacing();
  if (beginInfoTable("force_reference", 104.0f)) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(theme::kTextDim, "Moment at");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderScalar("##momentref", ImGuiDataType_Double,
                            &state.momentReferenceFraction, &kMomentReferenceMin,
                            &kMomentReferenceMax, "%.3f x/c")) {
      state.momentReferenceFraction =
          std::clamp(state.momentReferenceFraction, 0.0, 1.0);
      state.dirty = true;
    }
    infoRow(ui, "Alpha", std::format("{:+.2f} deg", forces.angleOfAttackDeg));
    infoRow(ui, "q", std::format("{:.5g} Pa", forces.dynamicPressure));
    ImGui::EndTable();
  }

  ImGui::PushStyleColor(ImGuiCol_Text, theme::kTextDisabled);
  ImGui::TextWrapped(
      "Integrated around the surface as -p n + tau_w t. Cm is nose-up positive. "
      "Forces are per metre of span, so the reference area is the chord.");
  ImGui::PopStyleColor();

  if (!state.showForcePlots || state.liftHistory.size() < 2) {
    ImGui::Checkbox("Convergence", &state.showForcePlots);
    return;
  }

  // Traces against extraction number rather than iteration, since that is when
  // a coefficient was actually produced. A force still drifting once the
  // residuals have flattened has not converged, whatever the residuals say.
  ImGui::Spacing();
  ImGui::PlotLines("##cl", state.liftHistory.data(),
                   static_cast<int>(state.liftHistory.size()), 0,
                   std::format("Cl  {:+.4f}", state.liftHistory.back()).c_str(),
                   FLT_MAX, FLT_MAX, ImVec2(-FLT_MIN, 52.0f));
  ImGui::PlotLines("##cd", state.dragHistory.data(),
                   static_cast<int>(state.dragHistory.size()), 0,
                   std::format("Cd  {:+.4f}", state.dragHistory.back()).c_str(),
                   FLT_MAX, FLT_MAX, ImVec2(-FLT_MIN, 52.0f));
  ImGui::Checkbox("Convergence", &state.showForcePlots);
}

void drawPolarPanel(UiState& ui) {
  PolarState& state = ui.polar;

  if (!ImGui::CollapsingHeader("Polar", ImGuiTreeNodeFlags_DefaultOpen)) {
    return;
  }

  // --- sweep range ---
  const bool locked = state.running;
  ImGui::BeginDisabled(locked);
  if (beginInfoTable("polar_range", 104.0f)) {
    const auto degreeRow = [&](const char* label, const char* id, double* value) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextColored(theme::kTextDim, "%s", label);
      ImGui::TableSetColumnIndex(1);
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (ImGui::DragScalar(id, ImGuiDataType_Double, value, 0.05f, nullptr, nullptr,
                            "%.2f deg")) {
        *value = std::clamp(*value, -90.0, 90.0);
      }
    };
    degreeRow("Start", "##polarstart", &state.startDeg);
    degreeRow("End", "##polarend", &state.endDeg);

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextColored(theme::kTextDim, "Step");
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::DragScalar("##polarstep", ImGuiDataType_Double, &state.stepDeg, 0.02f,
                          nullptr, nullptr, "%.2f deg")) {
      state.stepDeg = std::clamp(state.stepDeg, 0.05, 45.0);
    }
    ImGui::EndTable();
  }

  ImGui::Checkbox("Continue between points", &state.continueBetweenPoints);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Start each angle from the previous solution instead of the undisturbed\n"
        "stream. Much faster, and the same answer - a converged steady solution\n"
        "does not depend on what it was started from.");
  }
  ImGui::EndDisabled();

  // How many solves the current range implies, before committing to them.
  Result<std::vector<double>> planned =
      post::sweepAngles(state.startDeg, state.endDeg, state.stepDeg);
  if (!planned) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kLevelWarning);
    ImGui::TextWrapped("%s", planned.error().message().c_str());
    ImGui::PopStyleColor();
  } else if (!state.running) {
    ImGui::TextColored(theme::kTextDisabled, "%zu angles, one full solve each.",
                       planned.value().size());
  }

  // --- run controls ---
  ImGui::Spacing();
  const float buttonWidth = (ImGui::GetContentRegionAvail().x - 8.0f) / 2.0f;
  ImGui::BeginDisabled(state.running || !planned);
  if (ImGui::Button("Run sweep", ImVec2(buttonWidth, 0.0f))) {
    startPolarSweep(ui);
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!state.running);
  if (ImGui::Button("Stop", ImVec2(buttonWidth, 0.0f))) {
    stopPolarSweep(ui, "stopped by hand");
  }
  ImGui::EndDisabled();

  if (!state.errorMessage.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::kLevelError);
    ImGui::TextWrapped("%s", state.errorMessage.c_str());
    ImGui::PopStyleColor();
  }

  if (state.running) {
    const float progress = state.angles.empty()
                               ? 0.0f
                               : static_cast<float>(state.polar.size()) /
                                     static_cast<float>(state.angles.size());
    ImGui::ProgressBar(progress, ImVec2(-FLT_MIN, 0.0f), state.statusMessage.c_str());
    ImGui::TextColored(theme::kTextDim, "alpha %.2f deg, iteration %lld",
                       ui.flow.freestream.angleOfAttackDeg, ui.solving.iteration);
  } else if (!state.statusMessage.empty()) {
    ImGui::TextColored(theme::kTextDim, "%s", state.statusMessage.c_str());
  }

  if (state.polar.empty()) {
    return;
  }

  // --- results ---
  ImGui::Spacing();
  ImGui::SeparatorText("Results");
  if (beginInfoTable("polar_summary", 104.0f)) {
    infoRow(ui, "Points", std::format("{}", state.polar.size()));
    const int best = state.polar.bestLiftToDragIndex();
    if (best >= 0) {
      const post::PolarPoint& point = state.polar.points[static_cast<std::size_t>(best)];
      infoRow(ui, "Best L/D", std::format("{:.3f} at {:.2f} deg", point.liftToDrag,
                                          point.angleOfAttackDeg));
    } else {
      infoRow(ui, "Best L/D", "-");
    }
    if (!state.polar.allConverged()) {
      infoRow(ui, "Warning", "some points did not converge");
    }
    ImGui::EndTable();
  }

  // --- CSV ---
  ImGui::Spacing();
  ImGui::TextColored(theme::kTextDim, "CSV");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(-FLT_MIN);
  if (ui.fonts.mono != nullptr) {
    ImGui::PushFont(ui.fonts.mono, 0.0f);
  }
  ImGui::InputText("##polarcsv", state.csvPath.data(), state.csvPath.size());
  if (ui.fonts.mono != nullptr) {
    ImGui::PopFont();
  }
  if (ImGui::Button("Save CSV", ImVec2(-FLT_MIN, 0.0f))) {
    const std::string path{state.csvPath.data()};
    const Status written = post::writeCsv(state.polar, path);
    if (written) {
      state.savedPath = path;
      state.errorMessage.clear();
      CFD_LOG_INFO(kLogCategory, "polar written to {}", path);
    } else {
      state.errorMessage = written.error().message();
    }
  }
  if (!state.savedPath.empty()) {
    ImGui::TextColored(theme::kBcOutlet, "saved to %s", state.savedPath.c_str());
  }

  if (!state.showPolarPlots || state.alphaAxis.size() < 2) {
    ImGui::Checkbox("Curves", &state.showPolarPlots);
    return;
  }

  // --- curves ---
  const float alphaMin = state.alphaAxis.front();
  const float alphaMax = std::max(state.alphaAxis.back(), alphaMin + 1.0f);
  const std::vector<float> none;
  const auto plot = [&](const char* id, const char* title, const std::vector<float>& ys,
                        float height) {
    drawDistributionPlot(id, title, height, state.alphaAxis, ys, none, none,
                         rangeOf(state.alphaAxis, ys, none, none, -1e30f), false, -1.0,
                         -1.0, false, alphaMin, alphaMax);
  };

  ImGui::Spacing();
  plot("##polarcl", "Cl vs alpha (deg)", state.clSeries, 110.0f);
  ImGui::Spacing();
  plot("##polarcd", "Cd vs alpha (deg)", state.cdSeries, 90.0f);
  ImGui::Spacing();
  plot("##polarcm", "Cm vs alpha (deg)", state.cmSeries, 90.0f);
  ImGui::Spacing();
  plot("##polarld", "L/D vs alpha (deg)", state.ldSeries, 90.0f);
  ImGui::Spacing();
  ImGui::Checkbox("Curves", &state.showPolarPlots);
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
  if (ui.meshing.pendingDomainFit && ui.meshing.mesh != nullptr) {
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
                        ui.meshing.mesh != nullptr;
  if (haveFlow && ui.flow.showField) {
    drawScalarField(draw, ui, origin, size);
  }

  if (ui.meshing.mesh != nullptr) {
    if (ui.meshing.showInterior) {
      drawMeshLines(draw, ui, origin, size);
    }
    // Boundary conditions supersede the plain mesh patch colouring once the
    // flow exists, because the condition is the more informative thing.
    if (haveFlow && ui.flow.showBoundaryKinds && ui.flow.faces.has_value()) {
      drawBoundaryKinds(draw, ui, origin);
    } else if (ui.meshing.showBoundaries) {
      drawMeshBoundaries(draw, ui, origin);
    }
  }

  // Streamlines sit under the section: they are traced through the fluid, and
  // a curve appearing to cross the solid body would be a lie about the flow.
  if (haveFlow) {
    drawStreamlines(draw, ui, origin);
  }

  drawAirfoil(draw, ui, origin);

  // Wall shear is painted last of the field graphics, over the outline it
  // belongs to, so the sign of the surface flow is never hidden by the fill.
  if (haveFlow) {
    drawWallShear(draw, ui, origin);
  }

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
