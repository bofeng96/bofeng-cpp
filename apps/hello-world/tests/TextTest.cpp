#include "Text.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

TEST(TextTest, BuildsGreeting) {
  EXPECT_EQ(
      helloworld::greeting("Bofeng"),
      "Hello, Bofeng! Welcome to Folly.");
}

TEST(TextTest, FormatsTools) {
  const std::vector<std::string> tools{"C++", "Folly", "CMake"};
  EXPECT_EQ(helloworld::formatTools(tools), "C++ | Folly | CMake");
}
