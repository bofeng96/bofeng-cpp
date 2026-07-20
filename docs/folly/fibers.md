# Fibers

[← Folly guide index](README.md)

## Contents

- [The fiber model](#the-fiber-model)
- [A progressive fibers tutorial](#a-progressive-fibers-tutorial)
- [Blocking and stack safety](#blocking-and-stack-safety)
- [Fibers, Futures, and coroutines](#fibers-futures-and-coroutines)
- [Shutdown and testing](#shutdown-and-testing)
- [Common mistakes](#common-mistakes)

## The fiber model

Folly fibers are stackful, cooperatively scheduled execution contexts. Many
fibers can share one OS thread, and a fiber switches only at a fiber-aware
suspension point.

```text
one OS thread + FiberManager
        |
        +-> fiber A runs -> waits on fiber Baton
        +-> fiber B runs -> posts Baton
        +-> fiber A resumes
```

Because a fiber retains a stack, synchronous-looking functions can suspend deep
in the call stack without rewriting every caller as a callback or coroutine.
The cost is per-fiber stack memory and less visible suspension points.

## A progressive fibers tutorial

### Step 1: attach a `FiberManager` to an event loop

```cpp
#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/async/EventBase.h>

folly::EventBase eventBase;
auto& manager = folly::fibers::getFiberManager(eventBase);
```

The manager schedules fibers; its loop controller connects runnable fibers to
the `EventBase`. Keep one manager associated with its owning thread. A
`FiberManager` is not a general thread-safe submission object.

### Step 2: schedule fiber tasks

```cpp
manager.addTask([] {
   processRequest();
});

eventBase.loop();
```

`addTask` queues a callable to run on a fiber. The callable may use ordinary
stack locals across fiber-aware waits because the fiber's stack is preserved.

### Step 3: coordinate with `fibers::Baton`

```cpp
#include <folly/fibers/Baton.h>

folly::fibers::Baton baton;

manager.addTask([&] {
   prepareRequest();
   baton.wait();
   consumeResponse();
});

manager.addTask([&] {
   produceResponse();
   baton.post();
});

eventBase.loop();
```

`baton.wait()` suspends the current fiber, not the OS thread. The manager can
run another fiber until `post()` makes the waiter runnable.

Use `folly::fibers::Baton`, not a thread-blocking condition variable or the
coroutine baton. The namespace communicates which scheduler understands the
wait.

### Step 4: adapt a callback API with `fibers::await`

```cpp
#include <folly/fibers/Promise.h>

Response response = folly::fibers::await(
   [&](folly::fibers::Promise<Response> promise) {
      client.fetch(
         request,
         [promise = std::move(promise)](Response response) mutable {
            promise.setValue(std::move(response));
         });
   });
```

The setup callback runs immediately. `await` then suspends the current fiber
until the fiber promise receives a value or exception. Ensure every terminal
callback path fulfills the promise exactly once.

This adapter makes callback completion look synchronous, but the underlying
operation remains asynchronous and its callback/owner lifetimes still matter.

### Step 5: use fiber-aware synchronization

Folly fibers provide mutexes, timed mutexes, semaphores, promises, and
collection helpers built on fiber suspension. Use these inside fiber code so a
wait yields the OS thread to other fibers.

Do not hold a thread mutex while performing a fiber-aware wait. Another fiber
on the same OS thread may need that mutex to make progress, producing a
cooperative deadlock.

### Step 6: handle exceptions and timeouts

Exceptions thrown by a fiber task or delivered through `fibers::Promise` need
an explicit owner. Catch failures at a boundary that can record and translate
them; fire-and-forget fiber tasks must not silently lose errors.

Use timed fiber primitives when waiting has a deadline. A timeout stops the
wait, not necessarily the producer. Define whether the producer is cancelled,
allowed to finish, or detached with independent ownership.

## Blocking and stack safety

A fiber-aware wait yields cooperatively. A normal blocking syscall or mutex
wait blocks the entire OS thread and therefore every fiber scheduled on it.
Move unavoidable blocking calls to a dedicated blocking/CPU executor or use a
non-blocking callback adapter.

Each active fiber reserves stack space. Deep recursion, large stack arrays, and
unbounded numbers of suspended fibers can exhaust memory or overflow stacks.
Measure active fiber count and stack high-water behavior; place large payloads
on owned heap storage.

Fiber-local data exists for state that should follow one fiber rather than one
OS thread. Prefer explicit parameters for application data, and reserve local
storage for infrastructure where propagation is otherwise impractical.

## Fibers, Futures, and coroutines

| Model | Strength | Main risk |
| --- | --- | --- |
| Fibers | Synchronous-looking stackful code; easy adaptation of deep blocking-style call graphs | Hidden suspension, stack memory, accidental OS-thread blocking |
| Futures | Explicit continuation graph and executor transitions | Verbose branching and ownership chains |
| Coroutines | Direct-style stackless code with visible `co_await` | Borrowed lifetime mistakes and mixed-model boundaries |

Use fibers when maintaining or integrating a fiber-native subsystem, or when a
deep synchronous interface benefits materially from stackful suspension. For a
new C++20 async API, evaluate coroutines first unless the surrounding runtime
is already fiber-based.

Convert at clear boundaries. Repeatedly nesting fiber waits, Future callbacks,
and coroutine tasks makes cancellation, context propagation, and shutdown much
harder to reason about.

## Shutdown and testing

A fiber manager, its loop controller, event loop, and all task captures must
outlive active fibers. Stop admission, notify/cancel waiters, account for every
task, run required cleanup, and only then stop the event loop and destroy the
manager's dependencies.

Tests should coordinate with fiber batons and explicitly drive the event loop.
Cover promise failure, timeout, shutdown while suspended, and accidental
blocking. Stress stack usage separately from logical correctness.

## Common mistakes

1. Calling a blocking syscall in a fiber and stalling all peers on the thread.
2. Using a thread or coroutine synchronization primitive in fiber code.
3. Holding a thread mutex across `fibers::await` or `Baton::wait()`.
4. Capturing references whose owners disappear while a fiber is suspended.
5. Launching unbounded fibers without accounting for stack memory.
6. Assuming the `FiberManager` is safe to manipulate from arbitrary threads.
7. Timing out a waiter and destroying state still used by its producer.
8. Mixing async models repeatedly instead of converting at one boundary.

## Further reading

- [Official fibers guide](https://github.com/facebook/folly/blob/main/folly/fibers/README.md)
- [`FiberManager`](https://github.com/facebook/folly/blob/main/folly/fibers/FiberManager.h)
- [`fibers::Baton`](https://github.com/facebook/folly/blob/main/folly/fibers/Baton.h)
- [`fibers::Promise` and `await`](https://github.com/facebook/folly/blob/main/folly/fibers/Promise.h)
