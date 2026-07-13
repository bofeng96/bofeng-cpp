#include "Text.hpp"

#include <folly/Format.h>
#include <folly/String.h>

namespace helloworld {

std::string greeting(std::string_view name) {
  return folly::sformat("Hello, {}! Welcome to Folly.", name);
}

std::string formatTools(const std::vector<std::string>& tools) {
  return folly::join(" | ", tools);
}

}  // namespace helloworld
