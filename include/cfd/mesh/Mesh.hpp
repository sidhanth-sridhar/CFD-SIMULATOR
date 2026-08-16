// Mesh.hpp - a face-based two-dimensional finite-volume mesh.
//
// Why face-based
// --------------
// The grid this project generates is structured, so every quantity here could
// be recomputed on demand from (i, j) indices. It is stored explicitly as a
// list of faces anyway, because that is the form a finite-volume solver
// actually consumes.
//
// A finite-volume method works by integrating conservation laws over each
// cell. The divergence theorem turns the volume integral of a flux divergence
// into a sum over the cell's boundary:
//
//     d/dt integral_V (u) dV  +  sum_faces ( F . n ) A  =  0
//
// so the solver never asks "what are my neighbours in i and j"; it asks "for
// each face, what is the flux, which cell owns it, and which cell is on the
// other side". Every face therefore carries an owner, a neighbour, an outward
// normal and an area. Writing the solver against faces rather than against
// index arithmetic also means an unstructured mesh can be substituted later
// without touching it.
//
// In two dimensions the usual three-dimensional vocabulary collapses by one
// order: a "volume" is an area (m^2), and a face "area" is a length (m). The
// names are kept because the equations and the literature use them.

#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

#include "cfd/core/Error.hpp"
#include "cfd/core/Vec2.hpp"

namespace cfd::mesh {

/// What a face is, physically. Interior faces separate two cells; every other
/// kind has exactly one adjacent cell and needs a boundary condition once
/// there is a solver.
enum class BoundaryType {
  Interior,  ///< Between two cells.
  Wall,      ///< The airfoil surface: no-slip once there is a flow.
  Farfield,  ///< Outer boundary, far from the body: freestream conditions.
  Outlet,    ///< Downstream plane where the wake leaves the domain.
  WakeCut,   ///< The slit behind the trailing edge. See the note below.
};

[[nodiscard]] std::string_view toString(BoundaryType type) noexcept;

/// A face between two cells, or between a cell and the outside world.
///
/// The normal always points *out of the owner*. For an interior face that
/// means it points from owner to neighbour; for a boundary face it points out
/// of the domain. Fixing that convention once removes a sign question from
/// every flux the solver will ever assemble.
struct Face {
  std::array<int, 2> nodes{-1, -1};
  int owner{-1};                                ///< always a valid cell
  int neighbour{-1};                            ///< -1 for a boundary face
  BoundaryType boundary{BoundaryType::Interior};

  /// The face this one coincides with across the wake cut, or -1.
  ///
  /// A C-grid is cut open along the wake so the grid can be a topological
  /// rectangle. The two sides of that cut lie on top of each other in space
  /// but are separate faces in the mesh. They are not really a boundary at
  /// all: fluid crosses freely, so a solver treats a matched pair as an
  /// interior connection. Storing the partner keeps that recoverable.
  int partner{-1};

  [[nodiscard]] bool isInterior() const noexcept {
    return boundary == BoundaryType::Interior;
  }
};

/// Aggregate measures of how usable the mesh is. All computed from the
/// generated cells, not assumed.
struct MeshQuality {
  double minCellArea{0.0};
  double maxCellArea{0.0};
  /// Largest ratio of a cell's longer to shorter side. Boundary-layer cells
  /// are deliberately extreme here - thin normal to the wall, long along it.
  double maxAspectRatio{0.0};
  /// Worst angle, in degrees, between a face normal and the line joining the
  /// two cell centroids it separates. Zero is perfectly orthogonal; large
  /// values degrade the accuracy of a gradient reconstructed across that face.
  double maxNonOrthogonalityDeg{0.0};
  /// Cells with non-positive area, i.e. folded over. Must be zero.
  std::size_t invertedCells{0};
  /// Smallest wall-normal first-cell height, in metres.
  double minWallSpacing{0.0};
};

/// Input to buildStructured(): a rectangular block of nodes plus the boundary
/// condition to attach to each of its four sides.
///
/// Nodes are row-major, `nodes[j * nodesI + i]`.
struct StructuredMeshSpec {
  int nodesI{0};
  int nodesJ{0};
  std::vector<Vec2> nodes;

  /// Boundary type for each face along j = 0, one per face (nodesI - 1 of
  /// them). Per-face rather than uniform because the inner boundary of a
  /// C-grid is part wall and part wake cut.
  std::vector<BoundaryType> jMinBoundary;

  BoundaryType jMaxBoundary{BoundaryType::Farfield};
  BoundaryType iMinBoundary{BoundaryType::Outlet};
  BoundaryType iMaxBoundary{BoundaryType::Outlet};

