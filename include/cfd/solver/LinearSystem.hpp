// LinearSystem.hpp - the sparse matrix a finite-volume discretisation produces.
//
// Every equation here has the same shape: a cell's value, coupled to its face
// neighbours and nothing else.
//
//     a_P * x_P  +  sum_neighbours ( a_N * x_N )  =  b_P
//
// So the matrix is stored the way it is built - a diagonal per cell and two
// coefficients per face - rather than in a general sparse format. There is no
// index search anywhere: assembling a face writes to exactly three places.
// This is the "LDU" layout finite-volume codes use.
//
// Two coefficients per face, not one, because the matrix is only symmetric for
// some equations. Diffusion contributes equally to both sides of a face, but
// upwind convection does not: it weights the upstream cell, and which cell
// that is depends on the flow direction. Momentum is therefore asymmetric,
// while the pressure equation - pure diffusion in disguise - is symmetric, and
// that difference decides which solver each one gets.

#pragma once

#include <cstddef>
#include <vector>

#include "cfd/mesh/Mesh.hpp"

namespace cfd::solver {

class LinearSystem {
 public:
  LinearSystem() = default;
  LinearSystem(std::size_t cells, std::size_t faces);

  /// Zero every coefficient, keeping the allocation.
  void clear();

  [[nodiscard]] std::size_t cellCount() const noexcept { return diagonal.size(); }
  [[nodiscard]] std::size_t faceCount() const noexcept { return upper.size(); }

  /// a_P for each cell.
  std::vector<double> diagonal;
  /// b_P for each cell.
  std::vector<double> source;
  /// Coefficient on the *neighbour's* value in the *owner's* row, per face.
  std::vector<double> upper;
  /// Coefficient on the *owner's* value in the *neighbour's* row, per face.
  std::vector<double> lower;

  /// y = A x
  void multiply(const mesh::Mesh& mesh, const std::vector<double>& x,
                std::vector<double>& y) const;

  /// Sum of |b - A x| over all cells - the unscaled residual.
  [[nodiscard]] double residualL1(const mesh::Mesh& mesh, const std::vector<double>& x) const;

  /// Sum of |a_P| over all cells.
  ///
  /// Multiplied by a velocity scale, this normalises the residual so it is
  /// dimensionless and comparable between equations. Deliberately independent
  /// of the solution: normalising by sum|a_P x_P| collapses to zero whenever a
  /// component is identically zero - which is exactly the case in a uniform
  /// stream, where round-off would then be divided by nothing.
  [[nodiscard]] double diagonalL1() const;

  /// True if every interior face has upper == lower.
  ///
  /// Wake-cut faces are skipped: the two sides of a cut are separate faces, so
  /// each carries only its own row's coefficient and the pair is symmetric
  /// between them rather than within either one.
  [[nodiscard]] bool isSymmetric(const mesh::Mesh& mesh, double tolerance = 1e-12) const;
};

/// Re-exported so solver code can say `oppositeCell` unqualified; the
/// definition belongs to the mesh.
using mesh::oppositeCell;

/// Gauss-Seidel sweeps, in place.
///
/// Cheap, needs no extra storage, and converges for the diagonally dominant
/// systems a finite-volume discretisation produces. It is not asked to solve
/// the momentum equations accurately - inside a SIMPLE iteration the pressure
/// is about to change anyway, so a couple of sweeps to smooth the field is
/// both sufficient and faster than a tight solve.
void gaussSeidel(const mesh::Mesh& mesh, const LinearSystem& system, std::vector<double>& x,
                 int sweeps);

struct SolveReport {
  int iterations{0};
  double initialResidual{0.0};
  double finalResidual{0.0};
  bool converged{false};
};

/// Conjugate gradient with Jacobi preconditioning, for symmetric systems.
///
/// The pressure-correction equation is symmetric and positive definite once a
/// pressure level is anchored, which is exactly the case conjugate gradient is
/// for. It converges far faster than Gauss-Seidel on that equation, and the
/// pressure equation is where most of a SIMPLE iteration's time goes.
///
/// Falls back to reporting failure rather than diverging if handed an
/// asymmetric matrix; check isSymmetric() first when in doubt.
SolveReport conjugateGradient(const mesh::Mesh& mesh, const LinearSystem& system,
                              std::vector<double>& x, double tolerance, int maxIterations);

}  // namespace cfd::solver
