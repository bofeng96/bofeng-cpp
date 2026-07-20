# Executors and scheduling

[← Folly guide index](README.md)

## Contents

- [The executor model](#the-executor-model)
- [A progressive executor tutorial](#a-progressive-executor-tutorial)
- [Choosing an executor](#choosing-an-executor)
- [Backpressure and overload](#backpressure-and-overload)
- [Lifetime and shutdown](#lifetime-and-shutdown)
- [Testing scheduling](#testing-scheduling)
- [Common mistakes](#common-mistakes)

## The executor model

An executor accepts runnable work. It answers **where and when may this callable
run?**, not **what result does the operation produce?** Futures and coroutines
describe results and dependencies; executors supply execution resources.

```text
operation or continuation
          |
          v
       Executor
          |
          +---- CPU worker pool
          +---- EventBase / I/O worker
          +---- serial or strand wrapper
          +---- manual test executor
```

Executor affinity is not thread identity. A task bound to a multi-thread CPU
pool may resume on different workers while remaining on the same executor.

## A progressive executor tutorial

### Step 1: submit independent work

```cpp
#include <folly/executors/CPUThreadPoolExecutor.h>

folly::CPUThreadPoolExecutor cpuExecutor{4};
auto cpu = folly::Executor::getKeepAliveToken(cpuExecutor);

cpu->add([] {
   performIndependentWork();
});
```

`add()` transfers a callable to the executor and does not provide a result
handle. Use it only when ownership, failure reporting, and shutdown accounting
exist elsewhere. For value-producing work, prefer a Future or coroutine:

```cpp
auto result = folly::via(cpu, [] {
   return computeValue();
});
```

The returned `Future<T>` carries completion and exceptions. Unhandled
exceptions in a raw fire-and-forget callable have no useful caller to receive
them.

### Step 2: separate CPU and I/O work

```cpp
#include <folly/executors/IOThreadPoolExecutor.h>

folly::CPUThreadPoolExecutor cpuExecutor{4};
folly::IOThreadPoolExecutor ioExecutor{2};
```

Use CPU workers for compute, compression, parsing, serialization, and blocking
operations that cannot be made asynchronous. Use I/O workers for event loops,
socket readiness, timers, and brief callbacks.

An I/O thread may manage thousands of connections because callbacks yield
quickly. One blocking callback delays every connection assigned to that loop.

```cpp
auto* eventBase = ioExecutor.getEventBase();
eventBase->runInEventBaseThread([eventBase] {
   registerTransport(*eventBase);
});
```

Treat an `EventBase` as thread-affine. Marshal work to it instead of touching
its transports from arbitrary threads.

### Step 3: bind Future continuations

Libraries normally return `SemiFuture<T>` so the caller chooses execution:

```cpp
auto response = fetchAsync(request)
   .via(ioKeepAlive)
   .thenValue(readHeaders)
   .via(cpuKeepAlive)
   .thenValue(decodeAndValidate);
```

Every `.via()` changes the executor for following continuations. This makes
expensive transitions visible and prevents producer-completion races from
choosing an accidental callback thread.

### Step 4: bind and start a root coroutine

```cpp
auto task = buildResponse(request);
auto bound = folly::coro::co_withExecutor(
   cpuKeepAlive, std::move(task));
auto result = std::move(bound).start();
```

`co_withExecutor` consumes a lazy task and returns a still-lazy
`TaskWithExecutor`. `start()` schedules it and returns a `SemiFuture` for its
completion. Child tasks inherit the current executor unless explicitly rebound.

Within a coroutine, switch one child operation deliberately:

```cpp
auto encoded = co_await folly::coro::co_withExecutor(
   cpuKeepAlive, encodeResponse(std::move(page)));
```

After the child completes, the parent resumes on its own executor rather than
permanently adopting the child's executor.

### Step 5: serialize without dedicating a thread

When operations must execute one at a time, use a serializing executor wrapper
or a primitive such as `SerialExecutor`/`StrandExecutor` rather than allocating
one thread per logical key.

```text
concurrent callers -> serializing executor -> operation A -> B -> C
                              |
                              v
                       shared backing pool
```

Serialization controls ordering, not transaction boundaries. If an operation
starts asynchronous child work and returns immediately, later operations may
still overlap unless the submitted callable represents the full lifetime that
must be serialized.

### Step 6: add priorities only after isolation

Some executors support priority lanes or wrappers. Priorities influence queue
selection; they cannot preempt a long-running callable already occupying a
worker. Separate critical traffic, cap expensive work, and keep tasks small
before relying on priority as a latency guarantee.

## Choosing an executor

| Requirement | First choice | Important warning |
| --- | --- | --- |
| CPU-bound work | `CPUThreadPoolExecutor` | Bound task size and queue growth; workers are shared. |
| Non-blocking sockets and timers | `EventBase` or `IOThreadPoolExecutor` | Never block the loop; respect thread affinity. |
| Deterministic unit test | `ManualExecutor` | The test must explicitly drive queued work. |
| Immediate trivial adapter | `InlineExecutor` | Reentrancy and deep callback stacks are possible. |
| One-at-a-time execution | `SerialExecutor`, strand, or serial queue | Ensure the submitted unit covers the whole async operation. |
| Delayed/scheduled work | scheduled executor or `EventBase` timer | Define cancellation and owner lifetime during the delay. |
| Metrics, limits, or policy wrapping | metered/throttled executor wrappers | Wrapper ordering changes which cost is measured or limited. |

An executor is not automatically appropriate because its name matches a task.
Profile queue delay, runtime, utilization, and downstream saturation.

## Backpressure and overload

A thread pool limits running threads but does not by itself make unlimited
submission safe. A growing queue converts overload into latency and memory use.

Use several controls together:

- bound queues or reject work at admission;
- cap coroutine/Future fan-out;
- apply per-tenant and global concurrency limits;
- isolate latency-critical and best-effort work;
- measure queue time separately from execution time;
- propagate overload to callers instead of hiding it indefinitely.

Avoid blocking a pool worker while waiting for work queued to the same
exhausted pool. This thread-starvation deadlock can occur even without a mutex.

## Lifetime and shutdown

`Executor::KeepAlive<>` is the preferred transferable reference to an
executor. It prevents final executor teardown while asynchronous chains still
hold the token, but it does not define application shutdown by itself.

A robust shutdown sequence is:

1. stop accepting new work;
2. request cancellation of owned operations;
3. join scopes and observe Future completions;
4. close event-base transports and timers on their owning loops;
5. release keep-alive tokens;
6. join and destroy executor pools.

Capturing a raw executor pointer is safe only when a stronger owner guarantees
that sequence. Beware cycles in which work captures a keep-alive token but can
complete only after executor destruction begins.

## Testing scheduling

```cpp
#include <folly/executors/ManualExecutor.h>

folly::ManualExecutor executor;
bool ran = false;

executor.add([&] {
   ran = true;
});

EXPECT_FALSE(ran);
executor.drain();
EXPECT_TRUE(ran);
```

Manual execution lets a test assert queued-versus-completed state without
sleeping. Test executor switches, rejection/overload, callback ordering, and
shutdown with work outstanding. Use real pools only for integration tests that
need actual parallelism.

## Common mistakes

1. Treating an executor as a result or cancellation abstraction.
2. Blocking an event-loop thread.
3. Assuming executor affinity means one worker thread.
4. Launching unlimited work because the number of threads is bounded.
5. Calling `.get()` on a worker needed to complete that Future.
6. Using `add()` without an error and lifetime owner.
7. Assuming priority can preempt long-running tasks.
8. Destroying a pool before its tasks, callbacks, and keep-alive tokens finish.
9. Using inline execution without considering reentrancy and stack depth.
10. Switching executors repeatedly without measuring transition and queue cost.

## Further reading

- [Official executor guide](https://github.com/facebook/folly/blob/main/folly/docs/Executors.md)
- [`Executor` contract](https://github.com/facebook/folly/blob/main/folly/Executor.h)
- [`CPUThreadPoolExecutor`](https://github.com/facebook/folly/blob/main/folly/executors/CPUThreadPoolExecutor.h)
- [`IOThreadPoolExecutor`](https://github.com/facebook/folly/blob/main/folly/executors/IOThreadPoolExecutor.h)
- [`ManualExecutor`](https://github.com/facebook/folly/blob/main/folly/executors/ManualExecutor.h)
