#pragma once

#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace doctest {

struct TestCaseData {
  std::string_view name;
  void (*func)();
};

inline std::vector<TestCaseData>& registry() {
  static std::vector<TestCaseData> tests;
  return tests;
}

inline int& failure_count() {
  static int count = 0;
  return count;
}

inline void register_test(std::string_view name, void (*func)()) {
  registry().push_back(TestCaseData{name, func});
}

inline void check(bool condition, const char* expression, const char* file, int line) {
  if(!condition) {
    ++failure_count();
    std::cerr << file << ':' << line << " CHECK( " << expression << " ) failed\n";
  }
}

struct TestRegistrar {
  TestRegistrar(std::string_view name, void (*func)()) {
    register_test(name, func);
  }
};

}

#define DOCTEST_CONCAT_INNER(left, right) left##right
#define DOCTEST_CONCAT(left, right) DOCTEST_CONCAT_INNER(left, right)
#define DOCTEST_TEST_CASE_IMPL(name, line) \
  static void DOCTEST_CONCAT(doctest_test_, line)(); \
  static ::doctest::TestRegistrar DOCTEST_CONCAT(doctest_registrar_, line){name, &DOCTEST_CONCAT(doctest_test_, line)}; \
  static void DOCTEST_CONCAT(doctest_test_, line)()

#define TEST_CASE(name) DOCTEST_TEST_CASE_IMPL(name, __LINE__)
#define CHECK(expression) ::doctest::check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)
#define REQUIRE(expression) CHECK(expression)

#ifdef DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
int main() {
  for(const auto& test_case : ::doctest::registry()) {
    try {
      test_case.func();
    } catch(...) {
      ::doctest::check(false, "unexpected exception", __FILE__, __LINE__);
    }
  }

  if(::doctest::failure_count() != 0) {
    return EXIT_FAILURE;
  }

  std::cout << ::doctest::registry().size() << " test(s) passed\n";
  return EXIT_SUCCESS;
}
#endif
