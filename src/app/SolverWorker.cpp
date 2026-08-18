#include "SolverWorker.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

#include "cfd/core/Log.hpp"

namespace cfd::app {
namespace {

constexpr std::string_view kLogCategory = "solver";

/// Shortest interval between snapshots, in milliseconds.
///
/// Publishing copies the whole field, so on a cheap mesh an unthrottled worker
/// would spend most of its time copying data the display cannot show anyway.
/// A 30 ms floor is a little under half a 60 Hz frame: fast enough that the
/// viewport looks live, slow enough that the copies stay negligible.
constexpr double kMinPublishMs = 30.0;

/// Iterations between progress lines in the log.
constexpr long long kProgressEvery = 500;

float asLog(double value) {
  return static_cast<float>(std::log10(std::max(value, 1e-20)));
}

}  // namespace

SolverWorker::~SolverWorker() { stop(); }

void SolverWorker::adopt(std::shared_ptr<const mesh::Mesh> mesh,
                         solver::SimpleSolver solver) {
  // Joins the previous thread, so everything below happens with no worker
  // running and no lock needed for the owned state.
  stop();

  const std::lock_guard<std::mutex> guard(mutex_);
  mesh_ = std::move(mesh);
  solver_.emplace(std::move(solver));

  quit_ = false;
  budget_ = -1;
  hasPending_ = false;
  pending_ = SolverUpdate{};
  pendingContinuity_.clear();
  pendingMomentum_.clear();
  running_.store(false);

  thread_ = std::thread(&SolverWorker::runLoop, this);
}

void SolverWorker::stop() {
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    quit_ = true;
    running_.store(false);
  }
  wake_.notify_all();

  if (thread_.joinable()) {
    thread_.join();
  }

  // Only now is it safe to let go of the solver and the grid under it.
  const std::lock_guard<std::mutex> guard(mutex_);
  solver_.reset();
  mesh_.reset();
}

bool SolverWorker::hasSolver() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return solver_.has_value();
}

void SolverWorker::setRunning(bool running) {
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (running && budget_ == 0) {
      // A finished Step left no budget; asking to run means run freely again.
      budget_ = -1;
    }
    running_.store(running);
  }
  wake_.notify_all();
}

void SolverWorker::requestIterations(long long count) {
  if (count <= 0) {
    return;
  }
  {
    const std::lock_guard<std::mutex> guard(mutex_);
    budget_ = count;
    running_.store(true);
  }
  wake_.notify_all();
}

void SolverWorker::setSettings(const solver::SimpleSettings& settings) {
  const std::lock_guard<std::mutex> guard(mutex_);
  settings_ = settings;
}

void SolverWorker::setLimits(double convergenceTolerance, long long maxIterations,
                             int iterationsPerUpdate) {
  const std::lock_guard<std::mutex> guard(mutex_);
  tolerance_ = convergenceTolerance;
  maxIterations_ = maxIterations;
  iterationsPerUpdate_ = std::max(iterationsPerUpdate, 1);
}

bool SolverWorker::poll(SolverUpdate& out) {
  const std::lock_guard<std::mutex> guard(mutex_);
  if (!hasPending_) {
    return false;
  }
  out = std::move(pending_);
  out.continuityHistory = std::move(pendingContinuity_);
  out.momentumHistory = std::move(pendingMomentum_);
  pending_ = SolverUpdate{};
  pendingContinuity_.clear();
  pendingMomentum_.clear();
  hasPending_ = false;
  return true;
}

bool SolverWorker::shouldIterate() const {
  return !quit_ && running_.load() && budget_ != 0 && solver_.has_value();
}

void SolverWorker::runLoop() {
  long long iteration = 0;
  auto lastPublish = std::chrono::steady_clock::now();
  long long sincePublish = 0;

  for (;;) {
    solver::SimpleSettings settings;
    double tolerance = 0.0;
    long long maxIterations = 0;
    int perUpdate = 1;

    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return quit_ || shouldIterate(); });
      if (quit_) {
        return;
      }
      settings = settings_;
      tolerance = tolerance_;
      maxIterations = maxIterations_;
      perUpdate = iterationsPerUpdate_;
    }

    bool converged = false;
    bool hitLimit = false;
    bool diverged = false;

    solver_->setSettings(settings);
    const solver::SolverMonitor monitor = solver_->iterate();
    ++iteration;
    ++sincePublish;

    const double worstMomentum =
        std::max(monitor.residuals.momentumX, monitor.residuals.momentumY);

    if (!std::isfinite(monitor.residuals.continuity) ||
        !std::isfinite(monitor.residuals.momentumX) ||
        !std::isfinite(monitor.residuals.momentumY)) {
      diverged = true;
      // The only outcome that used to be silent, which is exactly backwards:
      // convergence and the iteration limit both announced themselves while a
      // blow-up did not, so a run that had destroyed its own field looked from
      // the log like one that had simply stopped.
      CFD_LOG_WARN(kLogCategory,
                   "diverged at iteration {}: continuity {:.3e}, momentum {:.3e} / {:.3e} "
                   "- the field is not a solution",
                   iteration, monitor.residuals.continuity, monitor.residuals.momentumX,
                   monitor.residuals.momentumY);
    } else if (monitor.residuals.worst() < tolerance) {
      converged = true;
      CFD_LOG_INFO(kLogCategory, "converged after {} iterations, continuity {:.3e}",
                   iteration, monitor.residuals.continuity);
    } else if (maxIterations > 0 && iteration >= maxIterations) {
      hitLimit = true;
      CFD_LOG_WARN(kLogCategory,
                   "stopped at the {} iteration limit; continuity {:.3e}, momentum {:.3e} "
                   "- not converged",
                   maxIterations, monitor.residuals.continuity, monitor.residuals.worst());
    }

    if (iteration % kProgressEvery == 0) {
      CFD_LOG_INFO(kLogCategory, "iteration {}: continuity {:.3e}, momentum {:.3e} / {:.3e}",
                   iteration, monitor.residuals.continuity, monitor.residuals.momentumX,
                   monitor.residuals.momentumY);
    }

    const bool finished = converged || hitLimit || diverged;
    if (finished) {
      running_.store(false);
    }

    const auto now = std::chrono::steady_clock::now();
    const double sinceMs =
        std::chrono::duration<double, std::milli>(now - lastPublish).count();

    bool publish = false;
    {
      const std::lock_guard<std::mutex> guard(mutex_);
      pendingContinuity_.push_back(asLog(monitor.residuals.continuity));
      pendingMomentum_.push_back(asLog(worstMomentum));

      // A Step asks for a fixed number of iterations and then a pause.
      bool budgetSpent = false;
      if (budget_ > 0) {
        --budget_;
        if (budget_ == 0) {
          budgetSpent = true;
          running_.store(false);
        }
      }

      // Whenever the worker is about to go quiet, publish regardless of the
      // interval: the field it stopped on is the one being waited for, and a
      // single-step that showed nothing would be no step at all.
      publish = finished || budgetSpent ||
                (sincePublish >= perUpdate && sinceMs >= kMinPublishMs);

      if (publish) {
        pending_.field = solver_->field();
        pending_.divergence = solver_->divergence();
        pending_.monitor = monitor;
        pending_.iteration = iteration;
        pending_.converged = converged;
        pending_.hitIterationLimit = hitLimit;
        pending_.diverged = diverged;
        pending_.running = running_.load();
        hasPending_ = true;
      }
    }

    if (publish) {
      lastPublish = now;
      sincePublish = 0;
    }
  }
}

}  // namespace cfd::app
