#include "cfd/app/Application.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

// ImGui and its backends must come before GLFW: the backend header sets up
// the GLFW inclusion the way it expects it.
#include <imgui.h>

#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

// GLFW includes an OpenGL header of its own choosing unless told not to - on
// macOS that is the legacy <OpenGL/gl.h>. We include <OpenGL/gl3.h> below for
// the core profile, and gl.h together with gl3.h is a diagnosed conflict, so
// GLFW must be kept out of the decision. Mirrored by GLFW_INCLUDE_NONE in
// src/app/CMakeLists.txt.
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <GLFW/glfw3.h>

// Only glViewport / glClear / glGetString / glReadPixels are called directly;
// everything else goes through ImGui's OpenGL backend, which loads its own
// function pointers. Those are all core entry points, so no loader is needed.
#if defined(__APPLE__)
// Normally supplied by the build (see src/app/CMakeLists.txt); defined here as
// well so this file still compiles if it is ever built standalone.
#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION
#endif
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

#include "Panels.hpp"
#include "PlatformGestures.hpp"
#include "Theme.hpp"
#include "cfd/core/BuildInfo.hpp"
#include "cfd/core/Log.hpp"

namespace cfd::app {
namespace {

constexpr std::string_view kLogCategory = "app";

/// Base font size in unscaled pixels. Small on purpose: an engineering tool
/// shows many labelled values at once, and oversized type is the single
/// fastest way to make a technical UI look unserious.
constexpr float kBaseFontSize = 15.0f;

/// GLFW reports errors through a global callback rather than return codes.
void onGlfwError(int code, const char* description) {
  CFD_LOG_ERROR(kLogCategory, "GLFW error {}: {}", code,
                description != nullptr ? description : "(no description)");
}

std::string glStringOrUnknown(GLenum name) {
  const GLubyte* value = glGetString(name);
  return value != nullptr ? std::string(reinterpret_cast<const char*>(value))
                          : std::string("unknown");
}

/// Write 24-bit BGR pixels as an uncompressed BMP.
///
/// BMP because it needs no encoder library and, unusually among image formats,
/// stores rows bottom-to-top - exactly the order glReadPixels returns them, so
/// no vertical flip is required.
Status writeBmp(const std::string& path, int width, int height,
                const std::vector<unsigned char>& rgb) {
  if (width <= 0 || height <= 0) {
    return Error{ErrorCode::InvalidArgument, "screenshot has zero area"};
  }

  // Each BMP row is padded to a multiple of four bytes.
  const int rowBytes = width * 3;
  const int padding = (4 - (rowBytes % 4)) % 4;
  const std::uint32_t imageBytes =
      static_cast<std::uint32_t>((rowBytes + padding) * height);
  const std::uint32_t fileBytes = 54u + imageBytes;

  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return Error{ErrorCode::IoFailure, "cannot open '" + path + "' for writing"};
  }

  const auto put16 = [&out](std::uint16_t v) {
    const unsigned char bytes[2]{static_cast<unsigned char>(v & 0xFFu),
                                 static_cast<unsigned char>((v >> 8) & 0xFFu)};
    out.write(reinterpret_cast<const char*>(bytes), 2);
  };
  const auto put32 = [&out](std::uint32_t v) {
    const unsigned char bytes[4]{static_cast<unsigned char>(v & 0xFFu),
                                 static_cast<unsigned char>((v >> 8) & 0xFFu),
                                 static_cast<unsigned char>((v >> 16) & 0xFFu),
                                 static_cast<unsigned char>((v >> 24) & 0xFFu)};
    out.write(reinterpret_cast<const char*>(bytes), 4);
  };

