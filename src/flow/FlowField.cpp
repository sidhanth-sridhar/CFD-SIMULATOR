#include "cfd/flow/FlowField.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace cfd::flow {

void FlowField::resize(std::size_t cells) {
  velocity.assign(cells, Vec2{0.0, 0.0});
  pressure.assign(cells, 0.0);
  density.assign(cells, 0.0);
  viscosity.assign(cells, 0.0);
}

bool FlowField::isConsistent() const noexcept {
  const std::size_t n = velocity.size();
  return pressure.size() == n && density.size() == n && viscosity.size() == n;
}

Result<FlowField> FlowField::uniform(std::size_t cells,
                                     const FreestreamConditions& freestream, double chord) {
  if (const Status valid = freestream.validate(); !valid) {
    return valid.error();
  }
  if (!std::isfinite(chord) || chord <= 0.0) {
    return Error{ErrorCode::InvalidArgument,
                 std::format("chord must be positive, got {}", chord)};
  }
  if (cells == 0) {
    return Error{ErrorCode::InvalidArgument, "cannot initialise a field with no cells"};
  }

  FlowField field;
  field.resize(cells);

  std::fill(field.velocity.begin(), field.velocity.end(), freestream.velocity());
  std::fill(field.pressure.begin(), field.pressure.end(), freestream.referencePressure);
  std::fill(field.density.begin(), field.density.end(), freestream.density);
  // Molecular viscosity only. The turbulence model will add an eddy viscosity
  // on top of this from Phase 5; until then the effective viscosity is the
  // fluid's own.
  std::fill(field.viscosity.begin(), field.viscosity.end(),
            freestream.dynamicViscosity(chord));

  return field;
}

double ResidualSet::worst() const noexcept {
  return std::max({std::abs(continuity), std::abs(momentumX), std::abs(momentumY)});
}

ResidualHistory::ResidualHistory(std::size_t capacity)
    : capacity_(capacity == 0 ? 1 : capacity) {}

void ResidualHistory::record(long long iteration, const ResidualSet& residuals) {
  if (entries_.size() >= capacity_) {
    entries_.pop_front();
    ++dropped_;
  }
  entries_.push_back(Entry{iteration, residuals});
}

void ResidualHistory::clear() noexcept {
  entries_.clear();
  dropped_ = 0;
}

ResidualSet ResidualHistory::latest() const {
  return entries_.empty() ? ResidualSet{} : entries_.back().residuals;
}

}  // namespace cfd::flow
