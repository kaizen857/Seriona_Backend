#include "artwork_resolver.h"

#include "spdlog/spdlog.h"

#include <exception>
#include <utility>

namespace seriona::control {

namespace {

[[nodiscard]] std::string coverErrorCodeName(CoverErrorCode code) {
  switch (code) {
  case CoverErrorCode::ExportDirectoryUnavailable:
    return "ExportDirectoryUnavailable";
  case CoverErrorCode::SidecarDiscoveryFailed:
    return "SidecarDiscoveryFailed";
  case CoverErrorCode::SidecarEntryLimitExceeded:
    return "SidecarEntryLimitExceeded";
  case CoverErrorCode::SourceReadFailed:
    return "SourceReadFailed";
  case CoverErrorCode::SourceBudgetExceeded:
    return "SourceBudgetExceeded";
  case CoverErrorCode::DecodeFailed:
    return "DecodeFailed";
  case CoverErrorCode::CacheReadFailed:
    return "CacheReadFailed";
  case CoverErrorCode::CacheWriteFailed:
    return "CacheWriteFailed";
  case CoverErrorCode::PublicationFailed:
    return "PublicationFailed";
  }
  return "UnknownCoverError";
}

[[nodiscard]] ArtworkResolveOutcomeView toOutcomeView(const ArtworkResolveOutcome& outcome) {
  ArtworkResolveOutcomeView view{};
  view.kind = outcome.kind;
  view.fullPath = outcome.fullPath;
  if (outcome.coverError.has_value()) {
    view.detail = coverErrorCodeName(*outcome.coverError);
  }
  return view;
}

ArtworkResolveOutcome loadArtworkViaTagReader(const std::filesystem::path& artworkSourcePath,
                                              const std::filesystem::path& coverExportDir) {
  const auto tag = TagReader::Read(artworkSourcePath,
                                   coverExportDir,
                                   CoverProcessingOptions{
                                       .mode = CoverProcessingOptions::CoverProcessingMode::FullOnly,
                                       .failurePolicy = CoverProcessingOptions::CoverFailurePolicy::Propagate,
                                   });
  if (tag.coverPath().empty()) {
    return {.kind = ArtworkResolveOutcomeKind::NoArt};
  }
  return {.kind = ArtworkResolveOutcomeKind::FullPath, .fullPath = tag.coverPath()};
}

}  // namespace

ArtworkLoader ArtworkResolver::productionLoader() {
  return loadArtworkViaTagReader;
}

ArtworkResolver::ArtworkResolver(std::filesystem::path coverExportDir,
                                 ArtworkResolverCompletion onCompleted,
                                 ArtworkLoader loader)
    : coverExportDir_(std::move(coverExportDir)),
      onCompleted_(std::move(onCompleted)),
      loader_(std::move(loader)),
      worker_([this] { workerMain(); }) {}

ArtworkResolver::~ArtworkResolver() {
  stop();
}

void ArtworkResolver::request(ArtworkResolveRequest request) noexcept {
  {
    std::lock_guard lock{mutex_};
    if (state_ != LifecycleState::Accepting) {
      spdlog::warn("artwork resolver request rejected: resolver is no longer accepting");
      return;
    }
    pending_ = std::move(request);
  }
  workChanged_.notify_one();
}

void ArtworkResolver::setResultCallback(ArtworkResolveCallback callback) noexcept {
  std::lock_guard lock{mutex_};
  resultCallback_ = std::move(callback);
}

void ArtworkResolver::stop() noexcept {
  {
    std::lock_guard lock{mutex_};
    if (state_ != LifecycleState::Accepting) {
      return;
    }
    pending_.reset();
    state_ = LifecycleState::Stopping;
  }
  completionEpoch_.fetch_add(1, std::memory_order_acq_rel);
  workChanged_.notify_all();
  if (worker_.joinable()) {
    if (worker_.get_id() == std::this_thread::get_id()) {
      // Defensive: stop() called from the completion callback (worker thread)
      // cannot join itself; the worker exits on its own right after.
      worker_.detach();
    } else {
      worker_.join();
    }
  }
  {
    std::lock_guard lock{mutex_};
    state_ = LifecycleState::Stopped;
  }
}

bool ArtworkResolver::stopped() const noexcept {
  std::lock_guard lock{mutex_};
  return state_ != LifecycleState::Accepting;
}

void ArtworkResolver::workerMain() noexcept {
  for (;;) {
    ArtworkResolveRequest request;
    {
      std::unique_lock lock{mutex_};
      workChanged_.wait(lock, [this] { return state_ != LifecycleState::Accepting || pending_.has_value(); });
      if (state_ != LifecycleState::Accepting) {
        return;
      }
      request = std::move(*pending_);
      pending_.reset();
    }

    ArtworkResolveOutcome outcome;
    try {
      outcome = loader_(request.artworkSourcePath, coverExportDir_);
    } catch (const CoverProcessingError& error) {
      outcome = {.kind = ArtworkResolveOutcomeKind::CoverError, .coverError = error.code()};
    } catch (...) {
      outcome = {.kind = ArtworkResolveOutcomeKind::ResolverFailure};
    }

    ArtworkResolveResult result;
    ArtworkResolveResultView resultView;
    ArtworkResolveCallback resultCallback;
    std::uint64_t publishEpoch{0};
    bool publish{false};
    {
      std::lock_guard lock{mutex_};
      if (state_ != LifecycleState::Accepting || pending_.has_value()) {
        continue;  // stopping or superseded by a newer request: drop
      }
      publishEpoch = completionEpoch_.load(std::memory_order_acquire);
      result = ArtworkResolveResult{.generation = request.generation,
                                    .identity = std::move(request.identity),
                                    .outcome = std::move(outcome)};
      resultView = ArtworkResolveResultView{.generation = result.generation,
                                            .identity = result.identity,
                                            .outcome = toOutcomeView(result.outcome)};
      resultCallback = resultCallback_;
      publish = true;
    }
    if (publish && completionEpoch_.load(std::memory_order_acquire) == publishEpoch) {
      if (onCompleted_) {
        onCompleted_(std::move(result));
      }
      if (resultCallback) {
        resultCallback(std::move(resultView));
      }
    }
  }
}

}
