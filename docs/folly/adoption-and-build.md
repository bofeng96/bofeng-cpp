# Adoption, build, portability, and failure modes

[← Folly guide index](README.md)

## Contents

- [A progressive adoption tutorial](#a-progressive-adoption-tutorial)
- [Build, versioning, and portability](#build-versioning-and-portability)
- [Decision guide](#a-decision-guide)
- [Failure modes](#failure-modes-worth-memorizing)

## A progressive adoption tutorial

### Step 1: start from a missing contract

Write down the concrete need before selecting a Folly type:

```text
Problem: callbacks resume on unpredictable threads
Required contract: caller-selected executor for every continuation
Candidate: SemiFuture<T> API + caller .via(executor)
Evidence: scheduling tests and queue-latency measurements
```

“Folly is faster” is not an adoption requirement. Name the ownership,
scheduling, concurrency, I/O, or measured performance property that is missing.

### Step 2: draw the execution and ownership boundaries

For an asynchronous component, document:

```text
caller owns request value
   -> operation starts on I/O executor
   -> CPU transformation owns detached bytes
   -> result returns to event-base thread
   -> request scope joins before service destruction
```

Mark every raw pointer, borrowed view, callback, executor token, cancellation
source, and blocking boundary. If the diagram cannot identify an owner until
completion, the design is not ready for implementation.

### Step 3: build one vertical slice

Adopt the smallest end-to-end path that exercises the real contract. Include
success, failure, timeout, cancellation, overload, and shutdown from the first
slice. A benchmark that excludes ownership and error paths can select the wrong
primitive.

Keep Folly types behind a narrow module boundary when the rest of the
application does not need them. This reduces rebuild, upgrade, and API-coupling
cost without pretending the dependency is free.

### Step 4: pin and reproduce the toolchain

Record the Folly revision or release, compiler, standard-library ABI, build
options, allocator, optional dependencies, and target platforms. Use C++20 and
rebuild Folly-dependent binaries together when upgrading.

A successful header-only compilation does not prove binary compatibility.
Inline implementation, feature macros, compiler flags, and linked dependencies
must agree across translation units.

### Step 5: validate semantics before performance

Test these contracts before benchmarking:

1. exact ownership and invalidation behavior;
2. executor and thread-affinity behavior;
3. exception, timeout, and cancellation propagation;
4. bounded admission and overload response;
5. cleanup order with work outstanding;
6. target-platform fallback behavior.

Then benchmark representative sizes, key distributions, contention, queueing,
and tail latency. Compare against the simplest standard-library design.

### Step 6: establish an upgrade loop

For each upgrade:

1. read upstream changes for every adopted subsystem;
2. rebuild the full dependency closure;
3. compile with deprecation warnings visible;
4. run sanitizers and concurrency/lifetime tests;
5. rerun performance baselines on deployment architectures;
6. canary with queue, error, timeout, and memory monitoring;
7. retain a rollback artifact and configuration plan.

Avoid depending on `detail`, `internal`, implementation headers, or accidental
transitive includes. These dramatically increase upgrade risk.

## Build, versioning, and portability

### Version and ABI policy

Folly uses date-based, frequent releases and evolves with Meta's internal
codebase. It does not guarantee ABI compatibility from commit to commit. Pin a
known-compatible revision or release, build dependents against the same
version, and treat upgrades like source migrations rather than drop-in shared
library replacements.

The official project recommends static linking in part because of this ABI
policy. A package-manager build is convenient for experimentation; production
systems should control compiler, standard library, Folly revision, feature
flags, and transitive dependency versions together.

### Build-system integration

Folly is C++20 and exposes the CMake target `Folly::folly` in common packaged
installations. The official `getdeps.py` flow builds compatible dependencies
and is the reference route when system packages are insufficient.

The dependency graph commonly includes Boost, fmt, glog, gflags, libevent,
double-conversion/fast-float, OpenSSL, and compression libraries. Exact
requirements depend on platform and enabled features. Large headers and
templates can also increase compile time; keep includes narrow and hide Folly
types behind implementation boundaries when they are not part of the API.

### Platform and feature checks

The same public header may select architecture-specific intrinsics, an OS
backend, or a portable fallback. Examples include F14 SIMD paths, epoll or
io_uring I/O, allocator integration, stack symbolization, and coroutine support.

Do not infer production behavior from one development machine. Build and test
on every target architecture/OS, exercise the selected backend, and consult
`folly-config.h`/portability feature macros when an API is conditional.

### Upgrade discipline

For an upgrade:

1. Read release/source changes between pinned revisions.
2. Rebuild Folly and all C++ dependents with one toolchain and standard library.
3. Compile with deprecation warnings visible; migrate away from legacy aliases.
4. Run concurrency, cancellation, teardown, sanitizer, and performance tests.
5. Recheck platform fallbacks and optional codec/TLS availability.

## A decision guide

| Need | Start with | Move to something else when |
| --- | --- | --- |
| Borrowed text/bytes | `std::string_view` or `StringPiece` | Ownership is needed: use a string or `IOBuf`. |
| Hash map | `std::unordered_map` | Profiling justifies F14 or address stability selects `F14NodeMap`. |
| Small sequence | `std::vector` | Inline storage is measured to help: `small_vector`. |
| Many allocations with one lifetime | normal ownership/allocator | Bulk reclamation is the desired contract: `SysArena`; use `ThreadCachedArena` for concurrent allocation. |
| Protected shared state | `Synchronized<T>` | A concurrent container or read-mostly reclamation has a proven advantage. |
| Integer-keyed concurrent map | ordinary map plus a lock | `AtomicHashMap`'s capacity, key, erase, and reclamation restrictions all fit. |
| Concurrent sorted unique set | protected `std::set` | Read scalability justifies `ConcurrentSkipList` and its accessor/reclamation contract. |
| Callback with move-only capture | `folly::Function` | The callback is borrowed rather than owned. |
| Domain value/error | `Expected<T, E>` | Exception/stopped transport is needed: consider `result<T>`. |
| Callback async API | `SemiFuture<T>` | Direct-style async is preferable: expose `coro::Task<T>`. |
| New immediately-awaited Folly coroutine | `coro::now_task<T>` where supported | Delayed/movable scheduling is genuinely required; then evaluate safe-task aliases or established `Task<T>`. |
| Established/movable Folly task API | `coro::Task<T>` | Borrowed lifetimes are hard to prove: redesign ownership or use safer task/closure facilities. |
| CPU work | `CPUThreadPoolExecutor` | Work is I/O readiness handling: use an I/O executor/event base. |
| Network payload | `IOBuf` chain | A foreign API requires contiguous memory: coalesce at that edge. |
| Async stream | `AsyncGenerator` for pull, bounded queue/channel for producer-driven flow | Fan-out, merge, transforms, or multiplexing justify `folly::channels`. |
| Live configuration | `observer::Observer<T>` snapshot | A hot read path justifies a specialized observer cache. |
| SPSC handoff | `ProducerConsumerQueue` | More producers/consumers require MPMC semantics. |
| Read-mostly shared lifetime | mutex/`shared_ptr` | Measurements justify hazard pointers or RCU complexity. |

## Failure modes worth memorizing

1. **Blocking an event loop.** One slow callback delays every connection on
   that loop.
2. **Blocking a required executor.** A `.get()` or `blockingWait()` can wait for
   work that cannot run because the current worker is occupied.
3. **Assuming task creation starts work.** `Task` is lazy.
4. **Assuming executor affinity means one OS thread.** CPU-pool work may resume
   on another worker.
5. **Using an event-base object from the wrong thread.** Most event-base APIs
   are thread-affine unless documented otherwise.
6. **Losing task/callback owners.** Captured references, `this`, executor
   handles, and callback targets must survive asynchronous completion.
7. **Holding locks across suspension or callbacks.** This expands critical
   sections unpredictably and invites deadlock.
8. **Mutating a shared `IOBuf`.** Clone is shallow; call `unshare()` before
   mutation.
9. **Coalescing buffers by habit.** It destroys the zero-copy advantage.
10. **Using a concurrent structure without its exact contract.** Producer
    count, capacity, iterator stability, reclamation, and memory ordering all
    matter.
11. **Treating cancellation as forced termination.** It is a request and must
    be observed, followed by cleanup/joining.
12. **Depending on ABI or internal APIs.** Pin Folly versions and rebuild
    dependents together.
13. **Launching unbounded concurrency.** One task per item can overwhelm
    queues, memory, downstream services, or file descriptors; use windowing,
    capacity, and admission control.
14. **Assuming a Folly type is faster by definition.** Platform, key/value
    shape, contention, allocator, and workload determine the result.
15. **Ignoring shutdown.** Executors, scopes, transports, log writers, and
    producers need an ordering that stops admission, requests cancellation,
    joins work, flushes output, and only then destroys dependencies.
