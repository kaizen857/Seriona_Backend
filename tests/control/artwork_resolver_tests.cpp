#include "../../src/control/artwork_resolver.h"

#include <doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace seriona::control;

namespace {

constexpr std::chrono::milliseconds kGenerationTimeout{2000};
constexpr std::chrono::milliseconds kQuickReturnBudget{200};  // design goal 50 ms; relaxed for CI jitter

ArtworkResolveRequest makeRequest(std::uint64_t generation, std::string sourcePath) {
  TrackIdentity identity;
  identity.trackId = "track-" + std::to_string(generation);
  return ArtworkResolveRequest{.generation = generation,
                               .identity = std::move(identity),
                               .artworkSourcePath = std::filesystem::path{std::move(sourcePath)},
                               .fallbackThumbnailPath = std::filesystem::path{}};
}

// Collects completion results on the resolver worker thread; assertions must
// stay on the test thread.
class ResultCollector {
public:
  void operator()(ArtworkResolveResult result) {
    {
      std::lock_guard lock{mutex_};
      results_.push_back(std::move(result));
    }
    changed_.notify_all();
  }

  bool waitForCount(std::size_t count, std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout, [this, count] { return results_.size() >= count; });
  }

  [[nodiscard]] std::size_t count() const {
    std::lock_guard lock{mutex_};
    return results_.size();
  }

  [[nodiscard]] ArtworkResolveResult last() const {
    std::lock_guard lock{mutex_};
    return results_.back();
  }

  [[nodiscard]] const std::vector<ArtworkResolveResult>& results() const {
    std::lock_guard lock{mutex_};
    return results_;
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<ArtworkResolveResult> results_;
};

// Loader that blocks until released, records every requested source path, and
// then returns a configured outcome or rethrows a configured exception.
class BlockingLoader {
public:
  ArtworkResolveOutcome operator()(const std::filesystem::path& sourcePath, const std::filesystem::path&) {
    {
      std::lock_guard lock{mutex_};
      requestedSourcePaths_.push_back(sourcePath.generic_string());
      entered_ = true;
    }
    enteredCv_.notify_all();
    {
      std::unique_lock lock{mutex_};
      releaseCv_.wait(lock, [this] { return released_; });
    }
    if (pendingException_) {
      std::rethrow_exception(pendingException_);
    }
    return outcome_;
  }

  bool waitForEnter(std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex_};
    return enteredCv_.wait_for(lock, timeout, [this] { return entered_; });
  }

  void release() {
    {
      std::lock_guard lock{mutex_};
      released_ = true;
    }
    releaseCv_.notify_all();
  }

  [[nodiscard]] std::vector<std::string> requestedSourcePaths() const {
    std::lock_guard lock{mutex_};
    return requestedSourcePaths_;
  }

  ArtworkResolveOutcome outcome_;
  std::exception_ptr pendingException_;

private:
  mutable std::mutex mutex_;
  std::condition_variable enteredCv_;
  std::condition_variable releaseCv_;
  bool entered_{false};
  bool released_{false};
  std::vector<std::string> requestedSourcePaths_;
};

// Loader that returns (or throws) immediately and counts its invocations.
class ImmediateLoader {
public:
  ArtworkResolveOutcome operator()(const std::filesystem::path&, const std::filesystem::path&) {
    ++calls_;
    if (pendingException_) {
      std::rethrow_exception(pendingException_);
    }
    return outcome_;
  }

  [[nodiscard]] std::size_t calls() const noexcept {
    return calls_;
  }

  ArtworkResolveOutcome outcome_;
  std::exception_ptr pendingException_;

private:
  std::atomic<std::size_t> calls_{0};
};

}  // namespace

