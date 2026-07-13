#include "Text.hpp"

#include <folly/Conv.h>
#include <folly/String.h>
#include <folly/futures/Future.h>
#include <folly/init/Init.h>

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
  folly::Init init(&argc, &argv);

  const std::string name = argc > 1 ? argv[1] : "developer";
  std::cout << helloworld::greeting(name) << '\n';

  std::vector<std::string> technologies;
  folly::split(',', "C++,Folly,CMake", technologies);
  std::cout << "Tools: " << helloworld::formatTools(technologies) << '\n';

  const int answer = folly::to<int>("42");
  auto doubled = folly::makeFuture(answer).thenValue([](int value) {
    return value * 2;
  });
  std::cout << "Future result: " << std::move(doubled).get() << '\n';

  return 0;
}
