#pragma once

#include "seriona/control/control_contracts.h"

#include <TagReader.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace seriona::control {

// Bounded, latest-wins background artwork resolver, internal to the control
// layer (never part of the public API).
//
// One worker thread resolves at most one request at a time and keeps at most
// one pending request, so the queue depth is bounded by two. A newer request
// replaces the pending one; a result that was superseded while in flight is
// dropped, and every published result carries the request generation so the
// completion side can still discard stale results.
//
// Lifecycle is Accepting -> Stopping -> Stopped. stop() clears the pending
// request, invalidates the shared completion epoch, then joins the worker.
// The in-flight synchronous TagReader call is never forcibly cancelled:
// production shutdown may wait for it to return. Once the epoch is
// invalidated no completion callback runs, so after stop() returns the
// callback must never touch controller state.

struct ArtworkResolveOutcome {
  ArtworkResolveOutcomeKind kind{ArtworkResolveOutcomeKind::NoArt};
  std::optional<std::filesystem::path> fullPath;
  std::optional<CoverErrorCode> coverError;
};

struct ArtworkResolveResult {
  std::uint64_t generation{0};
  TrackIdentity identity;
  ArtworkResolveOutcome outcome;
};

using ArtworkLoader = std::function<ArtworkResolveOutcome(const std::filesystem::path& artworkSourcePath,
                                                         const std::filesystem::path& coverExportDir)>;
using ArtworkResolverCompletion = std::function<void(ArtworkResolveResult)>;

class ArtworkResolver : public ArtworkResolveService {
public:
  ArtworkResolver(std::filesystem::path coverExportDir,
                  ArtworkResolverCompletion onCompleted,
                  ArtworkLoader loader = productionLoader());
  ~ArtworkResolver();

  ArtworkResolver(const ArtworkResolver&) = delete;
  ArtworkResolver& operator=(const ArtworkResolver&) = delete;

  // Rejects the request when the resolver is no longer accepting. Replaces
  // any earlier pending request (latest wins). Never blocks on the loader.
  void request(ArtworkResolveRequest request) noexcept override;
  // Registers the controller's result callback; results are delivered as
  // ArtworkResolveResultView alongside the internal completion callback.
  void setResultCallback(ArtworkResolveCallback callback) noexcept override;
  // Idempotent: clears pending, stops accepting, waits for in-flight work,
  // then reports stopped. Safe to call repeatedly.
  void stop() noexcept override;
  [[nodiscard]] bool stopped() const noexcept;

  [[nodiscard]] const std::filesystem::path& coverExportDir() const noexcept { return coverExportDir_; }

  // Loader that calls TagReader with FullOnly + Propagate, maps an empty
  // cover path to NoArt, and lets CoverProcessingError / ordinary media
  // errors surface for the worker to classify.
  static ArtworkLoader productionLoader();

private:
  enum class LifecycleState { Accepting, Stopping, Stopped };

  void workerMain() noexcept;

  std::filesystem::path coverExportDir_;
  ArtworkResolverCompletion onCompleted_;
  ArtworkResolveCallback resultCallback_;
  ArtworkLoader loader_;
  mutable std::mutex mutex_;
  std::condition_variable workChanged_;
  std::optional<ArtworkResolveRequest> pending_;
  std::atomic<std::uint64_t> completionEpoch_{0};
  LifecycleState state_{LifecycleState::Accepting};
  std::thread worker_;
};

}
