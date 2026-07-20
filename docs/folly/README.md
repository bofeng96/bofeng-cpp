# Meta Folly: a practical mental model and guide

This is a standalone guide to modern Folly. Start with the system map, then
follow the topic that matches the problem you are solving.

## Guide contents

- [Foundation types, memory, and containers](foundations.md)
- [Byte ownership, event loops, and networking](io-and-networking.md)
- [Synchronization and concurrent data](concurrency.md)
- [Executors and scheduling](executors.md)
- [Futures and promises](futures.md)
- [Coroutines and `Task`](coroutines.md)
- [Fibers](fibers.md)
- [Context, configuration, and observability](production.md)
- [Adoption, build, portability, and failure modes](adoption-and-build.md)
- [Official references](references.md)

Folly publishes frequently and makes no ABI-compatibility guarantee from commit
to commit. Examples favor current public APIs, but exact availability and
signatures can vary by release.

## What Folly is

Folly is Meta's collection of C++20 components for production systems. It is
not an application framework and it is not a replacement for the standard
library. It fills gaps where `std` or Boost lacks an abstraction Meta needs, or
where a different performance profile is important.

Three ideas explain most of its design:

1. **Make ownership and execution explicit.** Move-only tasks, explicit
   executors, `IOBuf` ownership, and lock-carrying pointers make hidden costs
   and lifetimes visible.
2. **Compose instead of block.** Futures, coroutines, fibers, event loops, and
   executor-aware APIs let a small number of threads manage much more work.
3. **Optimize measured hot paths.** F14 containers, specialized queues,
   thread-cached counters, hazard pointers, and chained buffers trade
   generality for locality, fewer allocations, or less contention.

Use the standard library first when it meets the contract and performance
need. Use Folly when its semantics are the point—not merely because an
equivalent-looking Folly type exists.

Do not depend on `folly::detail`, `folly::internal`, or implementation headers.
Avoid experimental APIs unless accepting upgrade breakage is an explicit
decision.

### Evaluation at a glance

| Dimension | Strength | Cost or limitation |
| --- | --- | --- |
| Async runtime | Executors, Futures, coroutines, fibers, cancellation, timers, and I/O share a composable execution model. | There are several generations of async APIs, so a codebase needs a deliberate primary model and boundary adapters. |
| Networking | `EventBase`, transports, sockets, TLS, timers, and `IOBuf` form a proven low-level stack. | It is infrastructure, not a complete HTTP/RPC framework; thread affinity and lifetime rules are sharp. |
| Performance primitives | F14, arenas, specialized queues, thread caches, hazard pointers, and RCU cover demanding hot paths. | Many types trade generality for speed. Their contracts must be learned and benchmarked against the actual workload. |
| Correctness tools | `Synchronized`, explicit executors, move-only operations, result carriers, and newer safe coroutine types expose ownership and execution. | Older or lower-level APIs still permit dangling references, accidental blocking, and unstructured background work. |
| Portability | Folly supports major desktop/server platforms and wraps many platform differences. | Some facilities are Linux- or architecture-sensitive, and optimized paths may differ from fallbacks. |
| Integration | Components can usually be adopted individually at the API level. | The build brings a substantial dependency graph, and Folly does not promise commit-to-commit ABI compatibility. |
| Documentation | Important headers contain unusually detailed contracts and examples. | Coverage is uneven; source comments and tests are often more current than narrative documents. |

### When Folly is a good fit

Folly is compelling when a project needs several of these together:

- executor-aware asynchronous services;
- high-throughput networking with buffer chaining and minimal copying;
- carefully controlled CPU and I/O scheduling;
- specialized concurrent or read-mostly data structures;
- performance measurement and tuning on known deployment platforms;
- compatibility with other Meta open-source C++ projects.

It is often excessive for a small portable library, a short-lived command-line
program, or a project that only needs one utility already available in C++20/23.
The integration and upgrade cost should be justified by system-level value.

### Adoption questions

Before choosing a Folly component, answer:

1. What exact contract or measured bottleneck is missing from `std` or an
   existing dependency?
2. Which executor or event loop owns the work, and where may it block?
3. Who owns every object, buffer, callback, and coroutine frame until
   completion?
4. How are timeout, cancellation, failure, and shutdown propagated?
5. Does the target platform use the optimized implementation or a fallback?
6. Can Folly and all dependents be pinned and rebuilt together during upgrades?

## The whole system in one picture

```text
Values and ownership
Range/StringPiece, Function, Expected/result/Try, dynamic, IOBuf, F14
                         |
                         v
Concurrency and lifetime
Synchronized, SharedMutex, Baton, queues, atomics, hazptr/RCU
                         |
                         v
Scheduling
Executor -> CPUThreadPoolExecutor / IOThreadPoolExecutor -> EventBase
                         |
             +-----------+-----------+
             |                       |
             v                       v
Async composition                 Stackful concurrency
Future/SemiFuture/Promise         fibers::FiberManager
coro::Task + cancellation
             |
             v
Asynchronous I/O
EventBase + AsyncTransport/AsyncSocket + timers + IOBuf/IOBufQueue
             |
             v
Production support
Init, logging, Observer, settings, stats, tracing, Benchmark
```