TEST_CASE("artwork resolver request returns well within the design target with a 500 ms loader") {
  ResultCollector collector;
  auto slowLoader = [](const std::filesystem::path&, const std::filesystem::path&) {
    std::this_thread::sleep_for(std::chrono::milliseconds{500});
    ArtworkResolveOutcome outcome;
    outcome.kind = ArtworkResolveOutcomeKind::FullPath;
    outcome.fullPath = std::filesystem::path{"/covers/full.png"};
    return outcome;
  };
  ArtworkResolver resolver{std::filesystem::path{}, std::ref(collector), slowLoader};

  const auto before = std::chrono::steady_clock::now();
  resolver.request(makeRequest(11, "/audio/slow.flac"));
  const auto elapsed = std::chrono::steady_clock::now() - before;
  CHECK(elapsed < kQuickReturnBudget);

  CHECK(collector.waitForCount(1, kGenerationTimeout));
  CHECK(collector.count() == 1);
  CHECK(collector.last().generation == 11);
  CHECK(collector.last().identity.trackId == "track-11");
  CHECK(collector.last().outcome.kind == ArtworkResolveOutcomeKind::FullPath);
  CHECK(collector.last().outcome.fullPath.has_value());
  CHECK(collector.last().outcome.fullPath->generic_string() == "/covers/full.png");
  resolver.stop();
}

TEST_CASE("artwork resolver request returns without waiting for a blocked loader") {
  BlockingLoader loader;
  ResultCollector collector;
  ArtworkResolver resolver{std::filesystem::path{}, std::ref(collector), std::ref(loader)};

  const auto before = std::chrono::steady_clock::now();
  resolver.request(makeRequest(1, "/audio/blocked.flac"));
  const auto elapsed = std::chrono::steady_clock::now() - before;
  CHECK(elapsed < kQuickReturnBudget);

  CHECK(loader.waitForEnter(kGenerationTimeout));
  loader.release();
  CHECK(collector.waitForCount(1, kGenerationTimeout));
  CHECK(collector.last().generation == 1);
  resolver.stop();
}

TEST_CASE("artwork resolver keeps queue depth bounded and latest pending wins") {
  BlockingLoader loader;
  loader.outcome_.kind = ArtworkResolveOutcomeKind::FullPath;
  loader.outcome_.fullPath = std::filesystem::path{"/covers/final.png"};
  ResultCollector collector;
  ArtworkResolver resolver{std::filesystem::path{}, std::ref(collector), std::ref(loader)};

  resolver.request(makeRequest(1, "/audio/a.flac"));
  CHECK(loader.waitForEnter(kGenerationTimeout));

  // While the first request is in flight, two more arrive: the second becomes
  // pending and the third replaces it (latest wins), keeping depth at two.
  resolver.request(makeRequest(2, "/audio/b.flac"));
  resolver.request(makeRequest(3, "/audio/c.flac"));

  loader.release();
  CHECK(collector.waitForCount(1, kGenerationTimeout));

  CHECK(collector.count() == 1);
  CHECK(collector.last().generation == 3);
  CHECK(collector.last().identity.trackId == "track-3");
  // The single published result is the final generation's full artwork.
  CHECK(collector.last().outcome.kind == ArtworkResolveOutcomeKind::FullPath);
  CHECK(collector.last().outcome.fullPath.has_value());
  CHECK(collector.last().outcome.fullPath->generic_string() == "/covers/final.png");

  const auto requested = loader.requestedSourcePaths();
  CHECK(requested.size() <= 2);
  CHECK(requested == std::vector<std::string>{"/audio/a.flac", "/audio/c.flac"});
  resolver.stop();
}

TEST_CASE("artwork resolver drops a result superseded by a newer request") {
  BlockingLoader loader;
  ResultCollector collector;
  ArtworkResolver resolver{std::filesystem::path{}, std::ref(collector), std::ref(loader)};

  resolver.request(makeRequest(1, "/audio/a.flac"));
  CHECK(loader.waitForEnter(kGenerationTimeout));
  resolver.request(makeRequest(2, "/audio/b.flac"));
  loader.release();

  CHECK(collector.waitForCount(1, kGenerationTimeout));
  CHECK(collector.count() == 1);
  CHECK(collector.last().generation == 2);
  resolver.stop();
}

TEST_CASE("artwork resolver publishes a resolved full path outcome") {
  ImmediateLoader loader;
  loader.outcome_.kind = ArtworkResolveOutcomeKind::FullPath;
  loader.outcome_.fullPath = std::filesystem::path{"/covers/ab12cd.png"};
  ResultCollector collector;
  ArtworkResolver resolver{std::filesystem::path{}, std::ref(collector), std::ref(loader)};

  resolver.request(makeRequest(7, "/audio/full.flac"));
  CHECK(collector.waitForCount(1, kGenerationTimeout));
  CHECK(collector.last().generation == 7);
  CHECK(collector.last().outcome.kind == ArtworkResolveOutcomeKind::FullPath);
  CHECK(collector.last().outcome.fullPath.has_value());
  CHECK(collector.last().outcome.fullPath->generic_string() == "/covers/ab12cd.png");
  resolver.stop();
}

