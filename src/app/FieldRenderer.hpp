// FieldRenderer.hpp - the shaded scalar field, drawn once and kept.
//
// The problem
// -----------
// Shading the field means two triangles per cell. On the fine C-grid that is
// 210,820 triangles, and the previous implementation emitted every one of them
// into an ImDrawList on every single frame: a colour-map lookup, four camera
// transforms and six vertices per cell, sixty times a second, to produce
// exactly the same picture each time. Nothing about it changes while the user
// sits still looking at a converged solution, which is precisely when it is
// most likely to be sat and looked at.
//
// The fix
// -------
// Draw the cells once into an offscreen texture and then, for as long as
// nothing that would change the picture has changed, blit that texture as a
// single quad. A frame costs one textured rectangle instead of a fifth of a
// million triangles.
//
// What counts as "changed" is the whole of it: the mesh, the field values, the
// scalar being shown, the colour range, where the camera is, and how big the
// viewport is. All of that is folded into a key, and a mismatch rebuilds. That
// makes panning cost what it cost before - the texture has to be redrawn - but
// leaves a still camera free, and during a solve it rebuilds when a new field
// arrives rather than on every redraw.

#pragma once

#include <cstdint>

#include <imgui.h>

#include "cfd/core/Vec2.hpp"
#include "cfd/flow/FlowField.hpp"
#include "cfd/mesh/Mesh.hpp"

namespace cfd::app {

/// Everything that decides what the shaded field looks like.
///
/// Compared as a whole; if any part differs the texture is redrawn. Defaults
/// are deliberately not a valid state, so the first comparison always misses.
struct FieldKey {
  std::uint64_t meshRevision{0};
  std::uint64_t fieldRevision{0};
  int view{-1};
  bool signedMap{false};
  double rangeMin{0.0};
  double rangeMax{0.0};
  /// Camera, as the two numbers that fully determine the mapping. The scale is
  /// in *framebuffer* pixels, matching the target size below, because the two
  /// differ by the backing scale factor on a Retina display and mixing them
  /// draws the field at half size.
  Vec2 cameraCentre{};
  double pixelsPerUnit{0.0};
  int widthPx{0};
  int heightPx{0};

  [[nodiscard]] bool operator==(const FieldKey&) const = default;
};

/// Renders the shaded field into a texture and hands back its identifier.
///
/// Owns OpenGL objects, so it must be destroyed while a context is still
/// current - which is why the application releases it explicitly rather than
/// leaving it to a destructor that may run too late.
class FieldRenderer {
 public:
  FieldRenderer() = default;
  ~FieldRenderer();

  FieldRenderer(const FieldRenderer&) = delete;
  FieldRenderer& operator=(const FieldRenderer&) = delete;
  FieldRenderer(FieldRenderer&&) = delete;
  FieldRenderer& operator=(FieldRenderer&&) = delete;

  /// Number of cells the last rebuild actually shaded.
  [[nodiscard]] std::size_t shadedCells() const noexcept { return shadedCells_; }
  /// Whether the last call rebuilt rather than reused the texture. Reported in
  /// the panel, because "the field redraws instantly" and "the field is not
  /// being redrawn at all" look identical until something is wrong.
  [[nodiscard]] bool rebuiltLast() const noexcept { return rebuiltLast_; }

  /// Texture covering the viewport for `key`, rebuilding if anything changed.
  /// Returns 0 if the texture could not be produced, in which case the caller
  /// should fall back to drawing the cells directly.
  ImTextureID texture(const FieldKey& key, const mesh::Mesh& grid,
                      const flow::FlowField& field,
                      const std::vector<double>& divergence, Vec2 worldMin, Vec2 worldMax);

  /// Drop every OpenGL object. Safe to call more than once.
  void release() noexcept;

 private:
  [[nodiscard]] bool ensureProgram();
  [[nodiscard]] bool ensureTarget(int width, int height);

  unsigned int program_{0};
  unsigned int vao_{0};
  unsigned int vbo_{0};
  unsigned int framebuffer_{0};
  unsigned int colour_{0};
  int targetWidth_{0};
  int targetHeight_{0};
  /// Set once a shader compile or link has failed, so it is not retried on
  /// every frame for the rest of the session.
  bool programFailed_{false};

  FieldKey key_{};
  bool hasKey_{false};
  std::size_t shadedCells_{0};
  bool rebuiltLast_{false};
};

}  // namespace cfd::app
