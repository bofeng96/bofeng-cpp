#include "TaskExample.hpp"

#include <folly/Format.h>
#include <folly/coro/Sleep.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

namespace follytask {

TaskExample::TaskExample(const std::chrono::milliseconds delay)
   : delay_{delay} {}

folly::coro::Task<int> TaskExample::loadAnswer() const {
   co_await folly::coro::sleep(delay_);
   co_return 42;
}

folly::coro::Task<std::string> TaskExample::makeMessage(
   std::string name) const {
   std::cout << "Started on worker thread " << std::this_thread::get_id()
      << ".\n";

   // A child Task inherits the executor of the coroutine that awaits it.
   const int answer = co_await loadAnswer();

   std::cout << "Resumed on worker thread " << std::this_thread::get_id()
      << ".\n";
   co_return folly::sformat(
      "Hello, {}. The answer is {}.", std::move(name), answer);
}

}  // namespace follytask