TEST_CASE("artwork resolver publishes a no-art outcome") {
  ImmediateLoader loader;
  loader.outcome_.kind = ArtworkResolveOutcomeKind::NoArt;
  ResultCollector collector;
  ArtworkResolver resolver{std::filesystem::path{}, std::ref(collector), std::ref(loader)};

  resolver.request(makeRequest(8, "/audio/noart.flac"));
  CHECK(collector.waitForCount(1, kGenerationTimeout));
  CHECK(collector.last().outcome.kind == ArtworkResolveOutcomeKind::NoArt);
  CHECK_FALSE(collector.last().outcome.fullPath.has_value());
  CHECK_FALSE(collector.last().outcome.coverError.has_value());
  resolver.stop();
}

TEST_CASE("artwork resolver maps a CoverProcessingError to its typed code") {
  ImmediateLoader loader;
  loader.pendingException_ = std::make_exception_ptr(
      CoverProcessingError(CoverErrorCode::SourceBudgetExceeded, "budget exceeded", std::filesystem::path{"/some/art.bin"}));
  ResultCollector collector;
  ArtworkResolver resolver{std::filesystem::path{}, std::ref(collector), std::ref(loader)};

  resolver.request(makeRequest(9, "/audio/big.flac"));
  CHECK(collector.waitForCount(1, kGenerationTimeout));
  CHECK(collector.last().outcome.kind == ArtworkResolveOutcomeKind::CoverError);
  CHECK(collector.last().outcome.coverError.has_value());
  CHECK(collector.last().outcome.coverError.value() == CoverErrorCode::SourceBudgetExceeded);
  resolver.stop();
}

TEST_CASE("artwork resolver maps ordinary media errors to an internal resolver failure") {
  ImmediateLoader loader;
  loader.pendingException_ = std::make_exception_ptr(std::runtime_error{"file does not exist"});
  ResultCollector collector;
  ArtworkResolver resolver{std::filesystem::path{}, std::ref(collector), std::ref(loader)};

  resolver.request(makeRequest(10, "/audio/broken.flac"));
  CHECK(collector.waitForCount(1, kGenerationTimeout));
  CHECK(collector.last().outcome.kind == ArtworkResolveOutcomeKind::ResolverFailure);
  CHECK_FALSE(collector.last().outcome.fullPath.has_value());
  CHECK_FALSE(collector.last().outcome.coverError.has_value());
  resolver.stop();
}

TEST_CASE("artwork resolver production loader maps ordinary media errors internally") {
  const auto exportDir = std::filesystem::temp_directory_path() / "seriona-artwork-resolver-tests";
  std::error_code error;
  std::filesystem::create_directories(exportDir, error);

  ResultCollector collector;
  ArtworkResolver resolver{exportDir, std::ref(collector)};  // default production loader

  resolver.request(makeRequest(5, (exportDir / "missing-track.flac").generic_string()));
  CHECK(collector.waitForCount(1, kGenerationTimeout));
  CHECK(collector.last().outcome.kind == ArtworkResolveOutcomeKind::ResolverFailure);
  resolver.stop();

  std::filesystem::remove_all(exportDir, error);
}

TEST_CASE("artwork resolver stop before start is immediate and idempotent") {
  ImmediateLoader loader;
  ResultCollector collector;
  ArtworkResolver resolver{std::filesystem::path{}, std::ref(collector), std::ref(loader)};

  const auto before = std::chrono::steady_clock::now();
  resolver.stop();
  const auto elapsed = std::chrono::steady_clock::now() - before;
  CHECK(elapsed < kQuickReturnBudget);
  CHECK(resolver.stopped());

  resolver.stop();  // repeated stop is safe
  CHECK(resolver.stopped());
  CHECK(collector.count() == 0);
  CHECK(loader.calls() == 0);
}

