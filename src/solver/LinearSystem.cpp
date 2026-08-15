#include "cfd/solver/LinearSystem.hpp"

#include <algorithm>
#include <cmath>

namespace cfd::solver {

LinearSystem::LinearSystem(std::size_t cells, std::size_t faces)
    : diagonal(cells, 0.0), source(cells, 0.0), upper(faces, 0.0), lower(faces, 0.0) {}

void LinearSystem::clear() {
  std::fill(diagonal.begin(), diagonal.end(), 0.0);
  std::fill(source.begin(), source.end(), 0.0);
  std::fill(upper.begin(), upper.end(), 0.0);
  std::fill(lower.begin(), lower.end(), 0.0);
}

int oppositeCell(const mesh::Mesh& mesh, std::size_t face) noexcept {
  const mesh::Face& f = mesh.faces()[face];
  if (f.neighbour >= 0) {
    return f.neighbour;
  }
  if (f.boundary == mesh::BoundaryType::WakeCut && f.partner >= 0) {
    return mesh.faces()[static_cast<std::size_t>(f.partner)].owner;
  }
  return -1;
}

void LinearSystem::multiply(const mesh::Mesh& mesh, const std::vector<double>& x,
                            std::vector<double>& y) const {
  y.assign(diagonal.size(), 0.0);
  for (std::size_t c = 0; c < diagonal.size(); ++c) {
    y[c] = diagonal[c] * x[c];
  }
  for (std::size_t f = 0; f < mesh.faceCount(); ++f) {
    const mesh::Face& face = mesh.faces()[f];

    if (face.neighbour >= 0) {
      const auto owner = static_cast<std::size_t>(face.owner);
      const auto neighbour = static_cast<std::size_t>(face.neighbour);
      y[owner] += upper[f] * x[neighbour];
      y[neighbour] += lower[f] * x[owner];
      continue;
    }

    // A wake cut face contributes to its owner's row only; the partner face
    // carries the mirror term for the cell on the other side.
    const int across = oppositeCell(mesh, f);
    if (across >= 0) {
      y[static_cast<std::size_t>(face.owner)] += upper[f] * x[static_cast<std::size_t>(across)];
    }
    // Ordinary boundary contributions are folded into diagonal and source.
  }
}

double LinearSystem::residualL1(const mesh::Mesh& mesh, const std::vector<double>& x) const {
  std::vector<double> ax;
  multiply(mesh, x, ax);

  double total = 0.0;
  for (std::size_t c = 0; c < diagonal.size(); ++c) {
    total += std::abs(source[c] - ax[c]);
  }
  return total;
}

double LinearSystem::diagonalL1() const {
  double total = 0.0;
  for (const double value : diagonal) {
    total += std::abs(value);
  }
  return total;
}

bool LinearSystem::isSymmetric(const mesh::Mesh& mesh, double tolerance) const {
  for (std::size_t f = 0; f < upper.size(); ++f) {
    if (mesh.faces()[f].neighbour < 0) {
      continue;  // boundary, or a wake cut whose pair carries the mirror term
    }
    const double scale = std::max({std::abs(upper[f]), std::abs(lower[f]), 1.0});
    if (std::abs(upper[f] - lower[f]) > tolerance * scale) {
      return false;
    }
  }
  return true;
}

void gaussSeidel(const mesh::Mesh& mesh, const LinearSystem& system, std::vector<double>& x,
                 int sweeps) {
  if (x.size() != system.cellCount()) {
    return;
  }

  for (int sweep = 0; sweep < sweeps; ++sweep) {
    for (std::size_t c = 0; c < system.cellCount(); ++c) {
      const double diagonal = system.diagonal[c];
      if (!(std::abs(diagonal) > 0.0)) {
        continue;
      }

      // Subtract every off-diagonal term, using the most recent values
      // available - which is what makes this Gauss-Seidel rather than Jacobi,
      // and roughly doubles the convergence rate for free.
      double offDiagonal = 0.0;
      for (const int faceIndex : mesh.cellFaces()[c]) {
        const auto f = static_cast<std::size_t>(faceIndex);
        const mesh::Face& face = mesh.faces()[f];

        if (face.neighbour >= 0) {
          if (face.owner == static_cast<int>(c)) {
            offDiagonal += system.upper[f] * x[static_cast<std::size_t>(face.neighbour)];
          } else {
            offDiagonal += system.lower[f] * x[static_cast<std::size_t>(face.owner)];
          }
          continue;
        }
        // Wake cut: this cell owns the face, and its coefficient multiplies
        // the cell on the far side of the slit.
        const int across = oppositeCell(mesh, f);
        if (across >= 0 && face.owner == static_cast<int>(c)) {
          offDiagonal += system.upper[f] * x[static_cast<std::size_t>(across)];
        }
      }
      x[c] = (system.source[c] - offDiagonal) / diagonal;
    }
  }
}

SolveReport conjugateGradient(const mesh::Mesh& mesh, const LinearSystem& system,
                              std::vector<double>& x, double tolerance, int maxIterations) {
  SolveReport report;
  const std::size_t n = system.cellCount();
  if (x.size() != n || n == 0) {
    return report;
  }

  std::vector<double> residual(n, 0.0);
  std::vector<double> direction(n, 0.0);
  std::vector<double> preconditioned(n, 0.0);
  std::vector<double> matrixTimesDirection(n, 0.0);

  system.multiply(mesh, x, matrixTimesDirection);
  for (std::size_t c = 0; c < n; ++c) {
    residual[c] = system.source[c] - matrixTimesDirection[c];
  }

  const auto norm = [&](const std::vector<double>& v) {
    double total = 0.0;
    for (const double value : v) {
      total += std::abs(value);
    }
    return total;
  };

  report.initialResidual = norm(residual);
  report.finalResidual = report.initialResidual;
  if (report.initialResidual <= 0.0) {
    report.converged = true;
    return report;
  }

  // Jacobi preconditioning: divide by the diagonal. Crude, but the diagonal of
  // a pressure equation varies by orders of magnitude across a graded mesh, and
  // simply removing that variation is most of the benefit.
  const auto applyPreconditioner = [&]() {
    for (std::size_t c = 0; c < n; ++c) {
      const double d = system.diagonal[c];
      preconditioned[c] = (std::abs(d) > 0.0) ? residual[c] / d : residual[c];
    }
  };

  applyPreconditioner();
  direction = preconditioned;

  double rDotZ = 0.0;
  for (std::size_t c = 0; c < n; ++c) {
    rDotZ += residual[c] * preconditioned[c];
  }

  for (int iteration = 0; iteration < maxIterations; ++iteration) {
    system.multiply(mesh, direction, matrixTimesDirection);

    double dAd = 0.0;
    for (std::size_t c = 0; c < n; ++c) {
      dAd += direction[c] * matrixTimesDirection[c];
    }
    if (!(std::abs(dAd) > 0.0)) {
      break;  // the direction has no component the matrix acts on
    }

    const double step = rDotZ / dAd;
    for (std::size_t c = 0; c < n; ++c) {
      x[c] += step * direction[c];
      residual[c] -= step * matrixTimesDirection[c];
    }

    report.iterations = iteration + 1;
    report.finalResidual = norm(residual);
    if (report.finalResidual <= tolerance * report.initialResidual) {
      report.converged = true;
      break;
    }

    applyPreconditioner();
    double rDotZNext = 0.0;
    for (std::size_t c = 0; c < n; ++c) {
      rDotZNext += residual[c] * preconditioned[c];
    }
    if (!(std::abs(rDotZ) > 0.0)) {
      break;
    }
    const double beta = rDotZNext / rDotZ;
    rDotZ = rDotZNext;

    for (std::size_t c = 0; c < n; ++c) {
      direction[c] = preconditioned[c] + beta * direction[c];
    }
  }

  return report;
}

}  // namespace cfd::solver
