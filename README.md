# bofeng-cpp

`bofeng-cpp` is a C++23 project for building small applications and testing
APIs from [Meta Folly](https://github.com/facebook/folly) and
[Abseil](https://github.com/abseil/abseil-cpp). 

## Applications

| Target | Description |
| --- | --- |
| `hello-world` | Demonstrates Folly formatting, string utilities, conversion, and futures. |
| `folly-task` | Introduces `folly::coro::Task`, coroutine composition, CPU executors, and eager task execution. |

## Requirements

- CMake 3.24 or newer
- A compiler with C++23 support
- Folly and its dependencies
- Abseil when building Abseil examples
- GoogleTest when tests are enabled

### macOS with Homebrew

Install the Xcode Command Line Tools if they are not already available:

```sh
xcode-select --install
```

Install CMake, Folly, Abseil, and GoogleTest:

```sh
brew install cmake folly abseil googletest
```

## Configure and build

From the repository root:

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build --parallel
```

`CMAKE_PREFIX_PATH` tells CMake where Homebrew installed external packages such
as Folly and Abseil. When they are installed somewhere else, replace
`$(brew --prefix)` with the appropriate installation prefix.

## Run the examples

Both applications accept an optional name:

```sh
./build/apps/hello-world/hello-world Bofeng
./build/apps/folly-task/folly-task --executor_threads=4 Bofeng
```

Without a name, they use `developer`. The `folly-task` executor uses two worker
threads by default; set `--executor_threads=N` to choose `2`, `4`, or `6`.

## Run the tests

```sh
ctest --test-dir build --output-on-failure
```

Testing is enabled by default. To configure without tests, pass
`-DBUILD_TESTING=OFF` to CMake.

## Use CLion

Open the repository root as a CMake project and reload CMake. If CLion cannot
find Folly or Abseil, add the following to the active CMake profile's options:

```text
-DCMAKE_PREFIX_PATH=/opt/homebrew
```

`/opt/homebrew` is the usual Apple Silicon Homebrew prefix. Run `brew --prefix`
to find the correct prefix for the current Mac.

Generated directories such as `build/` and `cmake-build-*/` are ignored and
should not be committed.