TEST_CASE("artwork resolver stop waits for in-flight work and publishes nothing") {
  BlockingLoader loader;
  ResultCollector collector;
  ArtworkResolver resolver{std::filesystem::path{}, std::ref(collector), std::ref(loader)};

  resolver.request(makeRequest(1, "/audio/a.flac"));
  CHECK(loader.waitForEnter(kGenerationTimeout));

  std::promise<void> stopDone;
  auto stopSignal = stopDone.get_future();
  std::thread stopper{[&resolver, &stopDone] {
    resolver.stop();
    stopDone.set_value();
  }};

  // stop() must block while the loader is in flight.
  std::this_thread::sleep_for(std::chrono::milliseconds{150});
  CHECK(stopSignal.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);

  loader.release();
  CHECK(stopSignal.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
  stopper.join();

  CHECK(resolver.stopped());
  CHECK(collector.count() == 0);  // no completion may run after stop
}

TEST_CASE("artwork resolver delivers resolved results to the registered controller callback") {
  ImmediateLoader loader;
  loader.outcome_.kind = ArtworkResolveOutcomeKind::FullPath;
  loader.outcome_.fullPath = std::filesystem::path{"/covers/full.png"};
  ResultCollector collector;
  ArtworkResolver resolver{std::filesystem::path{}, std::ref(collector), std::ref(loader)};

  std::mutex mutex;
  std::condition_variable changed;
  std::vector<ArtworkResolveResultView> views;
  resolver.setResultCallback([&](ArtworkResolveResultView view) {
    {
      std::lock_guard lock{mutex};
      views.push_back(std::move(view));
    }
    changed.notify_all();
  });

  resolver.request(makeRequest(12, "/audio/full.flac"));
  {
    std::unique_lock lock{mutex};
    CHECK(changed.wait_for(lock, kGenerationTimeout, [&] { return !views.empty(); }));
  }
  REQUIRE(views.size() == 1);
  CHECK(views[0].generation == 12);
  CHECK(views[0].identity.trackId == "track-12");
  CHECK(views[0].outcome.kind == ArtworkResolveOutcomeKind::FullPath);
  REQUIRE(views[0].outcome.fullPath.has_value());
  CHECK(views[0].outcome.fullPath->generic_string() == "/covers/full.png");
  CHECK(views[0].outcome.detail.empty());
  resolver.stop();
}

TEST_CASE("artwork resolver maps a cover error code into the controller view detail") {
  ImmediateLoader loader;
  loader.pendingException_ = std::make_exception_ptr(
      CoverProcessingError(CoverErrorCode::SourceBudgetExceeded, "budget exceeded", std::filesystem::path{"/some/art.bin"}));
  ResultCollector collector;
  ArtworkResolver resolver{std::filesystem::path{}, std::ref(collector), std::ref(loader)};

  std::mutex mutex;
  std::condition_variable changed;
  std::vector<ArtworkResolveResultView> views;
  resolver.setResultCallback([&](ArtworkResolveResultView view) {
    {
      std::lock_guard lock{mutex};
      views.push_back(std::move(view));
    }
    changed.notify_all();
  });

  resolver.request(makeRequest(13, "/audio/big.flac"));
  {
    std::unique_lock lock{mutex};
    CHECK(changed.wait_for(lock, kGenerationTimeout, [&] { return !views.empty(); }));
  }
  REQUIRE(views.size() == 1);
  CHECK(views[0].outcome.kind == ArtworkResolveOutcomeKind::CoverError);
  CHECK_FALSE(views[0].outcome.fullPath.has_value());
  CHECK(views[0].outcome.detail == "SourceBudgetExceeded");
  resolver.stop();
}

TEST_CASE("artwork resolver stop while running rejects queued and new requests") {
  BlockingLoader loader;
  ResultCollector collector;
  ArtworkResolver resolver{std::filesystem::path{}, std::ref(collector), std::ref(loader)};

  resolver.request(makeRequest(1, "/audio/a.flac"));
  CHECK(loader.waitForEnter(kGenerationTimeout));
  resolver.request(makeRequest(2, "/audio/b.flac"));  // pending

  // stop() joins the in-flight loader, so it must run off-thread here.
  std::thread stopper{[&resolver] { resolver.stop(); }};
  std::this_thread::sleep_for(std::chrono::milliseconds{100});
  loader.release();
  stopper.join();

  CHECK(resolver.stopped());
  CHECK(collector.count() == 0);

  resolver.request(makeRequest(3, "/audio/c.flac"));  // rejected: stopped
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  CHECK(collector.count() == 0);
  CHECK(loader.requestedSourcePaths().size() <= 1);
}
