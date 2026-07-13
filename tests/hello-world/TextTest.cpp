#include "Text.hpp"

#include <iostream>
#include <string>
#include <vector>

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
  bool passed = true;

  passed &= expectEqual(
      helloworld::greeting("Bofeng"),
      "Hello, Bofeng! Welcome to Folly.",
      "greeting");

  passed &= expectEqual(
      helloworld::formatTools({"C++", "Folly", "CMake"}),
      "C++ | Folly | CMake",
      "formatTools");

  return passed ? 0 : 1;
}