  out.write("BM", 2);                       // magic
  put32(fileBytes);
  put32(0);                                 // reserved
  put32(54);                                // offset to pixel data
  put32(40);                                // DIB header size (BITMAPINFOHEADER)
  put32(static_cast<std::uint32_t>(width));
  put32(static_cast<std::uint32_t>(height));
  put16(1);                                 // colour planes
  put16(24);                                // bits per pixel
  put32(0);                                 // no compression
  put32(imageBytes);
  put32(2835);                              // ~72 DPI, horizontal
  put32(2835);                              // ~72 DPI, vertical
  put32(0);                                 // palette colours used
  put32(0);                                 // "important" colours

  const std::array<char, 3> pad{0, 0, 0};
  for (int y = 0; y < height; ++y) {
    const std::size_t rowStart = static_cast<std::size_t>(y) * static_cast<std::size_t>(rowBytes);
    for (int x = 0; x < width; ++x) {
      const std::size_t i = rowStart + static_cast<std::size_t>(x) * 3u;
      // BMP stores BGR, OpenGL gave us RGB.
      const char pixel[3]{static_cast<char>(rgb[i + 2]), static_cast<char>(rgb[i + 1]),
                          static_cast<char>(rgb[i])};
      out.write(pixel, 3);
    }
    if (padding > 0) {
      out.write(pad.data(), padding);
    }
  }

