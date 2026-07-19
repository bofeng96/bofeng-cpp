#include "TaskExample.hpp"

#include <folly/coro/BlockingWait.h>
#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <utility>

TEST(TaskExampleTest, BuildsMessage) {
  const follytask::TaskExample example{std::chrono::milliseconds{0}};
  auto messageTask = example.makeMessage("Bofeng");

  const std::string message =
      folly::coro::blockingWait(std::move(messageTask));

  EXPECT_EQ(message, "Hello, Bofeng. The answer is 42.");
}
