#include <doctest/doctest.h>

#include <BS_thread_pool.hpp>

#include <algorithm>
#include <stdexcept>
#include <vector>

TEST_CASE("scanner thread pool basic integration constructs and returns submitted values") {
  BS::thread_pool pool{2};

  auto first = pool.submit_task([] { return 21; });
  auto second = pool.submit_task([] { return 34; });
  auto third = pool.submit_task([] { return 55; });

  std::vector<int> values{first.get(), second.get(), third.get()};
  std::ranges::sort(values);

  CHECK(pool.get_thread_count() == 2);
  CHECK(values == std::vector<int>{21, 34, 55});
}

TEST_CASE("scanner thread pool basic integration propagates future exceptions") {
  BS::thread_pool pool{1};

  auto failure = pool.submit_task([]() -> int { throw std::runtime_error{"scanner thread pool failure"}; });

  CHECK_THROWS_WITH_AS(failure.get(), "scanner thread pool failure", std::runtime_error);
}
