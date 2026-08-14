// Theme.hpp - visual language for the application.
//
// The target is the look of established engineering software (ParaView,
// Fluent, a CAD modeller): dense, quiet, and legible for long sittings.
// Concretely that means square corners, hairline borders that separate
// regions without drawing attention, a narrow desaturated palette, and colour
// reserved for information rather than decoration. The one saturated accent
// is spent on selection and focus only.
//
// Private to the app module; not part of the public include tree.

#pragma once

#include <imgui.h>

#include <string>
#include <vector>

namespace cfd::app::theme {

// ---------------------------------------------------------------------------
// Palette - cool neutral greys with a single steel-blue accent.
// ---------------------------------------------------------------------------
inline const ImVec4 kAppBackground{0.086f, 0.094f, 0.102f, 1.00f};  // outermost
inline const ImVec4 kPanel{0.129f, 0.140f, 0.152f, 1.00f};          // panel body
inline const ImVec4 kPanelHeader{0.169f, 0.184f, 0.200f, 1.00f};    // section bars
inline const ImVec4 kControl{0.098f, 0.106f, 0.116f, 1.00f};        // input fields
inline const ImVec4 kControlHovered{0.157f, 0.173f, 0.188f, 1.00f};
inline const ImVec4 kControlActive{0.204f, 0.224f, 0.243f, 1.00f};
inline const ImVec4 kBorder{0.216f, 0.235f, 0.255f, 1.00f};
inline const ImVec4 kViewport{0.071f, 0.078f, 0.086f, 1.00f};  // darkest: the canvas

inline const ImVec4 kText{0.788f, 0.812f, 0.835f, 1.00f};
inline const ImVec4 kTextDim{0.478f, 0.514f, 0.549f, 1.00f};
inline const ImVec4 kTextDisabled{0.353f, 0.380f, 0.408f, 1.00f};

inline const ImVec4 kAccent{0.267f, 0.475f, 0.639f, 1.00f};  // steel blue
inline const ImVec4 kAccentDim{0.196f, 0.341f, 0.463f, 1.00f};

// Severity colours for the log console. Muted so a wall of warnings does not
// turn the panel into a traffic light.
inline const ImVec4 kLevelTrace{0.435f, 0.463f, 0.490f, 1.00f};
inline const ImVec4 kLevelDebug{0.478f, 0.596f, 0.678f, 1.00f};
inline const ImVec4 kLevelInfo{0.741f, 0.769f, 0.792f, 1.00f};
inline const ImVec4 kLevelWarning{0.792f, 0.647f, 0.400f, 1.00f};
inline const ImVec4 kLevelError{0.796f, 0.451f, 0.427f, 1.00f};
inline const ImVec4 kLevelCritical{0.906f, 0.400f, 0.376f, 1.00f};

// Viewport grid. Deliberately low contrast: the grid is a reference, not
// content, and must never compete with the geometry drawn on top of it.
inline const ImVec4 kGridMinor{0.145f, 0.157f, 0.169f, 1.00f};
inline const ImVec4 kGridMajor{0.204f, 0.220f, 0.235f, 1.00f};
inline const ImVec4 kAxisX{0.396f, 0.478f, 0.400f, 1.00f};  // desaturated green
inline const ImVec4 kAxisY{0.478f, 0.427f, 0.400f, 1.00f};  // desaturated warm

/// Fixed metrics, in unscaled pixels. DPI scaling is applied by ImGui.
inline constexpr float kToolbarHeight = 30.0f;
inline constexpr float kStatusBarHeight = 24.0f;
inline constexpr float kSplitterThickness = 4.0f;
inline constexpr float kMinPanelSize = 140.0f;

/// Fonts resolved at startup. Either may be null, in which case ImGui's
/// built-in bitmap font is used instead.
struct Fonts {
  ImFont* ui{nullptr};    ///< proportional, for labels and prose
  ImFont* mono{nullptr};  ///< fixed-width, for numbers and log output
};

/// Apply colours, spacing and border widths to the active ImGui style.
void apply(ImGuiStyle& style);

/// Load a UI and a monospace face from the platform's font directories.
/// Numeric readouts belong in a fixed-width face so that digits line up
/// column-wise and a changing value does not make the layout twitch.
///
/// `sizePixels` is the unscaled base size; DPI scaling is handled separately
/// through ImGuiStyle::FontScaleDpi.
Fonts loadFonts(ImGuiIO& io, float sizePixels);

}  // namespace cfd::app::theme
