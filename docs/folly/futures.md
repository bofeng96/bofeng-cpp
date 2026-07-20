# Futures and promises

[← Folly guide index](README.md)

## Contents

- [The Future model](#the-future-model)
- [A progressive Future tutorial](#a-progressive-future-tutorial)
  - [Create and fulfill a result](#step-1-create-and-fulfill-a-result)
  - [Adapt a callback API](#step-2-adapt-a-callback-api)
  - [Choose an executor](#step-3-return-semifuture-then-choose-an-executor)
  - [Transform values](#step-4-transform-values-with-thenvalue)
  - [Move Future chains](#step-5-understand-why-chains-use-stdmove)
  - [Handle errors and cleanup](#step-6-handle-errors-and-cleanup)
  - [Combine independent Futures](#step-7-combine-independent-futures)
  - [Add deadlines](#step-8-add-deadlines-deliberately)
  - [Support interruption](#step-9-support-interruption-when-the-producer-can-cancel)
  - [Share one result](#step-10-share-one-result-with-multiple-consumers)
  - [Use synchronous boundaries safely](#step-11-block-only-at-a-synchronous-boundary)
- [Executor and thread rules](#executor-and-thread-rules)
- [Ownership and lifetime checklist](#ownership-and-lifetime-checklist)
- [Future and coroutine interoperability](#future-and-coroutine-interoperability)
- [Testing Future code](#testing-future-code)
- [Futures versus coroutines](#futures-versus-coroutines)
- [Common mistakes](#common-mistakes)
- [Further reading](#further-reading)

## The Future model

Folly Futures express one asynchronous result and a continuation graph around
it. They are especially useful for adapting callback APIs, composing existing
Future-based systems, and building pipelines whose stages naturally form a
graph.

```text
producer owns Promise<T>
       | setValue / setException / setTry
       v
single shared result state
       ^
consumer owns SemiFuture<T> --via(executor)--> Future<T>
                                                |
                                                v
                                      thenValue / thenTry / thenError
```

The four types to learn first are:

| Type | Role |
| --- | --- |
| `Promise<T>` | The producer handle. It completes the shared state once with a value or exception. |
| `SemiFuture<T>` | A move-only, single-consumer result without a fixed continuation executor. It is the preferred library return type. |
| `Future<T>` | A move-only, single-consumer result associated with an executor for continuation scheduling. |
| `Try<T>` | A value-or-exception carrier used when a callback needs to inspect either outcome. |

`Future<T>` does not mean a new thread, and it does not make blocking work
asynchronous. The producer must arrange the work using an event loop, executor,
device, callback API, or another asynchronous mechanism.

Unlike `coro::Task`, a Future is not generally a lazy recipe for restarting an
operation. A Future-returning function commonly initiates its producer before
returning the handle. `SemiFuture` primarily means that continuation execution
policy has not yet been fixed; it does not universally mean that the producer
is dormant. Deferred continuations are activated later, but an underlying
socket, timer, or callback operation may already be running. Each producer API
must document its start semantics.

## A progressive Future tutorial

### Step 1: create and fulfill a result

```cpp
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>

folly::Promise<int> promise;
folly::SemiFuture<int> result = promise.getSemiFuture();

promise.setValue(42);
```

The promise and semi-future are two handles to one shared state. The producer
may complete it exactly once:

```cpp
promise.setValue(42);
promise.setValue(43); // Throws: the Promise was already fulfilled.
```

An asynchronous failure completes the same state with an exception:

```cpp
promise.setException(NetworkError{"connection failed"});
```

`setTry()` is convenient when a callback already reports `Try<T>`:

```cpp
promise.setTry(std::move(outcome));
```

Call `getSemiFuture()` once on an ordinary `Promise`. If a promise is destroyed
without being fulfilled, its consumer observes a broken-promise exception.

For an already available result, avoid constructing a promise manually:

```cpp
auto ready = folly::makeSemiFuture<int>(42);
auto failed = folly::makeSemiFuture<int>(
   folly::make_exception_wrapper<NetworkError>("connection failed"));
```

### Step 2: adapt a callback API

Suppose a client reports completion through a move-capable callback:

```cpp
class Client {
public:
   void fetch(
      Request request,
      folly::Function<void(folly::Try<Response>&&)> callback);
};
```

Wrap it by moving a promise into the callback and returning its consumer side:

```cpp
folly::SemiFuture<Response> fetchAsync(
   Client& client,
   Request request) {
   folly::Promise<Response> promise;
   auto result = promise.getSemiFuture();

   client.fetch(
      std::move(request),
      [promise = std::move(promise)](
         folly::Try<Response>&& outcome) mutable {
         promise.setTry(std::move(outcome));
      });

   return result;
}
```

The adapter's correctness depends on a few contracts:

- the callback is invoked exactly once;
- every terminal path invokes it, including setup failure and shutdown;
- the captured promise survives until callback completion;
- the client survives as long as required by its own callback contract;
- synchronous callback invocation is allowed and does not invalidate local
  assumptions;
- the producer documents whether interruption is supported.

If the callback can be invoked more than once, put a separate exactly-once gate
in front of the promise. Do not catch a double completion after the fact as a
normal control-flow technique.

### Step 3: return `SemiFuture`, then choose an executor

An asynchronous library should normally return `SemiFuture<T>`:

```cpp
folly::SemiFuture<Response> fetchAsync(Client&, Request);
```

The caller chooses where subsequent work executes:

```cpp
auto future = fetchAsync(client, std::move(request)).via(executorKeepAlive);
```

`.via()` consumes the `SemiFuture` and produces `Future<T>`. A keep-alive token
ensures that the executor remains valid while the chain still depends on it.

This division is intentional:

```text
library: describes eventual completion with SemiFuture<T>
caller:  selects execution policy with via(executor)
chain:   composes executor-bound work with Future<T>
```

`SemiFuture` also supports deferred transformations such as `defer`,
`deferValue`, `deferError`, and `deferEnsure`. They remain executor-neutral until
the operation is activated by `.via()`, a blocking boundary, or another
consumer. Prefer them when a library must transform a result without choosing
the caller's executor.

### Step 4: transform values with `thenValue`

Once an executor is attached, build a continuation chain:

```cpp
folly::Future<Model> decodeResponse(
   Client& client,
   Request request,
   folly::Executor::KeepAlive<> executor) {
   return fetchAsync(client, std::move(request))
      .via(std::move(executor))
      .thenValue([](Response&& response) {
         return decode(std::move(response));
      });
}
```

`thenValue` runs only when the upstream Future contains a value. If the
upstream contains an exception, it bypasses the callback and propagates to the
next error-aware stage.

The callback may return:

| Callback result | Resulting chain |
| --- | --- |
| `U` | `Future<U>` |
| `void` | `Future<folly::Unit>` |
| `Future<U>` | Flattened to `Future<U>` rather than nested |
| `SemiFuture<U>` | Activated and flattened while preserving the chain's execution policy |
| throws an exception | A failed Future containing that exception |

Flattening lets one asynchronous stage naturally follow another:

```cpp
auto stored = decodeResponse(client, std::move(request), executor)
   .thenValue([&store](Model&& model) {
      return store.writeAsync(std::move(model));
   });
```

Do not perform long blocking work in a continuation running on an event-loop
executor. Switch explicitly with `.via(cpuExecutor)` before a CPU-heavy or
blocking stage, then switch back if later work requires the original executor.

### Step 5: understand why chains use `std::move`

Future handles are move-only and normally consumed as each continuation is
attached:

```cpp
auto first = fetchAsync(client, request).via(executor);
auto second = std::move(first).thenValue(parse);
auto third = std::move(second).thenValue(validate);
```

The API uses rvalue-qualified operations to express one continuation owner.
After moving from `first` or `second`, do not use that handle again.

Writing one expression avoids intermediate moved-from variables:

```cpp
auto result = fetchAsync(client, request)
   .via(executor)
   .thenValue(parse)
   .thenValue(validate);
```

`std::move` does not run a Future or move its value by itself. It casts the
handle to an rvalue so the called operation may transfer its shared-state
ownership.

### Step 6: handle errors and cleanup

Use `thenTry` when one stage needs to inspect both success and failure:

```cpp
auto observed = std::move(future).thenTry(
   [](folly::Try<Response>&& outcome) -> Response {
      if (outcome.hasException()) {
         recordFailure(outcome.exception());
      }
      return std::move(outcome).value();
   });
```

Calling `.value()` returns the value or rethrows the captured exception. Use
`hasValue()`, `hasException()`, and `exception()` when branching explicitly.

Use `thenError` for selective recovery while letting successful values pass:

```cpp
auto recovered = std::move(future).thenError(
   folly::tag<NetworkError>,
   [](const NetworkError&) {
      return Response::fromCache();
   });
```

An unmatched exception continues down the chain. A recovery callback must
produce a value or Future compatible with the chain's original value type.

Use `ensure` for cleanup that must run on either outcome:

```cpp
auto cleaned = std::move(future).ensure([lease = std::move(lease)]() mutable {
   lease.release();
});
```

`ensure` passes the original value or exception through. If the cleanup itself
throws, its exception replaces the earlier outcome, so cleanup should normally
be non-throwing.

Choose an error policy at each API boundary:

- expected domain failures can be values such as `Expected<T, E>`;
- unexpected failures can travel as Future exceptions;
- interruption and timeout should have documented meanings;
- logging a failure is not the same as recovering from it.

### Step 7: combine independent Futures

Start all independent operations before collecting them:

```cpp
std::vector<folly::SemiFuture<Response>> operations;
operations.reserve(requests.size());

for (auto& request : requests) {
   operations.push_back(fetchAsync(client, std::move(request)));
}

auto all = folly::collectAll(std::move(operations));
```

The collection consumes the input Future handles. The major combinators have
different result and failure contracts:

| Combinator | Result and completion behavior |
| --- | --- |
| `collectAll` | Waits for every input and returns `Try<T>` for each. Individual input failures do not fail the outer collection. |
| `collect` | Returns values directly and completes exceptionally when an input fails, potentially before all other inputs finish. |
| `collectAny` | Returns the index and `Try<T>` of the first completion. |
| `collectAnyWithoutException` | Returns the first successful value; if all fail, it reports a failure. |
| `collectN` | Returns after a requested number of inputs complete. |

The modern safe collection functions return `SemiFuture`, because the thread
that completes the last input should not implicitly become the continuation
policy. Attach the desired executor afterward:

```cpp
auto summarized = folly::collectAll(std::move(operations))
   .via(executor)
   .thenValue([](std::vector<folly::Try<Response>>&& outcomes) {
      return summarize(std::move(outcomes));
   });
```

Future collection does not imply structured cancellation. In particular,
first-result and short-circuit combinators do not make unfinished producers
disappear. Their underlying operations may continue, so they must retain valid
state and have an explicit interruption and shutdown policy.

Avoid legacy `collect*Unsafe` helpers for new code. They erase incoming
executor information and produce an inline Future; prefer a safe collection
followed by `.via(executor)`.

### Step 8: add deadlines deliberately

`within()` creates a result that fails with `FutureTimeout` if the source does
not finish before the duration:

```cpp
using namespace std::chrono_literals;

auto bounded = fetchAsync(client, request)
   .within(500ms)
   .via(executor);
```

After an executor is attached, `onTimeout()` can provide a fallback:

```cpp
auto response = fetchAsync(client, request)
   .via(executor)
   .onTimeout(500ms, [] {
      return Response::temporaryUnavailable();
   });
```

A timeout settles the derived Future; it does not forcibly terminate arbitrary
producer code. The original operation may continue unless its implementation
supports interruption or another cancellation mechanism. Keep its state alive
and account for its resource use after the caller's deadline.

Use an injected or manual `Timekeeper` when timeout behavior must be tested
without wall-clock sleeps.

### Step 9: support interruption when the producer can cancel

The consumer can send an advisory interrupt upstream:

```cpp
future.raise(folly::FutureCancellation{});
// Equivalent cancellation shorthand:
future.cancel();
```

The producer observes it only if its promise installs an interrupt handler:

```cpp
folly::Promise<Response> promise;
auto future = promise.getSemiFuture();

promise.setInterruptHandler(
   [&operation](const folly::exception_wrapper&) {
      operation.requestCancellation();
   });
```

The handler should arrange for the underlying operation to finish and for the
promise eventually to receive a terminal outcome. Merely receiving an
interrupt does not fulfill the promise automatically.

Interruption has race semantics:

- the value may win before the interrupt;
- the interrupt may reach the producer before normal completion;
- the underlying API may be unable to cancel;
- an interrupt handler can run during registration or in the consumer's
  calling thread, so it must be thread-safe and quick;
- completion must remain exactly once regardless of the winner.

Treat interruption as a request, not thread termination. The application must
still account for the producer's eventual cleanup and shutdown.

### Step 10: share one result with multiple consumers

Ordinary Future handles have one continuation chain. Do not attempt to copy a
Future or attach unrelated consumers to the same handle.

`FutureSplitter<T>` turns one executor-bound Future into multiple consumers:

```cpp
#include <folly/futures/FutureSplitter.h>

auto splitter = folly::splitFuture(
   fetchAsync(client, request).via(executor));

auto forCache = splitter.getFuture();
auto forMetrics = splitter.getFuture();
```

`SharedPromise<T>` is the producer-side alternative when one producer is
designed from the beginning to serve multiple consumers. It can issue multiple
`SemiFuture<T>` handles before or after fulfillment, but the shared promise's
lifetime must be managed explicitly. `FutureSplitter` handles that lifetime
internally for an existing Future.

Shared delivery may require copying the result. For expensive or move-only
payloads, consider whether the shared value should instead be an immutable
`std::shared_ptr<const T>`.

### Step 11: block only at a synchronous boundary

`get()` waits and consumes the Future, returning its value or rethrowing its
exception:

```cpp
Response response = std::move(future).get();
```

`wait()` blocks until readiness while retaining a Future handle. `getTry()`
provides a non-throwing inspection boundary. Timed variants report that the
wait expired but do not imply producer cancellation.

Never call a blocking Future method on an event-loop thread or on an executor
worker required to complete that same Future. This can stall unrelated work or
deadlock the operation entirely.

## Executor and thread rules

Without a deliberate executor, the thread that attaches a callback can race
with the thread that fulfills the promise, making callback placement
surprising. Prefer this shape:

```cpp
producerApi()
   .via(executorA)
   .thenValue(stageA)
   .via(executorB)
   .thenValue(stageB);
```

The executor associated with a Future governs its following continuations
until another `.via()` changes it. It does not guarantee one OS thread unless
the executor itself has that property.

Use inline continuations only when their bounded, non-blocking behavior and
stack-depth consequences are understood. The normal executor-aware operations
are the safer default.

## Ownership and lifetime checklist

Before returning a Future chain, verify:

1. Every `Promise` has exactly one terminal completion path.
2. Every named Future is moved at most once and not reused afterward.
3. Callback captures own their required values or borrow objects guaranteed to
   outlive completion.
4. Executor keep-alive handles remain valid for all scheduled continuations.
5. First-result, timeout, and error paths do not destroy state still used by an
   unfinished producer.
6. Interruption races cannot cause double fulfillment or data races.
7. No continuation blocks the event loop or a required worker pool.
8. Every chain ends in a consumer, returned handle, callback, or intentional
   lifetime owner; results are not silently abandoned.

## Future and coroutine interoperability

Folly Futures are semi-awaitable in Folly coroutine machinery. A `Task` can
consume a `SemiFuture` and inherit the task's current executor for resumption:

```cpp
folly::coro::Task<Response> fetchAsTask(
   Client& client,
   Request request) {
   co_return co_await fetchAsync(client, std::move(request));
}
```

Plain awaiting does not automatically translate task cancellation into a
Future interrupt. Use the explicit bridge when that policy is desired:

```cpp
#include <folly/coro/FutureUtil.h>

co_return co_await folly::coro::toTaskInterruptOnCancel(
   fetchAsync(client, std::move(request)));
```

In the other direction:

```cpp
auto semiFuture = folly::coro::toSemiFuture(buildResponse(request));
auto future = folly::coro::toFuture(
   buildResponse(request), executorKeepAlive);
```

`toSemiFuture()` preserves lazy/deferred activation. `toFuture()` binds an
executor and starts the awaitable. At an API boundary, prefer one primary model
and convert once rather than alternating Future and coroutine abstractions at
every layer.

## Testing Future code

A manual executor makes continuation scheduling deterministic:

```cpp
#include <folly/executors/ManualExecutor.h>

folly::ManualExecutor executor;
folly::Promise<int> promise;

auto result = promise.getSemiFuture()
   .via(&executor)
   .thenValue([](int value) {
      return value * 2;
   });

promise.setValue(21);
EXPECT_FALSE(result.isReady());

executor.drain();
EXPECT_EQ(std::move(result).get(), 42);
```

Useful tests cover:

- value completion before and after continuation registration;
- exceptional completion and selective recovery;
- a producer destroyed without fulfillment;
- executor switching and callback ordering;
- timeout with a controlled timekeeper;
- interruption racing with normal completion;
- collection with zero, one, and many inputs;
- owner shutdown while operations are outstanding.

## Futures versus coroutines

Use Futures when an API is callback-native, when a continuation graph is the
natural representation, or when integrating a Future-heavy subsystem. Use
coroutines when direct control flow, loops, scoped local state, and exception
handling make the operation easier to read.

Both models still require explicit executor, cancellation, ownership, and
shutdown design. Coroutine syntax does not remove those responsibilities, and
a Future chain is not automatically unstructured if its terminal ownership is
clear.

## Common mistakes

1. Returning `Future<T>` from a general library and thereby choosing the
   caller's executor policy.
2. Creating a Future without making blocking work asynchronous.
3. Reusing a Future after `thenValue`, `thenTry`, `.via()`, or `get()` consumed
   it.
4. Assuming a continuation always executes on the producer thread or the
   registration thread.
5. Blocking on an event loop or on the executor required for completion.
6. Assuming `collectAny`, `collect`, or a timeout stops unfinished producers.
7. Installing an interrupt handler that does not eventually settle the
   promise.
8. Capturing references or `this` without guaranteeing their lifetime.
9. Dropping a Future chain without an intentional owner for its completion and
   errors.
10. Using inline or `collect*Unsafe` behavior without auditing callback cost,
    reentrancy, and executor semantics.

## Further reading

- [Official Folly Futures guide](https://github.com/facebook/folly/blob/main/folly/docs/Futures.md)
- [`Future`, `SemiFuture`, and collection API contracts](https://github.com/facebook/folly/blob/main/folly/futures/Future.h)
- [`Promise` producer contract](https://github.com/facebook/folly/blob/main/folly/futures/Promise.h)
- [`SharedPromise`](https://github.com/facebook/folly/blob/main/folly/futures/SharedPromise.h)
- [`FutureSplitter`](https://github.com/facebook/folly/blob/main/folly/futures/FutureSplitter.h)
- [Future/coroutine conversion utilities](https://github.com/facebook/folly/blob/main/folly/coro/FutureUtil.h)
