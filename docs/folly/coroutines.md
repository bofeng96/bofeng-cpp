# Coroutines and `Task`

[← Folly guide index](README.md)

## Coroutines: direct-style asynchronous composition

Folly coroutines use C++ language suspension while preserving Folly executor,
cancellation, request-context, and async-stack integration. The syntax looks
sequential, but execution remains lazy, executor-driven, and cooperative.

### The task family: which type to use

Modern Folly contains both the established movable `Task` API and newer task
wrappers designed to prevent coroutine lifetime bugs.

| Type | Meaning and appropriate use |
| --- | --- |
| `Task<T>` | Established move-only, single-use, lazy task. It can be stored and moved before being awaited, which is flexible but can extend borrowed data beyond its lifetime. It remains important for existing APIs and interoperability. |
| `TaskWithExecutor<T>` | A `Task` after explicit executor binding. It is still lazy, but may be awaited from anywhere or eagerly started. Usually create it with `co_withExecutor`, not as a public return type. |
| `now_task<T>` (`NowTask<T>`) | Newer immovable task that must be awaited in the full expression that creates it. Where supported, Folly's header recommends it as the safer default because C++ lifetime extension protects many temporary/reference cases. |
| `safe_task<Safety, T>` | Movable wrapper with compile-time alias/lifetime checks. Most users select aliases such as `value_task`, `member_task`, `closure_task`, or `auto_safe_task` rather than spelling the safety level. |

The safe-task family is evolving and compiler/configuration dependent. Pin the
Folly version before making it a public API. For learning Folly and integrating
with established code, understand `Task` first; for new production coroutine
APIs, evaluate `now_task` and the safe aliases rather than defaulting to a
movable task.

### A progressive `Task` tutorial

This tutorial follows one operation from definition to production-style
execution. The examples use movable `Task` because it is the established API
and remains common in Folly interfaces. The same ownership, executor, and
cancellation questions still apply when adopting safer task variants.

#### Step 1: define one lazy operation

```cpp
#include <folly/coro/Sleep.h>
#include <folly/coro/Task.h>

#include <chrono>

using namespace std::chrono_literals;

folly::coro::Task<int> loadAnswer() {
  co_await folly::coro::sleep(10ms);
  co_return 42;
}
```

Calling `loadAnswer()` creates one task but does not run its body:

```cpp
auto task = loadAnswer();  // suspended; the timer has not started
```

Treat the coroutine function as a reusable factory and each returned `Task` as
one single-use invocation. To execute the operation again, call the function
again; never try to await the same task twice.

#### Step 2: compose child tasks

```cpp
#include <folly/Format.h>

folly::coro::Task<std::string> makeMessage(std::string name) {
  const int answer = co_await loadAnswer();
  co_return folly::sformat("Hello, {}: {}", name, answer);
}
```

`loadAnswer()` is a temporary task, so `co_await` consumes it directly. If a
task has first been stored in a named variable, move it at the consuming point:

```cpp
auto child = loadAnswer();
const int answer = co_await std::move(child);
// child is consumed and must not be used again
```

Take ordinary inputs by value when the coroutine should own them. A reference
parameter remains borrowed even though the coroutine is suspended:

```cpp
folly::coro::Task<void> unsafeIfCallerDies(const Request& request);
folly::coro::Task<void> ownsItsInput(Request request);
```

#### Step 3: repeat sequentially or concurrently

For sequential repetition, create and await a fresh task on each iteration:

```cpp
folly::coro::Task<std::vector<int>> loadSequentially(std::size_t count) {
  std::vector<int> results;
  results.reserve(count);

  for (std::size_t i = 0; i < count; ++i) {
    results.push_back(co_await loadAnswer());
  }

  co_return results;
}
```

For concurrent repetition, create fresh tasks and give ownership of the entire
collection to a collection combinator:

```cpp
#include <folly/coro/Collect.h>

folly::coro::Task<std::vector<int>> loadConcurrently(std::size_t count) {
  std::vector<folly::coro::Task<int>> tasks;
  tasks.reserve(count);

  for (std::size_t i = 0; i < count; ++i) {
    tasks.push_back(loadAnswer());
  }

  co_return co_await folly::coro::collectAllRange(std::move(tasks));
}
```

Use `collectAllWindowed` rather than creating an unbounded number of active
operations when `count` can be large or the downstream dependency has a
concurrency limit.