  if (!out) {
    return Error{ErrorCode::IoFailure, "failed while writing '" + path + "'"};
  }
  return Status::ok();
}

/// Read the current framebuffer back into host memory and save it.
Status captureFramebuffer(GLFWwindow* window, const std::string& path) {
  int width = 0;
  int height = 0;
  glfwGetFramebufferSize(window, &width, &height);
  if (width <= 0 || height <= 0) {
    return Error{ErrorCode::Internal, "framebuffer has zero size"};
  }

  std::vector<unsigned char> pixels(
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u);

  // Default row alignment is 4 bytes; our buffer is tightly packed.
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

  return writeBmp(path, width, height, pixels);
}

ImGuiWindowFlags fixedPanelFlags() {
  // These windows are positioned by our layout code every frame, so all of
  // ImGui's own window management is switched off. NoBringToFrontOnFocus keeps
  // clicking a panel from raising it above its neighbours, which in a tiled
  // layout only ever causes flicker.
  return ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
         ImGuiWindowFlags_NoNavFocus;
}

/// Draggable divider between two tiled regions.
///
/// It lives in its own borderless window sitting exactly on the seam. `sign`
/// is +1 when dragging towards increasing screen coordinates should grow the
/// tracked panel, and -1 when it should shrink it - which is the case for the
/// console, since it is anchored to the bottom.
void drawSplitter(const char* id, ImVec2 pos, ImVec2 size, bool vertical, float* value,
                  float minValue, float maxValue, float sign) {
  if (size.x <= 0.0f || size.y <= 0.0f) {
    return;
  }

  ImGuiIO& io = ImGui::GetIO();

  ImGui::SetNextWindowPos(pos);
  ImGui::SetNextWindowSize(size);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  // Without this, ImGui clamps the window up to style.WindowMinSize (32 px)
  // and the splitter would be far wider than the seam it sits on.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, 1.0f));

  if (ImGui::Begin(id, nullptr, fixedPanelFlags() | ImGuiWindowFlags_NoBackground)) {
    ImGui::InvisibleButton("handle", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    if (hovered || active) {
      ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW
                                     : ImGuiMouseCursor_ResizeNS);
    }
    if (active) {
      const float delta = vertical ? io.MouseDelta.x : io.MouseDelta.y;
      *value = std::clamp(*value + sign * delta, minValue, maxValue);
    }

    // The seam is normally just the border colour; it brightens only while
    // the user is actually interacting with it.
    const ImVec4& colour = (active || hovered) ? theme::kAccent : theme::kBorder;
    ImGui::GetWindowDrawList()->AddRectFilled(
        pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(colour));
  }
  ImGui::End();
  ImGui::PopStyleVar(3);
}

/// Begin a panel window pinned to an exact rectangle.
bool beginPanel(const char* name, ImVec2 pos, ImVec2 size,
                ImGuiWindowFlags extraFlags = 0) {
  ImGui::SetNextWindowPos(pos);
  ImGui::SetNextWindowSize(size);
  return ImGui::Begin(name, nullptr, fixedPanelFlags() | extraFlags);
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct Application::Impl {
  GLFWwindow* window{nullptr};
  bool imguiPlatformReady{false};
  /// Set each frame by the solver: true while it still has work to do.
  bool solverWantsRedraw{false};
  bool imguiRendererReady{false};
  bool contextCreated{false};
  UiState ui;

  std::string screenshotPath;
  int screenshotAfterFrames{3};
  int frameStatsEvery{0};
  long long maxFrames{0};

  void drawFrame();
  void layoutAndDrawPanels();
};

void Application::Impl::layoutAndDrawPanels() {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();

  // WorkPos/WorkSize already exclude the main menu bar drawn above.
  float regionX = viewport->WorkPos.x;
  float regionY = viewport->WorkPos.y;
  float regionW = viewport->WorkSize.x;
  float regionH = viewport->WorkSize.y;

  // --- toolbar strip along the top ---
  const float toolbarHeight = theme::kToolbarHeight;
  if (beginPanel("##toolbar", ImVec2(regionX, regionY), ImVec2(regionW, toolbarHeight),
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
    drawToolbar(ui);
  }
  ImGui::End();
  regionY += toolbarHeight;
  regionH -= toolbarHeight;

  // --- status bar along the bottom ---
  const float statusHeight = theme::kStatusBarHeight;
  regionH -= statusHeight;
  if (beginPanel("##statusbar", ImVec2(regionX, regionY + regionH),
                 ImVec2(regionW, statusHeight),
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
    drawStatusBar(ui);
  }
  ImGui::End();

  regionH = std::max(regionH, 1.0f);

  // --- left column: session panel ---
  const float splitter = theme::kSplitterThickness;
  float leftWidth = 0.0f;
  if (ui.showLeftPanel) {
    ui.leftPanelWidth =
        std::clamp(ui.leftPanelWidth, theme::kMinPanelSize, std::max(regionW * 0.6f, theme::kMinPanelSize));
    leftWidth = ui.leftPanelWidth;

    if (beginPanel("##session", ImVec2(regionX, regionY), ImVec2(leftWidth, regionH))) {
      drawGeometryPanel(ui);
      drawMeshPanel(ui);
      drawFlowPanel(ui);
      drawSolverPanel(ui);
      drawSurfacePanel(ui);
      drawSessionPanel(ui);
    }
    ImGui::End();

    drawSplitter("##splitter_left", ImVec2(regionX + leftWidth, regionY),
                 ImVec2(splitter, regionH), /*vertical=*/true, &ui.leftPanelWidth,
                 theme::kMinPanelSize, std::max(regionW * 0.6f, theme::kMinPanelSize),
                 +1.0f);
    leftWidth += splitter;
  }

  const float rightX = regionX + leftWidth;
  const float rightW = std::max(regionW - leftWidth, 1.0f);

  // --- right column: viewport above, console below ---
  float consoleHeight = 0.0f;
  if (ui.showConsole) {
    ui.consoleHeight =
        std::clamp(ui.consoleHeight, theme::kMinPanelSize, std::max(regionH * 0.7f, theme::kMinPanelSize));
    consoleHeight = ui.consoleHeight + splitter;
  }

  const float viewportHeight = std::max(regionH - consoleHeight, 1.0f);

  // The canvas runs edge to edge inside its border; padding would leave a
  // stripe of panel colour that reads as a misaligned frame.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  if (beginPanel("##viewport", ImVec2(rightX, regionY), ImVec2(rightW, viewportHeight),
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
    drawViewport(ui);
  }
  ImGui::End();
  ImGui::PopStyleVar();

  if (ui.showConsole) {
    drawSplitter("##splitter_console", ImVec2(rightX, regionY + viewportHeight),
                 ImVec2(rightW, splitter), /*vertical=*/false, &ui.consoleHeight,
                 theme::kMinPanelSize, std::max(regionH * 0.7f, theme::kMinPanelSize),
                 // Dragging down must shrink a bottom-anchored panel.
                 -1.0f);

    if (beginPanel("##console", ImVec2(rightX, regionY + viewportHeight + splitter),
                   ImVec2(rightW, ui.consoleHeight))) {
      drawConsole(ui);
    }
    ImGui::End();
  }
}

void Application::Impl::drawFrame() {
  // Regenerate before anything is drawn, so the viewport and the measured
  // readout in the panel can never disagree within a frame.
  updateGeometry(ui);
  // Strictly ordered: the grid is built around the current section, and the
  // flow is sized by the grid.
  updateMesh(ui);
  updateFlow(ui);
  // Returns true while the solve is running, which keeps the loop redrawing
  // instead of idling on events.
  solverWantsRedraw = updateSolver(ui);
  // Last: the surface distributions and streamlines are read off whatever
  // field the previous steps left behind.
  updateSurface(ui);

  // Drain any trackpad pinch once per frame; the viewport applies and clears it.
  ui.pinchMagnification += platform::consumePinchMagnification();

  if (ImGui::BeginMainMenuBar()) {
    drawMenuBar(ui);
    ImGui::EndMainMenuBar();
  }

  layoutAndDrawPanels();
  drawAboutModal(ui);

  // Keyboard shortcut, suppressed while a text field has focus.
  ImGuiIO& io = ImGui::GetIO();
  if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F, false)) {
    resetView(ui);
  }

  if (ui.quitRequested) {
    glfwSetWindowShouldClose(window, GLFW_TRUE);
  }
}

// ---------------------------------------------------------------------------
// Application
// ---------------------------------------------------------------------------

Application::Application() : impl_(std::make_unique<Impl>()) {}

Application::~Application() { shutdown(); }

Application::Application(Application&&) noexcept = default;
Application& Application::operator=(Application&&) noexcept = default;

bool Application::isInitialized() const noexcept {
  return impl_ != nullptr && impl_->window != nullptr;
}

Status Application::initialize(const ApplicationOptions& options) {
  if (isInitialized()) {
    return Error{ErrorCode::Internal, "Application::initialize called twice"};
  }

  impl_->ui.logBuffer = options.logBuffer;
  if (!options.initialSection.empty()) {
    auto& buffer = impl_->ui.geometry.designation;
    const std::size_t copied = std::min(options.initialSection.size(), buffer.size() - 1);
    std::copy_n(options.initialSection.begin(), copied, buffer.begin());
    buffer[copied] = '\0';
    impl_->ui.geometry.dirty = true;
  }
  if (!options.initialMeshResolution.empty()) {
    std::string level = options.initialMeshResolution;
    std::transform(level.begin(), level.end(), level.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (level == "coarse") {
      impl_->ui.meshing.resolution = mesh::MeshResolution::Coarse;
    } else if (level == "fine") {
      impl_->ui.meshing.resolution = mesh::MeshResolution::Fine;
    } else {
      impl_->ui.meshing.resolution = mesh::MeshResolution::Medium;
    }
    impl_->ui.meshing.firstLayerHeight =
        mesh::optionsFor(impl_->ui.meshing.resolution).firstLayerHeight;
    impl_->ui.meshing.enabled = true;
    impl_->ui.meshing.dirty = true;
    // A mesh asked for on the command line is the thing to look at, so open
    // on the whole domain rather than on the section.
    impl_->ui.meshing.pendingDomainFit = true;
  }

  if (options.initialiseFlow) {
    // A flow needs somewhere to live, so bring the mesh up with it if the
    // caller did not ask for one explicitly.
    if (!impl_->ui.meshing.enabled) {
      impl_->ui.meshing.enabled = true;
      impl_->ui.meshing.dirty = true;
    }
    impl_->ui.flow.enabled = true;
    impl_->ui.flow.dirty = true;
    // Frame the section, not the domain: the interesting part of a flow is
    // what happens at the wall, and at domain scale a chord is a few pixels.
    // Only --mesh on its own means "show me the domain".
    impl_->ui.meshing.pendingDomainFit = false;
  }

  if (options.reynoldsNumber > 0.0) {
    impl_->ui.flow.freestream.reynoldsNumber = options.reynoldsNumber;
    impl_->ui.flow.dirty = true;
  }

  if (options.angleGiven) {
    impl_->ui.flow.freestream.angleOfAttackDeg = options.angleOfAttackDeg;
    impl_->ui.flow.dirty = true;
  }

  if (options.startSolver) {
    if (!impl_->ui.meshing.enabled) {
      impl_->ui.meshing.enabled = true;
      impl_->ui.meshing.dirty = true;
    }
    impl_->ui.flow.enabled = true;
    impl_->ui.flow.dirty = true;
    impl_->ui.solving.running = true;
    impl_->ui.solving.dirty = true;
    impl_->ui.meshing.pendingDomainFit = false;
    // A solved run is worth seeing the surface results of.
    impl_->ui.surface.showStreamlines = true;
  }

  if (!options.initialFieldView.empty()) {
    std::string view = options.initialFieldView;
    std::transform(view.begin(), view.end(), view.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (view == "vx") {
      impl_->ui.flow.view = FieldView::VelocityX;
    } else if (view == "vy") {
      impl_->ui.flow.view = FieldView::VelocityY;
    } else if (view == "pressure") {
      impl_->ui.flow.view = FieldView::Pressure;
    } else if (view == "divergence") {
      impl_->ui.flow.view = FieldView::Divergence;
    } else {
      impl_->ui.flow.view = FieldView::VelocityMagnitude;
    }
    impl_->ui.flow.dirty = true;
  }

  impl_->screenshotPath = options.screenshotPath;
  impl_->frameStatsEvery = std::max(options.frameStatsEvery, 0);
  impl_->maxFrames = std::max(options.maxFrames, 0LL);
  impl_->screenshotAfterFrames = std::max(options.screenshotAfterFrames, 1);

  glfwSetErrorCallback(&onGlfwError);
  if (glfwInit() != GLFW_TRUE) {
    return Error{ErrorCode::InitializationFailure,
                 "glfwInit failed (no display or unsupported platform)"};
  }

  // OpenGL 3.2 core is the highest version macOS offers, and forward-compatible
  // core profile is mandatory there. It is also the floor for ImGui's GLSL 150
  // shader path, so this one request works on every target.
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
  glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);

  const std::string title =
      options.windowTitle.empty()
          ? std::string{BuildInfo::projectName()} + " " + std::string{BuildInfo::version()}
          : options.windowTitle;

  impl_->window = glfwCreateWindow(std::max(options.windowWidth, 640),
                                   std::max(options.windowHeight, 480), title.c_str(),
                                   nullptr, nullptr);
  if (impl_->window == nullptr) {
    glfwTerminate();
    return Error{ErrorCode::InitializationFailure,
                 "could not create an OpenGL 3.2 core profile window"};
  }

  glfwMakeContextCurrent(impl_->window);
  glfwSwapInterval(1);  // vsync: cap redraws at the display refresh rate

  // Trackpad pinch, which GLFW does not report. No-op where unsupported.
  const bool pinchAvailable = platform::installGestureHandlers(impl_->window);
  CFD_LOG_DEBUG(kLogCategory, "trackpad pinch zoom {}",
                pinchAvailable ? "enabled" : "unavailable on this platform");

  impl_->ui.renderer.vendor = glStringOrUnknown(GL_VENDOR);
  impl_->ui.renderer.renderer = glStringOrUnknown(GL_RENDERER);
  impl_->ui.renderer.glVersion = glStringOrUnknown(GL_VERSION);
  impl_->ui.renderer.glslVersion = glStringOrUnknown(GL_SHADING_LANGUAGE_VERSION);
  impl_->ui.renderer.windowSystem = glfwGetVersionString();

  CFD_LOG_INFO(kLogCategory, "OpenGL {} on {}", impl_->ui.renderer.glVersion,
               impl_->ui.renderer.renderer);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  impl_->contextCreated = true;

  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  // The layout is defined in code, so there is nothing worth persisting; this
  // also stops an imgui.ini file appearing in the working directory.
  io.IniFilename = nullptr;

  impl_->ui.fonts = theme::loadFonts(io, kBaseFontSize);
  theme::apply(ImGui::GetStyle());

  if (!ImGui_ImplGlfw_InitForOpenGL(impl_->window, true)) {
    shutdown();
    return Error{ErrorCode::InitializationFailure, "ImGui GLFW backend failed to start"};
  }
  impl_->imguiPlatformReady = true;

  if (!ImGui_ImplOpenGL3_Init("#version 150")) {
    shutdown();
    return Error{ErrorCode::InitializationFailure, "ImGui OpenGL backend failed to start"};
  }
  impl_->imguiRendererReady = true;

  // Returns 1.0 on macOS, where Retina scaling is handled by the framebuffer
  // scale instead; elsewhere it is the real per-monitor DPI factor.
  ImGui::GetStyle().FontScaleDpi = ImGui_ImplGlfw_GetContentScaleForWindow(impl_->window);

  CFD_LOG_INFO(kLogCategory, "window {}x{}, Dear ImGui {}", options.windowWidth,
               options.windowHeight, IMGUI_VERSION);
  return Status::ok();
}

int Application::run() {
  if (!isInitialized()) {
    CFD_LOG_CRITICAL(kLogCategory, "run() called before a successful initialize()");
    return 1;
  }

  CFD_LOG_INFO(kLogCategory, "entering main loop");

  const bool capturing = !impl_->screenshotPath.empty();
  int exitCode = 0;
  long long frameIndex = 0;

  // Frame-time accumulation for the periodic summary.
  float statsTotalMs = 0.0f;
  float statsWorstMs = 0.0f;
  long long statsFrames = 0;

  // Ceiling on redraws while a solve is running. The solver has a thread of its
  // own now, so redrawing faster than this buys nothing at all: it cannot make
  // the answer arrive sooner, it cannot show more than the display refreshes,
  // and every frame spent on it is a core taken away from the solve. Vsync
  // would normally impose a similar limit, but it is not imposed when the
  // window is hidden or occluded - which is exactly when spinning is most
  // wasteful and least visible.
  constexpr double kSolveFrameSeconds = 1.0 / 60.0;
  // Measured from the start of the previous frame, so the interval covers the
  // frame's own cost rather than being added on top of it.
  auto lastFrameStart = std::chrono::steady_clock::now();

  while (glfwWindowShouldClose(impl_->window) == 0) {
    if (capturing || impl_->solverWantsRedraw) {
      // Never block waiting for input: a solve needs the next frame with or
      // without it. Waiting out the remainder of the frame interval still
      // returns early the moment input arrives, so this costs no
      // responsiveness - it only stops the loop free-running.
      const double sinceLastFrame =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - lastFrameStart)
              .count();
      const double remaining = kSolveFrameSeconds - sinceLastFrame;
      if (remaining > 0.0) {
        glfwWaitEventsTimeout(remaining);
      } else {
        glfwPollEvents();
      }
    } else {
      // Blocks until something happens, so an idle window costs no CPU. The
      // timeout bounds the wait so anything time-based still updates.
      glfwWaitEventsTimeout(0.25);
    }

    const auto frameStart = std::chrono::steady_clock::now();
    lastFrameStart = frameStart;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    impl_->drawFrame();

    ImGui::Render();

    // Framebuffer pixels, not window points: on a Retina display these differ
    // by the backing scale factor, and using the wrong one renders to a
    // quarter of the window.
    int framebufferWidth = 0;
    int framebufferHeight = 0;
    glfwGetFramebufferSize(impl_->window, &framebufferWidth, &framebufferHeight);
    glViewport(0, 0, framebufferWidth, framebufferHeight);

    const ImVec4& clear = theme::kAppBackground;
    glClearColor(clear.x, clear.y, clear.z, clear.w);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // Measured before the buffer swap on purpose: with vsync enabled
    // glfwSwapBuffers blocks until the next refresh, so including it would
    // report the display's refresh interval rather than our own cost.
    const std::chrono::duration<float, std::milli> elapsed =
        std::chrono::steady_clock::now() - frameStart;
    impl_->ui.lastFrameCpuMs = elapsed.count();

    if (impl_->frameStatsEvery > 0) {
      statsTotalMs += impl_->ui.lastFrameCpuMs;
      statsWorstMs = std::max(statsWorstMs, impl_->ui.lastFrameCpuMs);
      ++statsFrames;
      if (statsFrames >= impl_->frameStatsEvery) {
        const float mean = statsTotalMs / static_cast<float>(statsFrames);
        CFD_LOG_INFO(kLogCategory,
                     "frames {}: {:.2f} ms mean, {:.2f} ms worst ({:.0f} fps mean)",
                     frameIndex + 1, mean, statsWorstMs,
                     (mean > 0.0f) ? 1000.0f / mean : 0.0f);
        statsTotalMs = 0.0f;
        statsWorstMs = 0.0f;
        statsFrames = 0;
      }
    }

    ++frameIndex;
    if (impl_->maxFrames > 0 && frameIndex >= impl_->maxFrames) {
      glfwSetWindowShouldClose(impl_->window, GLFW_TRUE);
    }
    // When a solve is running, let it finish before capturing: a screenshot of
    // the third iteration of a SIMPLE run shows nothing worth looking at.
    constexpr long long kMaxSolveFrames = 20000;
    const bool waitingOnSolver =
        impl_->solverWantsRedraw && frameIndex < kMaxSolveFrames;
    if (capturing && !waitingOnSolver && frameIndex >= impl_->screenshotAfterFrames) {
      // Read back before the swap, while the rendered image is still in the
      // back buffer we just drew into.
      if (const Status status = captureFramebuffer(impl_->window, impl_->screenshotPath);
          status) {
        CFD_LOG_INFO(kLogCategory, "wrote screenshot to {}", impl_->screenshotPath);
      } else {
        CFD_LOG_ERROR(kLogCategory, "screenshot failed: {}", status.error().format());
        exitCode = 1;
      }
      glfwSetWindowShouldClose(impl_->window, GLFW_TRUE);
    }

    glfwSwapBuffers(impl_->window);
  }

  CFD_LOG_INFO(kLogCategory, "main loop finished");
  shutdown();
  return exitCode;
}

void Application::shutdown() noexcept {
  if (impl_ == nullptr) {
    return;
  }

  // Strict reverse order of construction. Tearing down the ImGui context
  // before its backends would leave the backends pointing at freed state.
  if (impl_->imguiRendererReady) {
    ImGui_ImplOpenGL3_Shutdown();
    impl_->imguiRendererReady = false;
  }
  if (impl_->imguiPlatformReady) {
    ImGui_ImplGlfw_Shutdown();
    impl_->imguiPlatformReady = false;
  }
  if (impl_->contextCreated) {
    ImGui::DestroyContext();
    impl_->contextCreated = false;
  }
  if (impl_->window != nullptr) {
    glfwDestroyWindow(impl_->window);
    impl_->window = nullptr;
    glfwTerminate();
  }
}

}  // namespace cfd::app
