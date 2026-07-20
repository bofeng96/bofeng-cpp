#include "TaskApplication.hpp"

#include "TaskConfig.hpp"
#include "TaskExample.hpp"

#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/init/Init.h>

#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

namespace follytask {

int TaskApplication::run(int argc, char* argv[]) const {
   folly::Init init(&argc, &argv);

   const std::string name = argc > 1 ? argv[1] : "developer";

   const auto config = loadTaskConfig();
   if (!config) {
      std::cerr << "folly-task: " << config.error() << '\n';
      return 2;
   }

   // Folly Init parses the gflag before its value is used to create the pool.
   folly::CPUThreadPoolExecutor executor{config->executorThreads};
   std::cout << "Application is running on main thread "
      << std::this_thread::get_id() << "; executor has "
      << config->executorThreads << " worker threads.\n";

   // The service remains alive until its Task and child Tasks have completed.
   const TaskExample example;

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

}  // namespace follytask