#### Step 4: enter from synchronous code

A root task has no parent from which to inherit an executor. Bind it to an
executor, start it, and consume the returned future at the synchronous edge:

```cpp
#include <folly/executors/CPUThreadPoolExecutor.h>

folly::CPUThreadPoolExecutor executor{4};
auto keepAlive = folly::Executor::getKeepAliveToken(executor);

auto task = makeMessage("Bofeng");
auto bound = folly::coro::co_withExecutor(keepAlive, std::move(task));
auto future = std::move(bound).start();

std::string message = std::move(future).get();
```

The ownership flow is:

```text
fresh Task
   --co_withExecutor--> TaskWithExecutor
   --start-----------> running operation + SemiFuture
   --get-------------> value or rethrown exception
```

`get()` blocks the current thread. Use it only at a top-level synchronous
boundary, never inside an event-loop callback or another coroutine. A small
unit test can instead use `blockingWait(makeMessage("Bofeng"))` without
creating a pool explicitly.

#### Step 5: handle failures with normal control flow

```cpp
folly::coro::Task<Response> loadWithFallback() {
  try {
    co_return co_await fetchPrimary();
  } catch (const TemporaryError&) {
    co_return co_await fetchBackup();
  }
}
```

An exception is captured in the child task and rethrown at its consuming
`co_await`. Catch it where recovery is meaningful. Use `co_awaitTry` or
`collectAllTry` when the exception should instead be inspected as data.

#### Step 6: add a deadline and cancellation policy

```cpp
#include <folly/coro/Timeout.h>

folly::coro::Task<Response> fetchBeforeDeadline() {
  using namespace std::chrono_literals;
  co_return co_await folly::coro::timeout(fetchPrimary(), 500ms);
}
```

When the deadline wins, `timeout` requests cancellation and reports
`folly::FutureTimeout`. The child must cooperate with cancellation; a timeout
cannot forcibly terminate blocking code. At a service boundary, decide whether
the timeout is retried, translated to a domain error, or allowed to propagate.

#### Step 7: preserve owner lifetime

A non-static member coroutine borrows its object through `this`:

```cpp
Service service;
auto task = service.fetch();
// service must remain alive until task completes
```

Prefer structured nesting: await the task before the owner leaves scope, or
place dynamically launched children in an `AsyncScope` and join it. Capturing
`shared_ptr<Service>` is appropriate only when shared lifetime is genuinely the
desired ownership model; it should not be an automatic substitute for joining.

#### Step 8: test the asynchronous behavior

```cpp
TEST(MessageTest, BuildsMessage) {
  EXPECT_EQ(
      folly::coro::blockingWait(makeMessage("Bofeng")),
      "Hello, Bofeng: 42");
}
```

Keep unit tests fast and deterministic. Inject zero delays, fake dependencies,
or controllable executors rather than sleeping in real time. Add separate tests
for success, child failure, timeout, cancellation, and owner shutdown.

### `Task<T>` semantics

`folly::coro::Task<T>` is a lazy description of one computation that will
eventually produce a value or exception. It is neither a thread nor a reusable
job object.

```cpp
folly::coro::Task<Response> fetch(Request request) {
  auto bytes = co_await read(std::move(request));
  co_return decode(bytes);
}
```

Calling `fetch()` allocates/creates its coroutine frame and captures its
parameters, then initially suspends before executing the body. Value parameters
live in the frame; reference parameters remain borrowed references.

```text
call coroutine function
        |
        v
create frame + capture parameters + initial suspend
        |
        | co_await / bind-and-start / blockingWait
        v
run until completion or next incomplete co_await
        |
        | incomplete awaitable
        v
suspend frame; executor thread runs other work
        |
        v
resume on bound executor -> co_return or exception
        |
        v
resume awaiting coroutine / complete future -> destroy frame when owner releases it
```

`Task<void>` completes without a C++ value. At Future boundaries Folly uses
`folly::Unit` where a concrete value type is required.

### `co_await`, suspension, and executor stickiness

`co_await child()` does three conceptual things:

1. It evaluates and starts the lazy child task.
2. It suspends the parent if the child cannot complete immediately.
3. It resumes the parent with the child's value or rethrows its exception.

An unbound child inherits the awaiting Folly task's executor. Folly arranges
for the task to resume on that bound executor after asynchronous awaits, even
when the awaited operation completes elsewhere. This is executor stickiness,
not worker-thread stickiness: a CPU pool may select a different worker.

