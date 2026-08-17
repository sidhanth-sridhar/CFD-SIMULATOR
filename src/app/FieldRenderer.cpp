#include "FieldRenderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <vector>

#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif

#if defined(__APPLE__)
#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION
#endif
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

#include "Theme.hpp"
#include "cfd/core/Log.hpp"

namespace cfd::app {
namespace {

constexpr std::string_view kLogCategory = "app";

/// Positions arrive already in clip space, so the vertex stage only has to
/// pass things along. Doing the transform on the CPU keeps the shader trivial
/// and costs nothing: the vertices are rebuilt only when the camera moves, at
/// which point they have to be touched anyway.
constexpr const char* kVertexShader = R"(#version 150 core
in vec2 aPosition;
in vec4 aColour;
out vec4 vColour;
void main() {
  vColour = aColour;
  gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";

constexpr const char* kFragmentShader = R"(#version 150 core
in vec4 vColour;
out vec4 fragColour;
void main() {
  fragColour = vColour;
}
)";

struct Vertex {
  float x{0.0f};
  float y{0.0f};
  std::uint32_t rgba{0};
};

unsigned int compile(GLenum type, const char* source, const char* what) {
  const unsigned int shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);

  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (ok == GL_FALSE) {
    std::array<char, 512> log{};
    glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
    CFD_LOG_ERROR(kLogCategory, "{} shader failed to compile: {}", what, log.data());
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

}  // namespace

FieldRenderer::~FieldRenderer() { release(); }

void FieldRenderer::release() noexcept {
  if (vbo_ != 0) {
    glDeleteBuffers(1, &vbo_);
    vbo_ = 0;
  }
  if (vao_ != 0) {
    glDeleteVertexArrays(1, &vao_);
    vao_ = 0;
  }
  if (colour_ != 0) {
    glDeleteTextures(1, &colour_);
    colour_ = 0;
  }
  if (framebuffer_ != 0) {
    glDeleteFramebuffers(1, &framebuffer_);
    framebuffer_ = 0;
  }
  if (program_ != 0) {
    glDeleteProgram(program_);
    program_ = 0;
  }
  targetWidth_ = 0;
  targetHeight_ = 0;
  hasKey_ = false;
}

bool FieldRenderer::ensureProgram() {
  if (program_ != 0) {
    return true;
  }
  if (programFailed_) {
    return false;
  }

  const unsigned int vertex = compile(GL_VERTEX_SHADER, kVertexShader, "field vertex");
  if (vertex == 0) {
    programFailed_ = true;
    return false;
  }
  const unsigned int fragment =
      compile(GL_FRAGMENT_SHADER, kFragmentShader, "field fragment");
  if (fragment == 0) {
    glDeleteShader(vertex);
    programFailed_ = true;
    return false;
  }

  const unsigned int program = glCreateProgram();
  glAttachShader(program, vertex);
  glAttachShader(program, fragment);
  glBindAttribLocation(program, 0, "aPosition");
  glBindAttribLocation(program, 1, "aColour");
  glLinkProgram(program);
  glDeleteShader(vertex);
  glDeleteShader(fragment);

  GLint ok = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (ok == GL_FALSE) {
    std::array<char, 512> log{};
    glGetProgramInfoLog(program, static_cast<GLsizei>(log.size()), nullptr, log.data());
    CFD_LOG_ERROR(kLogCategory, "field shader failed to link: {}", log.data());
    glDeleteProgram(program);
    programFailed_ = true;
    return false;
  }
  program_ = program;

  glGenVertexArrays(1, &vao_);
  glGenBuffers(1, &vbo_);
  glBindVertexArray(vao_);
  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                        reinterpret_cast<const void*>(offsetof(Vertex, x)));
  glEnableVertexAttribArray(1);
  // Normalised bytes: the colour maps already produce packed RGBA, so this
  // avoids unpacking a quarter of a million colours into floats.
  glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex),
                        reinterpret_cast<const void*>(offsetof(Vertex, rgba)));
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  return true;
}

