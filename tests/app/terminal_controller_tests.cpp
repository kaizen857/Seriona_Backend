#include "../../app/terminal_io.h"

#include "../control/control_test_harness.h"

#include "seriona/control/media_controller.h"

#include <doctest.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace control = seriona::control;
namespace control_test = seriona::control::test;
namespace scanner = seriona::scanner;

namespace seriona::app::testing {
int runTerminalControllerForTest(const std::filesystem::path& musicPath,
                                 control::MediaController& controller,
                                 const std::function<TerminalAction(std::chrono::milliseconds)>& readAction,
                                 std::ostream& output,
                                 std::ostream& error);
}

namespace {

struct TerminalControllerFixture {
  std::shared_ptr<control_test::FakeAudioPlaybackService> fakeAudio{std::make_shared<control_test::FakeAudioPlaybackService>()};
  std::shared_ptr<control_test::FakeFileScannerService> fakeScanner{std::make_shared<control_test::FakeFileScannerService>()};
  std::unique_ptr<control::MediaController> controller{};

  TerminalControllerFixture() {
    auto metadataService = std::make_unique<control_test::FakeMetadataSharingService>();
    controller = control::makeMediaController(control::MediaControllerDependencies{.audio = fakeAudio,
                                                                                   .scanner = fakeScanner,
                                                                                   .metadata = std::move(metadataService),
                                                                                   .folderSortSettingsStore = {}},
                                              control::MediaControllerOptions{.runInlineForTests = true});
  }
};

class ScriptedTerminalActions {
public:
  explicit ScriptedTerminalActions(std::vector<seriona::app::TerminalAction> actions) : actions_{std::move(actions)} {}

  seriona::app::TerminalAction operator()(std::chrono::milliseconds) {
    if (next_ >= actions_.size()) {
      return seriona::app::TerminalAction::Quit;
    }
    return actions_[next_++];
  }

private:
  std::vector<seriona::app::TerminalAction> actions_{};
  std::size_t next_{0};
};

class InspectingQuitAction {
public:
  InspectingQuitAction(const control_test::FakeFileScannerService& scanner, std::filesystem::path expectedRoot)
      : scanner_{scanner}, expectedRoot_{std::move(expectedRoot)} {}

  seriona::app::TerminalAction operator()(std::chrono::milliseconds) {
    CHECK(scanner_.scanCalls() == 1U);
    CHECK(scanner_.startWatchingCalls() == 1U);
    REQUIRE(scanner_.lastScannedRoots().has_value());
    REQUIRE(scanner_.lastWatchingRoots().has_value());
    REQUIRE(scanner_.lastScannedRoots()->size() == 1U);
    REQUIRE(scanner_.lastWatchingRoots()->size() == 1U);
    CHECK(scanner_.lastScannedRoots()->front().path == expectedRoot_);
    CHECK(scanner_.lastWatchingRoots()->front().path == expectedRoot_);
    CHECK(scanner_.lastScanMode().has_value());
    CHECK(*scanner_.lastScanMode() == scanner::ScanMode::Full);
    return seriona::app::TerminalAction::Quit;
  }

private:
  const control_test::FakeFileScannerService& scanner_;
  std::filesystem::path expectedRoot_;
};

}

TEST_CASE("terminal controller scans through MediaController so watcher starts") {
  TerminalControllerFixture fixture{};
  std::ostringstream output{};
  std::ostringstream error{};
  const auto root = std::filesystem::path{"/tmp/seriona-terminal-library"};
  InspectingQuitAction actions{*fixture.fakeScanner, root};

  const auto exitCode = seriona::app::testing::runTerminalControllerForTest(root, *fixture.controller, actions, output, error);

  CHECK(exitCode == 0);
  CHECK(fixture.fakeScanner->stopWatchingCalls() == 1U);
  CHECK_FALSE(fixture.fakeScanner->lastWatchingRoots().has_value());
  CHECK(error.str().find("failed to scan library") == std::string::npos);
}

TEST_CASE("terminal controller shutdown path stops watcher through MediaController") {
  TerminalControllerFixture fixture{};
  ScriptedTerminalActions actions{{seriona::app::TerminalAction::Quit}};
  std::ostringstream output{};
  std::ostringstream error{};

  const auto exitCode = seriona::app::testing::runTerminalControllerForTest("/tmp/seriona-terminal-shutdown", *fixture.controller, actions, output, error);

  CHECK(exitCode == 0);
  CHECK(fixture.fakeScanner->startWatchingCalls() == 1U);
  CHECK(fixture.fakeScanner->stopWatchingCalls() == 1U);
  CHECK_FALSE(fixture.fakeScanner->lastWatchingRoots().has_value());
}

TEST_CASE("terminal controller reports watcher start failure as scan failure") {
  TerminalControllerFixture fixture{};
  fixture.fakeScanner->startWatchingThrows(std::runtime_error{"terminal watcher start failed"});
  ScriptedTerminalActions actions{{seriona::app::TerminalAction::Quit}};
  std::ostringstream output{};
  std::ostringstream error{};

  const auto exitCode = seriona::app::testing::runTerminalControllerForTest("/tmp/seriona-terminal-failure", *fixture.controller, actions, output, error);

  CHECK(exitCode == 1);
  CHECK(fixture.fakeScanner->scanCalls() == 1U);
  CHECK(fixture.fakeScanner->startWatchingCalls() == 1U);
  CHECK(fixture.fakeScanner->stopWatchingCalls() >= 1U);
  CHECK_FALSE(fixture.fakeScanner->lastWatchingRoots().has_value());
  CHECK(error.str().find("failed to scan library") != std::string::npos);
  CHECK(error.str().find("terminal watcher start failed") != std::string::npos);
}