```cpp
folly::coro::Task<void> parent(folly::Executor::KeepAlive<> other) {
  auto* original = co_await folly::coro::co_current_executor;
  co_await child();  // child inherits original
  co_await folly::coro::co_withExecutor(other, child());
  // Parent resumes on original after the explicitly rebound child completes.
}
```

Use `co_current_executor` to inspect the current executor, not to infer a
specific thread identity.

### Starting a root task

A task awaited by another task gets its executor from the parent. A root task
entered from synchronous or callback code needs an explicit boundary.

| Boundary | Behavior |
| --- | --- |
| `co_await task` | Starts a child lazily and suspends the parent until it completes. |
| `co_withExecutor(executor, task)` | Consumes the task, binds it, and returns a still-lazy `TaskWithExecutor`. |
| `std::move(boundTask).start()` | Schedules eager execution and returns `SemiFuture<T>`. An overload accepts a completion callback and optional cancellation token. |
| `std::move(task).semi()` | Bridges a task into the Future ecosystem; the resulting `SemiFuture` is activated when attached/driven. |
| `blockingWait(awaitable)` | Drives an awaitable and blocks the calling thread. Reserve it for tests and top-level synchronous edges. |

```cpp
auto bound = folly::coro::co_withExecutor(keepAlive, fetch(request));
folly::SemiFuture<Response> result = std::move(bound).start();
Response response = std::move(result).get();  // synchronous edge only
```

`scheduleOn()` is the older spelling for binding a task and is deprecated in
recent Folly releases; prefer `co_withExecutor()` where available.

### Sequential versus concurrent composition

Creating several lazy tasks does not start them. Awaiting them one after
another therefore executes them sequentially:

```cpp
auto a = fetchA();
auto b = fetchB();
auto va = co_await std::move(a);
auto vb = co_await std::move(b);  // starts only after a completes
```

Use a collection primitive to express concurrency:

```cpp
auto [va, vb] = co_await folly::coro::collectAll(fetchA(), fetchB());
```

Collection starts each input in argument order until it first suspends. Tasks
that complete synchronously can consequently still run sequentially on the
current thread.

| Combinator | Completion and failure semantics |
| --- | --- |
| `collectAll` | Waits for all. A failure requests cancellation of outstanding work, waits for completion, discards partial values, and rethrows one failure. |
| `collectAllTry` | Waits for all and returns `Try<T>` for each input. A child failure does not request cancellation of the other children. |
| `collectAllRange` / `collectAllTryRange` | Range/vector forms that preserve input order. Tasks stored in containers must be moved into the operation. |
| `collectAllWindowed` | Processes a range with a maximum concurrency, providing an essential backpressure control. |
| `collectAny` | Returns the index and `Try` of the first completion, requests cancellation of the remainder, and waits for them to complete before returning. |
| `collectAnyWithoutException` | Returns the first success, or the final failure if every input fails. |
| `collectAnyNoDiscard` | Cancels after the first completion but retains every final result, including cancellation outcomes. |

“First result” does not imply abandoned work. The normal combinators account
for remaining operations before returning. If cancellation is not supported,
the wait can still last as long as the slowest child.

### Structured dynamic concurrency

`collectAll` is ideal when the child set is known at one expression.
`AsyncScope` supports a dynamic number of concurrent `void`/`Unit` operations:

```cpp
folly::coro::AsyncScope scope;
scope.add(folly::coro::co_withExecutor(executor, process(item1)));
scope.add(folly::coro::co_withExecutor(executor, process(item2)));
co_await scope.joinAsync();
```

Key rules:

- An unbound `Task` cannot be passed directly to `AsyncScope::add`; bind its
  executor first.
- Added work must handle its errors according to the scope's policy and cannot
  return a value that would be silently discarded.
- After adding work, complete `joinAsync()` or `cleanup()` before destroying
  the scope.
- Do not add new unrelated work once joining/cleanup can complete.

`CancellableAsyncScope` attaches a cancellation token to added work. It offers
`requestCancellation()`, `joinAsync()`, and `cancelAndJoinAsync()`. Cancellation
still depends on every child cooperating.

Structured concurrency means the parent scope cannot finish while children
that borrow its state are still running. Prefer this shape over fire-and-forget
execution.

### Cancellation and timeouts