bool FieldRenderer::ensureTarget(int width, int height) {
  if (framebuffer_ != 0 && width == targetWidth_ && height == targetHeight_) {
    return true;
  }

  if (framebuffer_ == 0) {
    glGenFramebuffers(1, &framebuffer_);
  }
  if (colour_ == 0) {
    glGenTextures(1, &colour_);
  }

  glBindTexture(GL_TEXTURE_2D, colour_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colour_, 0);
  const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);

  if (status != GL_FRAMEBUFFER_COMPLETE) {
    CFD_LOG_ERROR(kLogCategory, "field framebuffer incomplete (0x{:x})",
                  static_cast<unsigned>(status));
    return false;
  }

  targetWidth_ = width;
  targetHeight_ = height;
  return true;
}

ImTextureID FieldRenderer::texture(const FieldKey& key, const mesh::Mesh& grid,
                                   const flow::FlowField& field,
                                   const std::vector<double>& divergence, Vec2 worldMin,
                                   Vec2 worldMax) {
  rebuiltLast_ = false;
  if (key.widthPx <= 0 || key.heightPx <= 0 || field.size() != grid.cellCount()) {
    return 0;
  }
  if (!ensureProgram() || !ensureTarget(key.widthPx, key.heightPx)) {
    return 0;
  }
  if (hasKey_ && key_ == key) {
    return static_cast<ImTextureID>(colour_);
  }

  // --- build the vertices -------------------------------------------------
  //
  // Straight to clip space: x from world through the camera to the viewport,
  // then to [-1, 1]. The vertical flip is folded in here, so the texture ends
  // up with its first row at the bottom the way OpenGL expects, and the caller
  // flips the UVs back when it draws the quad.
  double low = key.rangeMin;
  double high = key.rangeMax;
  if (high <= low) {
    const double pad = std::max(std::abs(high), 1.0) * 1e-6;
    low -= pad;
    high += pad;
  }
  const double span = high - low;

  const double scale = key.pixelsPerUnit;
  const double halfWidthWorld = 0.5 * static_cast<double>(key.widthPx) / scale;
  const double halfHeightWorld = 0.5 * static_cast<double>(key.heightPx) / scale;
  const auto toClip = [&](const Vec2& world) {
    return Vertex{static_cast<float>((world.x - key.cameraCentre.x) / halfWidthWorld),
                  static_cast<float>((world.y - key.cameraCentre.y) / halfHeightWorld), 0};
  };

  const auto value = [&](std::size_t c) -> double {
    switch (key.view) {
      case 0: return length(field.velocity[c]);
      case 1: return field.velocity[c].x;
      case 2: return field.velocity[c].y;
      case 3: return field.pressure[c];
      case 4: return (c < divergence.size()) ? divergence[c] : 0.0;
      default: return 0.0;
    }
  };

  std::vector<Vertex> vertices;
  vertices.reserve(grid.cellCount() * 6);
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

    const double t = (value(c) - low) / span;
    const ImU32 colour =
        key.signedMap ? theme::divergingColour(t) : theme::sequentialColour(t);

    Vertex a = toClip(p0);
    Vertex b = toClip(p1);
    Vertex d = toClip(p2);
    Vertex e = toClip(p3);
    a.rgba = colour;
    b.rgba = colour;
    d.rgba = colour;
    e.rgba = colour;

    vertices.push_back(a);
    vertices.push_back(b);
    vertices.push_back(d);
    vertices.push_back(a);
    vertices.push_back(d);
    vertices.push_back(e);
    ++shaded;
  }
  shadedCells_ = shaded;

  // --- draw into the texture ----------------------------------------------
  GLint previousFramebuffer = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);

  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
  glViewport(0, 0, key.widthPx, key.heightPx);
  // Transparent, not black: the grid and axes are drawn underneath, and the
  // parts of the viewport no cell covers have to keep showing them.
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  if (!vertices.empty()) {
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);

    glUseProgram(program_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)), vertices.data(),
                 GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glUseProgram(0);
    // ImGui's backend expects blending on and sets the rest of its state
    // itself; leaving this off would drop every subsequent transparency.
    glEnable(GL_BLEND);
  }

  glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));

  key_ = key;
  hasKey_ = true;
  rebuiltLast_ = true;
  return static_cast<ImTextureID>(colour_);
}

}  // namespace cfd::app
