#pragma once

#include <folly/coro/Task.h>

#include <chrono>
#include <string>

namespace follytask {

class TaskExample {
public:
   // The configurable delay keeps production behavior visible while allowing
   // tests to construct an instance with no delay.
   explicit TaskExample(
      std::chrono::milliseconds delay = std::chrono::milliseconds{100});

   // The Task retains access to this object, so the TaskExample instance must
   // remain alive until the returned Task has completed.
   [[nodiscard]] folly::coro::Task<std::string> makeMessage(
      std::string name) const;

private:
   [[nodiscard]] folly::coro::Task<int> loadAnswer() const;
   std::chrono::milliseconds delay_;
};

}  // namespace follytask