`CancellationSource` owns the ability to request cancellation. Copies of its
`CancellationToken` are passed to consumers. `CancellationCallback` adapts a
token to an underlying callback/cancel API.

Within a Folly task:

```cpp
const auto& token =
    co_await folly::coro::co_current_cancellation_token;
if (token.isCancellationRequested()) {
  co_yield folly::coro::co_stopped_may_throw;
}
```

`co_withCancellation(token, awaitable)` injects a token only when the awaitable
supports the customization; the generic fallback cannot magically make an
uncancellable operation cancellable.

Cancellation is cooperative and races with normal completion:

- requesting cancellation does not stop an OS thread;
- an operation must poll the token or register a callback;
- either completion or cancellation may win;
- resource cleanup and joining remain mandatory.

`coro::timeout(operation, duration)` starts a timer, requests cancellation if
the timer wins, waits for the child to respond, and reports `FutureTimeout`.
Its latency therefore depends on cancellation responsiveness.
`timeoutNoDiscard` preserves the child's eventual result rather than replacing
it with the timeout result. `timed_wait` and `detachOnCancel` have different
discard/detachment semantics and require a careful lifetime audit before use.

### Errors and result transport

An exception thrown from a task is stored in its coroutine state and rethrown
by `co_await`, `SemiFuture::get()`, or `blockingWait()`. Normal `try`/`catch`
works naturally around `co_await`.

- `co_awaitTry(awaitable)` captures completion as `Try<T>` so the caller can
  inspect a value or exception without immediately throwing.
- `collectAllTry` applies that idea to concurrent children.
- `result<T>` and `or_unwind` support value/error/stopped propagation without
  making every expected failure take the throw/catch path.
- `co_result` and `co_error` are lower-level completion adapters used by Folly
  result-aware coroutine machinery.

Choose one error vocabulary at an API boundary and document it. Mixing domain
errors, exceptions, and stopped cancellation without a policy makes callers
handle the same failure multiple ways.

### Coroutine synchronization

Thread-blocking primitives park an OS thread; coroutine primitives suspend only
the current coroutine. Major `folly::coro` building blocks include:

| Primitive | Use |
| --- | --- |
| `Baton` | One-shot notification from another coroutine/thread. |
| `Mutex` / `SharedMutex` | Coroutine-aware exclusive/shared critical sections. |
| `SharedLock` | RAII-style shared ownership of a coroutine lock. |
| `Promise` / `SharedPromise` | One producer completing one or multiple coroutine consumers. |
| `BoundedQueue`, `UnboundedQueue`, `SmallUnboundedQueue` | Async producer/consumer handoff with different capacity/allocation trade-offs. |
| `SerialQueueRunner` | Serialize asynchronous operations without dedicating a thread. |

Never hold `std::mutex`, `Synchronized::LockedPtr`, or another thread lock
across `co_await`. The coroutine may suspend indefinitely while retaining the
lock, and resumption may occur on another worker.

### Streams, generators, and channels

`AsyncGenerator<Reference>` represents a pull-based asynchronous sequence. A
producer uses `co_yield`; a consumer awaits `next()`. Each request for the next
item establishes executor affinity for that production step.

Generators require explicit ownership thinking:

- yielded references must remain valid until the consumer is finished with
  that item;
- a consumer may stop before draining the generator;
- cleanable generators require their asynchronous `cleanup()` to be awaited.

`AsyncPipe` and coroutine queues provide point-to-point streaming. The separate
`folly::channels` library adds sender/receiver channels, transforms, merge,
fan-out, multiplexing, processors, and rate limiting. Choose bounded channels
or explicit concurrency windows when producers can outrun consumers.

### Lifetime hazards in movable `Task`

A movable lazy task can start much later than the expression that created it.
Memorize these hazards:

1. A reference parameter remains a reference in the coroutine frame; its
   referent must survive until the task finishes.
2. A non-static member coroutine retains `this`; the object must outlive all
   awaits and children.
3. Invoking a temporary coroutine lambda can leave the task referring to a
   destroyed closure. Immediately await it, use `co_invoke`, or use the safer
   closure/task facilities.
4. Views, iterators, spans, and `StringPiece`s do not become owning merely
   because they were captured in a frame.
5. Detached work must own everything it touches and must have an explicit
   error, cancellation, and shutdown policy.
6. Destroying a never-started task destroys the frame without executing the
   body.

