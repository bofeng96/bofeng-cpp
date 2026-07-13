#include "TaskExample.hpp"

#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/init/Init.h>
#include <gflags/gflags.h>

#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

DEFINE_uint32(
    executor_threads,
    2,
    "Number of worker threads in the CPU executor (must be greater than zero)");

int main(int argc, char* argv[]) {
  folly::Init init(&argc, &argv);

  const std::string name = argc > 1 ? argv[1] : "developer";

  if (FLAGS_executor_threads == 0) {
    std::cerr << "folly-task: --executor_threads must be greater than zero\n";
    return 2;
  }

  // Folly Init parses the gflag before its value is used to create the pool.
  folly::CPUThreadPoolExecutor executor{FLAGS_executor_threads};
  std::cout << "main() is running on thread " << std::this_thread::get_id()
      << "; executor has " << FLAGS_executor_threads << " worker threads.\n";

  // The service remains alive until its Task and child Tasks have completed.
  const follytask::TaskExample example;

  // Constructing and binding a Task still does not start it.
  auto messageTask = example.makeMessage(name);
  auto scheduledTask = folly::coro::co_withExecutor(
      folly::Executor::getKeepAliveToken(executor), std::move(messageTask));
  std::cout << "Task created and bound to the CPU executor.\n";

  try {
    // start() schedules eager execution and returns immediately with a
    // SemiFuture. get() is the synchronous boundary that waits for the result.
    auto messageFuture = std::move(scheduledTask).start();
    const std::string message = std::move(messageFuture).get();
    std::cout << message << '\n';
  } catch (const std::exception& error) {
    std::cerr << "folly-task: " << error.what() << '\n';
    return 1;
  }

  return 0;
}
