#include "TaskExample.hpp"

#include <folly/coro/BlockingWait.h>

#include <chrono>
#include <exception>
#include <iostream>
#include <string>
#include <utility>

namespace {

bool expectEqual(
    const std::string& actual,
    const std::string& expected,
    const std::string& testName) {
  if (actual == expected) {
    return true;
  }

  std::cerr << testName << " failed\n"
            << "  expected: " << expected << '\n'
            << "  actual:   " << actual << '\n';
  return false;
}

}  // namespace

int main() {
  try {
    const follytask::TaskExample example{std::chrono::milliseconds{0}};
    auto messageTask = example.makeMessage("Bofeng");
    const std::string message =
        folly::coro::blockingWait(std::move(messageTask));

    return expectEqual(
               message,
               "Hello, Bofeng. The answer is 42.",
               "makeMessage")
        ? 0
        : 1;
  } catch (const std::exception& error) {
    std::cerr << "makeMessage threw: " << error.what() << '\n';
    return 1;
  }
}