The most important distinction is this:

- An **operation** describes work and a future result.
- An **executor** decides where runnable CPU work executes.
- An **event loop** waits for I/O readiness and timers.
- A **thread** is only the physical resource underneath them.

Confusing those layers leads to blocking event loops, accidental thread hops,
deadlocks, and unclear ownership.

## A practical learning sequence

1. Learn `StringPiece`/ranges, `Function`, checked conversion, `Expected` or
   `result`, and F14 trade-offs.
2. Build a small shared component with `Synchronized`; use an SPSC or MPMC
   queue only when its topology matches.
3. Learn `Executor`, then contrast CPU and I/O thread pools.
4. Use `SemiFuture`/`Future` once to understand promise state and executor-aware
   continuations.
5. Build the same flow with `coro::Task`, then compare `now_task`; add
   cancellation, timeout, `collectAll`, and a join scope.
6. Learn `EventBase` thread affinity and move CPU work off the loop.
7. Parse and build a protocol using `IOBuf`, `IOBufQueue`, `Cursor`, and
   `Appender` without unnecessary coalescing.
8. Add logging, metrics, configuration snapshots, and benchmarks.
9. Study hazard pointers, RCU, fibers, and specialized locks only when a real
   system or profile requires them.

## Subsystem inventory

This table is a navigation map, not a promise that every header has the same
stability or portability. Start with a component's public header contract and
tests before adopting it.

| Area | Representative facilities | Learning priority |
| --- | --- | --- |
| Top-level foundations | ranges/string pieces, conversion, formatting, `Function`, `Expected`, `Try`, files, process helpers, initialization | High; these appear throughout Folly APIs. |
| `algorithm`, `math`, `chrono`, `hash`, `random` | SIMD/algorithm helpers, checked math, time utilities, hashing, fingerprints, random generation | Use selectively; distinguish fast utilities from security contracts. |
| `container` | F14, small/heap/compact containers, views, access helpers | High when container performance matters; benchmark variants. |
| `synchronization`, `concurrency` | mutexes, batons, semaphores, atomic structures, concurrent queues/maps, hazard pointers, RCU | High for multithreaded systems; advanced reclamation is specialist material. |
| `memory` | arenas, allocator adapters, aligned allocation, jemalloc-aware helpers, read-mostly ownership | Medium; crucial after allocation/lifetime profiling. |
| `functional`, `poly` | invocation, composition, type-erased callables and interfaces | Medium; useful for callback and value-polymorphism designs. |
| `result` | value/error/stopped carriers, rich errors, coroutine propagation | High for new failure-aware APIs, but version-sensitive. |
| `json`, `dynamic` | dynamic values, JSON parse/serialize, JSON pointer and patch | High at untyped boundaries; convert to static domain types afterward. |
| `executors` | CPU/I/O pools, serial/strand/manual/inline/scheduled/metered executors | High; scheduling is the foundation of every Folly async model. |
| `futures` | Promise/Future/SemiFuture, continuation and collection utilities | High for existing Folly systems and callback integration. |
| `coro`, `coro/safe` | tasks, safe task wrappers, scopes, cancellation, collection, synchronization, generators, queues | High for modern asynchronous code. |
| `fibers` | stackful cooperative tasks, fiber synchronization and event-loop controllers | Learn when maintaining a fiber-based subsystem. |
| `channels` | async sender/receiver flows, transforms, merge, fan-out, multiplexing, rate limiting | Medium for complex streaming topologies. |
| `io`, `io/async`, `io/coro` | `IOBuf`, cursors, queues, event loops, sockets, transports, timers, coroutine transport wrappers | High for networking and service infrastructure. |
| `net`, `ssl` | network utilities, addresses, TLS contexts/transports/certificates | High for secure networking; platform and OpenSSL details matter. |
| `codec`, `compression`, `crypto` | hex/UUID/varint utilities, compression codecs and context pools, low-level crypto helpers | Adopt per need; validate optional dependencies and security properties. |
| `observer`, `settings` | dependency-tracked configuration snapshots and runtime settings | Medium to high for long-running configurable services. |
| `stats`, `logging`, `tracing`, `debugging` | time series, histograms, quantiles, log pipelines, async stacks, symbolization and exception tracing | High for operable production services. |
| `gen` | lazy sequence comprehensions and parallel transforms | Compare with C++ ranges for new portable code. |
| `init`, `cli`, `system` | process setup, command-line helpers, OS/architecture utilities | Use at executable and platform boundaries. |
| `testing` and test helpers | temporary resources, coroutine/Future test support, deterministic execution tools | High for verifying async and concurrent code. |
| `portability`, `lang`, `ext` | compiler/OS abstraction and language-building utilities | Mostly infrastructure; depend only on documented public symbols. |
| `python` | bridges such as an asyncio executor | Specialized interoperability. |

Folly deliberately stops below a complete application/service framework.
Projects such as Wangle and fbthrift build higher-level networking or RPC
abstractions on top of Folly and have their own contracts and release concerns.