This is the motivation for `now_task`: preventing a task from being moved away
from the full expression lets normal C++ temporary-lifetime extension protect
many borrowed inputs. The safe-task aliases add compile-time constraints when
a task genuinely must be movable or scheduled into another scope.

### Testing tasks

For a small synchronous unit test, `blockingWait(std::move(task))` is clear and
appropriate. Folly also provides coroutine-aware GoogleTest helpers (`CO_TEST`
families) for tests that should themselves be coroutines. Use zero-duration or
controllable time sources instead of sleeping in unit tests, and use manual or
drivable executors when scheduling order is part of the behavior under test.

## Worked design: an asynchronous aggregation pipeline

Consider a request handler that loads two independent resources, enriches a
variable number of results, and returns a response under a deadline. This
small design exercises the relationships that matter in a real Folly service:

```text
incoming request
      |
      v
root Task bound to an I/O executor
      |
      +---- collectAll ---- load primary data
      |                 \\- load recommendations
      |
      v
bounded concurrent enrichment
      |
      v
assemble response in owned IOBufs
      |
      v
write through AsyncTransport -> complete request scope

deadline or shutdown
      -> request cancellation
      -> children clean up
      -> parent joins them
      -> release request state
```

The outer coroutine owns the request value and composes independently useful
operations concurrently:

```cpp
folly::coro::Task<Page> buildPage(Request request) {
  auto [profile, recommendations] =
      co_await folly::coro::collectAll(
          loadProfile(request.userId),
          loadRecommendations(request.userId));

  auto cards = co_await enrichWithConcurrencyLimit(
      std::move(recommendations), 16);

  co_return Page{
      .profile = std::move(profile),
      .cards = std::move(cards),
  };
}
```

This design makes several policies explicit:

| Question | Design choice |
| --- | --- |
| Who owns request data? | The outer coroutine takes `Request` by value, so suspended work does not borrow a caller's temporary. |
| Which work is concurrent? | Only independent loads and explicitly windowed enrichment operations. |
| Where does work resume? | Child tasks inherit the parent executor unless explicitly rebound. |
| What limits fan-out? | The enrichment stage has a fixed concurrency window rather than launching one active operation per item. |
| How do failures propagate? | `collectAll` cancels outstanding siblings, waits for them, and rethrows a failure. Use `collectAllTry` if partial results are part of the contract. |
| What happens on timeout? | The request boundary asks the task tree to cancel and does not release request-owned state until children finish. |
| Where may blocking occur? | Nowhere on the I/O path; CPU-heavy transformation is explicitly moved to a CPU executor. |

Apply the deadline at the boundary that owns the user-visible latency policy:

```cpp
folly::coro::Task<Page> serve(Request request) {
  using namespace std::chrono_literals;

  try {
    co_return co_await folly::coro::timeout(
        buildPage(std::move(request)), 750ms);
  } catch (const folly::FutureTimeout&) {
    throw ServiceUnavailable{"request deadline exceeded"};
  }
}
```

The timeout is useful only if leaf operations cooperate. Socket operations,
queues, timers, and nested tasks should observe the inherited cancellation
token or adapt it to their underlying cancellation mechanism. A blocking
third-party call that ignores cancellation must run on an appropriate executor
and may continue after the caller's deadline; capacity planning must account
for that behavior.

If response serialization is significant CPU work, make the executor switch
visible instead of performing it on the event-loop thread:

```cpp
auto response = co_await folly::coro::co_withExecutor(
    cpuExecutor, encodePage(std::move(page)));
```

The complete request scope must retain the service objects, executor keep-alive
tokens, buffers, and transport callbacks used by its children. During shutdown,
stop admitting requests, request cancellation, join outstanding scopes, close
transports, flush observability output, and only then destroy executors and
dependencies.

Useful exercises for understanding the design are:

1. Change the independent loads to sequential awaits and measure latency.
2. Replace `collectAll` with `collectAllTry` and define a partial-response
   policy.
3. Vary the enrichment window and measure throughput, tail latency, queueing,
   and downstream load.
4. Inject cancellation during every await point and verify cleanup and joining.
5. Move an expensive transformation between the I/O and CPU executors and
   observe event-loop delay.
6. Represent the response as an `IOBuf` chain, then measure the cost of
   unnecessary coalescing.
7. Test shutdown with requests active and verify that no callback observes a
   destroyed owner.