  /// For each j = 0 face, the index of the j = 0 face it coincides with
  /// across the wake cut, or -1. Empty means no cut.
  std::vector<int> jMinPartner;
};

/// A generated mesh: geometry, connectivity and the derived metrics a
/// finite-volume solver needs.
class Mesh {
 public:
  // --- geometry and connectivity ---
  [[nodiscard]] const std::vector<Vec2>& nodes() const noexcept { return nodes_; }
  /// Four node indices per cell, ordered counter-clockwise.
  [[nodiscard]] const std::vector<std::array<int, 4>>& cellNodes() const noexcept {
    return cellNodes_;
  }
  /// Four face indices per cell.
  [[nodiscard]] const std::vector<std::array<int, 4>>& cellFaces() const noexcept {
    return cellFaces_;
  }
  [[nodiscard]] const std::vector<Face>& faces() const noexcept { return faces_; }

  // --- metrics ---
  /// Exact polygon centroid, not the average of the corners. For a skewed
  /// cell those differ, and the finite-volume method wants the centroid.
  [[nodiscard]] const std::vector<Vec2>& cellCentroids() const noexcept {
    return cellCentroids_;
  }
  /// Cell areas in m^2 - the two-dimensional "volume".
  [[nodiscard]] const std::vector<double>& cellAreas() const noexcept { return cellAreas_; }
  [[nodiscard]] const std::vector<Vec2>& faceCentres() const noexcept { return faceCentres_; }
  /// Unit normals, pointing out of the owner cell.
  [[nodiscard]] const std::vector<Vec2>& faceNormals() const noexcept { return faceNormals_; }
  /// Face lengths in m - the two-dimensional "area".
  [[nodiscard]] const std::vector<double>& faceAreas() const noexcept { return faceAreas_; }

  // --- counts ---
  [[nodiscard]] std::size_t nodeCount() const noexcept { return nodes_.size(); }
  [[nodiscard]] std::size_t cellCount() const noexcept { return cellNodes_.size(); }
  [[nodiscard]] std::size_t faceCount() const noexcept { return faces_.size(); }
  [[nodiscard]] std::size_t countFaces(BoundaryType type) const;

  /// Total area of the fluid domain, in m^2.
  [[nodiscard]] double totalArea() const noexcept { return totalArea_; }
  [[nodiscard]] const MeshQuality& quality() const noexcept { return quality_; }

  // --- structured indexing ---
  // This generator produces structured grids, and the renderer and the tests
  // both benefit from being able to walk them by index. A future unstructured
  // mesh would simply report zero here.
  [[nodiscard]] int nodesI() const noexcept { return nodesI_; }
  [[nodiscard]] int nodesJ() const noexcept { return nodesJ_; }
  [[nodiscard]] bool isStructured() const noexcept { return nodesI_ > 0 && nodesJ_ > 0; }

  [[nodiscard]] int nodeIndex(int i, int j) const noexcept { return j * nodesI_ + i; }
  [[nodiscard]] int cellIndex(int i, int j) const noexcept { return j * (nodesI_ - 1) + i; }

 private:
  friend Result<Mesh> buildStructured(StructuredMeshSpec spec);
  Mesh() = default;

  std::vector<Vec2> nodes_;
  std::vector<std::array<int, 4>> cellNodes_;
  std::vector<std::array<int, 4>> cellFaces_;
  std::vector<Face> faces_;

  std::vector<Vec2> cellCentroids_;
  std::vector<double> cellAreas_;
  std::vector<Vec2> faceCentres_;
  std::vector<Vec2> faceNormals_;
  std::vector<double> faceAreas_;

  double totalArea_{0.0};
  MeshQuality quality_{};

  int nodesI_{0};
  int nodesJ_{0};
};

/// The cell on the far side of a face, or -1 if there is none.
///
/// For an ordinary interior face that is simply the neighbour. For a wake cut
/// it is the owner of the partner face: the two coincide in space, so they
/// behave as one interior connection even though the mesh stores them as two
/// boundary faces. Anything that traverses the mesh - flux assembly, a matrix
/// product, a streamline walk - has to agree about this, which is why it lives
/// with the mesh rather than in any one consumer.
[[nodiscard]] int oppositeCell(const Mesh& mesh, std::size_t face) noexcept;

/// Turn a block of structured nodes into a mesh: enumerate cells and faces,
/// assign owners, neighbours and boundary types, and compute every metric.
///
/// Fails on degenerate dimensions or a node count that does not match, but
/// does *not* reject a folded grid - inverted cells are reported through
/// MeshQuality so that a caller (or a test) can see how bad it is rather than
/// only that it failed.
[[nodiscard]] Result<Mesh> buildStructured(StructuredMeshSpec spec);

}  // namespace cfd::mesh
