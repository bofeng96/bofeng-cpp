# Meta Folly: a practical mental model and guide to its critical primitives

This is a standalone guide to modern Folly rather than a description of one
application or installation. Folly publishes frequently and makes no
ABI-compatibility guarantee from commit to commit. The examples favor current
public APIs, but exact availability and signatures can vary by release. Treat
names and signatures as versioned; treat the architectural relationships as
the durable knowledge.

## Table of contents

1. [What Folly is](#1-what-folly-is)
2. [The whole system in one picture](#2-the-whole-system-in-one-picture)
3. [Foundation types and value handling](#3-foundation-types-and-value-handling)
4. [Byte ownership and zero-copy I/O](#4-byte-ownership-and-zero-copy-io)
5. [Thread synchronization and concurrent data](#5-thread-synchronization-and-concurrent-data)
6. [Executors: where runnable work runs](#6-executors-where-runnable-work-runs)
7. [`EventBase`: the I/O reactor](#7-eventbase-the-io-reactor)
8. [Futures: callback-based asynchronous composition](#8-futures-callback-based-asynchronous-composition)
9. [Coroutines: direct-style asynchronous composition](#9-coroutines-direct-style-asynchronous-composition)
   - [A progressive `Task` tutorial](#a-progressive-task-tutorial)
10. [Fibers: stackful cooperative concurrency](#10-fibers-stackful-cooperative-concurrency)
11. [Context propagation, configuration, and observability](#11-context-propagation-configuration-and-observability)
12. [Build, versioning, and portability](#12-build-versioning-and-portability)
13. [A decision guide](#13-a-decision-guide)
14. [Failure modes worth memorizing](#14-failure-modes-worth-memorizing)
15. [Worked design: an asynchronous aggregation pipeline](#15-worked-design-an-asynchronous-aggregation-pipeline)
16. [A practical learning sequence](#16-a-practical-learning-sequence)
17. [Subsystem inventory](#17-subsystem-inventory)
18. [Official references](#18-official-references)

## 1. What Folly is

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

## 2. The whole system in one picture

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

## 3. Foundation types and value handling

### `Range<T>` and `StringPiece`

`Range` is a non-owning pair of iterators/pointers. `StringPiece` is its common
character specialization: cheap to pass, slice, and inspect without copying.

Use it for borrowed input when the caller owns the bytes. Never let it outlive
the source string or buffer. Prefer `std::string_view` for portable new APIs
unless a Folly API or Folly-specific operation makes `StringPiece` useful.

### `fbstring`, `fbvector`, and `small_vector`

- `fbstring` and `fbvector` are performance-oriented analogues of
  `std::string` and `std::vector` with Folly-specific storage optimizations.
- `small_vector<T, N>` keeps a small number of elements inline, avoiding a heap
  allocation until it grows beyond its inline capacity.

For one-byte characters on a representative modern 64-bit build, `fbstring`
uses
three storage categories: small strings are stored inline, medium strings use
an allocation and copy eagerly, and large strings use reference-counted storage
with lazy copying. Some current implementations use thresholds of 0–23,
24–254, and 255+ characters, respectively, but these are implementation details
rather than a portable API contract. Allocations use Folly's allocator-size
helpers and can benefit from jemalloc-aware builds.

Default to standard containers. Switch only after measuring or when an API
requires the Folly type. Inline containers increase the size of every owning
object, so a larger `N` is not automatically better. Likewise, do not assume
`fbstring` beats a modern `std::string` for every size and workload.

### `Arena`, `SysArena`, and `ThreadCachedArena`

An arena serves allocations that share one lifetime. `Arena<Alloc>` obtains
larger blocks from an underlying allocator and satisfies many small allocations
from those blocks. Individual `deallocate()` calls intentionally do nothing;
the blocks are reclaimed together when the arena is destroyed.

- `SysArena` is the ready-made `malloc`/`free`-backed arena.
- `ArenaAllocator` and `SysArenaAllocator` adapt an arena for allocator-aware
  containers.
- `ThreadCachedArena` gives each calling thread its own `SysArena`, reducing
  contention. When a thread exits, its blocks remain owned by the parent arena
  and are reclaimed when the `ThreadCachedArena` is destroyed.

This is a strong fit for request parsing, syntax trees, or batches of
short-lived objects that can all die together. It is a poor fit when individual
objects must promptly return memory. Raw arena allocation manages storage, not
arbitrary object destruction; object-lifetime responsibilities still apply.

### F14 hash containers

F14 maps and sets are cache-conscious open-addressing hash containers. They are
often a better performance candidate than `std::unordered_map`, but their
variants encode real trade-offs:

| Type | Choose it when |
| --- | --- |
| `F14FastMap` / `F14FastSet` | You want Folly to choose a generally fast representation. This is the first variant to consider. |
| `F14ValueMap` / `F14ValueSet` | Keys and values are suitable for inline table storage and locality matters. Rehashing may invalidate references. |
| `F14NodeMap` / `F14NodeSet` | Stable element addresses/references are required; per-node allocation is acceptable. |
| `F14VectorMap` / `F14VectorSet` | Compact storage and fast iteration matter; insertion/erasure and iterator behavior have different trade-offs. |

Reserve capacity when cardinality is predictable. Confirm iterator and
reference invalidation rules before storing addresses. Benchmark with the real
key distribution and target CPU; F14 relies on vectorized operations on
supported platforms and has a fallback elsewhere.

### `Function`

`folly::Function<Signature>` is a type-erased callable like `std::function`,
but move-only and able to own move-only captures:

```cpp
folly::Function<void()> work =
    [p = std::make_unique<Job>()]() mutable { p->run(); };
```

It also models callable constness in the signature. Use it for owned callbacks
that should be invoked once or transferred. Use `FunctionRef`-style borrowing
only when the callback's lifetime is clearly longer than the call.

### Values, errors, and exceptions

Folly has several result carriers because they solve different problems:

| Type | Meaning | Best fit |
| --- | --- | --- |
| `Optional<T>` | value or absence | Legacy Folly APIs; prefer `std::optional` in portable new code. |
| `Expected<T, E>` | value or an explicit typed error `E` | Synchronous domain errors where the error is data. |
| `Try<T>` | value, captured exception, or legacy empty state | Future continuations and exception-aware async plumbing. |
| `result<T>` | value, exception error, or stopped/cancelled state | Newer result/coroutine code needing a non-empty result model and distinct cancellation. |
| `exception_wrapper` | Type-erased captured exception with inspection/rethrow helpers | Transporting exceptions without immediately throwing them. |

An expected failure such as “key not found” usually belongs in an explicit
domain type. A broken invariant or unexpected implementation failure may be an
exception. Cancellation is control flow, not necessarily an error; newer Folly
`result` APIs model a separate stopped state for that reason.

### `dynamic` and JSON

`folly::dynamic` is a runtime-typed JSON-like value supporting null, booleans,
numbers, strings, arrays, and objects. `parseJson`, `toJson`, and
`toPrettyJson` bridge it to JSON.

It is convenient at untyped boundaries, but type checks happen at runtime and
each value is relatively large. Convert validated input into static domain
types rather than spreading `dynamic` through business logic.

### Strings, conversion, and formatting

- `folly::to<T>(...)` and `tryTo<T>(...)` perform checked conversions.
- `folly::split`, `join`, trimming, replacement, and case helpers live in the
  string utilities.
- `folly::sformat` returns a formatted string; `folly::format` builds a
  formatter object; formatting syntax is Python-like.
- Base64, URI, IP address, Unicode, hash, and bit helpers cover common systems
  tasks.

Conversion is a validation boundary. Prefer checked conversion over C-style
parsing or unchecked narrowing.

### RAII, lifetime, and operating-system helpers

- `ScopeGuard`, `SCOPE_EXIT`, `SCOPE_FAIL`, and `SCOPE_SUCCESS` attach cleanup
  to lexical scope. Prefer ordinary RAII types when a reusable owner exists;
  use a scope guard for local rollback and cleanup logic.
- `File` is an owning file-descriptor wrapper. `FileUtil` supplies common file
  operations, and `Subprocess` manages child processes and pipes.
- `Indestructible<T>` deliberately avoids static destruction. `Singleton`
  coordinates more complex process-lifetime ordering. Both solve narrow
  lifetime problems; neither makes hidden global state desirable.
- `Lazy` and `ConcurrentLazy` defer construction, with different concurrency
  contracts.
- `Poly` builds non-intrusive, type-erased interfaces with value/reference
  forms. It is powerful but more elaborate than a virtual base class or a
  simple `Function`; choose it when value-semantic polymorphism is the goal.

### Algorithms and sequence processing

Folly includes map helpers, bit and hash functions, sorted-vector containers,
and the `gen` library for lazy pipeline-style sequence processing. In portable
new code, compare `gen` with C++20 ranges before committing to a Folly-specific
pipeline. `sorted_vector_map`/`sorted_vector_set` are useful when data changes
rarely and compact ordered iteration matters more than insertion cost.

## 4. Byte ownership and zero-copy I/O

### `IOBuf`

`IOBuf` is one of Folly's central networking primitives. It separates the
small buffer descriptor from the underlying byte storage.

```text
IOBuf descriptor
  buffer() -> [ headroom | readable data | tailroom ] <- bufferEnd()
                         length()
```

Important properties:

- The underlying storage is reference-counted and may be shared by cloned
  `IOBuf` objects.
- Multiple `IOBuf`s can form a circular chain representing one logical byte
  sequence without copying it into contiguous memory.
- `clone()` is normally shallow: it clones descriptors and shares bytes.
- Before mutating shared bytes, call the appropriate `unshare()` operation.
- `coalesce()` makes data contiguous and may allocate and copy. Do it only
  when an API truly needs contiguous bytes.
- The buffer's atomic reference count does not make mutation thread-safe.

`IOBufQueue` manages a stream-like queue of `IOBuf` chains. `io::Cursor` reads
typed values or byte ranges across chain boundaries without requiring the
chain to be coalesced. `io::Appender` writes into an `IOBuf`, while
`io::QueueAppender` writes into and grows an `IOBufQueue`. These abstractions
are preferable to manual pointer arithmetic for protocol codecs.

The zero-copy rule is: preserve chains and shared slices for as long as
possible; coalesce only at an unavoidable boundary.

### Codecs and compression

The codec and compression directories add hex/UUID helpers, varint encodings,
and codec wrappers for algorithms such as zlib and Zstandard. Compression APIs
can reuse contexts through pools to avoid repeated setup cost.

Treat codecs as boundary components: enforce output limits, validate malformed
input, and make decompression-bomb protection explicit. Availability and exact
codec set depend on how Folly and its optional dependencies were built.

## 5. Thread synchronization and concurrent data

### `Synchronized<T, Mutex>`

`Synchronized` packages data with the mutex that protects it. Access requires
a lock-carrying object:

```cpp
folly::Synchronized<State> state;

state.withWLock([](State& s) { s.advance(); });
auto snapshot = state.withRLock([](const State& s) { return s.snapshot(); });
```

`wlock()` gives exclusive mutable access; `rlock()` gives shared const access
when the mutex supports it. Keep locked-pointer scopes short and never retain a
reference, pointer, or iterator after the lock object dies.

This is usually the best first Folly concurrency primitive because it makes the
correctness relationship between state and lock explicit.

### Mutexes, spin locks, batons, and semaphores

- `SharedMutex` supports shared/exclusive locking and is the default behind
  `Synchronized`.
- Micro/spin/RW locks are specialized for extremely short critical sections.
  They burn CPU while waiting and should be chosen only with contention data.
- `Baton` represents a small one-shot handoff: one side waits, another posts.
  There are thread-blocking and coroutine-oriented baton variants.
- `LifoSem` and related semaphores coordinate worker wakeups with locality in
  mind.

Never hold a lock across blocking I/O, a coroutine suspension point, or an
unknown callback unless the API explicitly makes that safe.

### Concurrent queues and maps

| Primitive | Contract |
| --- | --- |
| `ProducerConsumerQueue<T>` | Bounded single-producer/single-consumer ring. |
| `MPMCQueue<T>` | Bounded multiple-producer/multiple-consumer queue; capacity is allocated up front. |
| `UnboundedQueue` variants | Concurrent queues when a fixed capacity is inappropriate; understand allocation and reclamation costs. |
| `ConcurrentHashMap` | Concurrent associative access with API-specific consistency and mutation semantics. |
| `AtomicHashMap` | Specialized integer-keyed map with wait-free lookup and concurrency-safe insertion. Capacity must be planned; growth degrades performance, and erase does not reclaim its cell. |
| `ConcurrentSkipList` | Sorted unique-key set with lock-free, mostly wait-free reads. Writes acquire locks local to neighboring nodes, and removed nodes remain until no accessor can observe them. |

Choose by producer count, consumer count, required bounds/backpressure, and
blocking behavior. Choose maps by key restrictions, ordering needs, capacity,
mutation behavior, and reclamation rules. “Lock-free” does not mean every
operation is lock-free, nor does it guarantee lower latency under a particular
workload.

### Atomics, hazard pointers, and RCU

Plain mutexes and `shared_ptr` are the default lifetime tools. Hazard pointers
and RCU are for read-dominated paths where reference-count traffic or reader
locking has become measurably expensive.

- A hazard pointer lets a reader announce which object it may dereference.
- A writer removes an object from publication and **retires** it.
- Reclamation is deferred until no reader can still hold a hazard.
- RCU uses read-side critical sections and grace periods at a coarser grain.

The bargain is very cheap scalable reads in exchange for harder reasoning,
more expensive publication/reclamation, non-deterministic destruction, and
possibly higher temporary memory use. Use Folly's domain and holder APIs; do
not invent memory-reclamation schemes casually.

Thread-local and core-cached facilities such as `ThreadLocal`,
`ThreadCachedInt`, and read-mostly shared pointers similarly trade freshness,
memory, or write cost for reduced cross-core contention.

## 6. Executors: where runnable work runs

`folly::Executor` is the minimal scheduling abstraction. At its heart, it
accepts a callable through `add()`. An executor is not itself an asynchronous
operation and does not necessarily own a thread.

### The two principal pools

| Executor | Use it for | Do not do |
| --- | --- | --- |
| `CPUThreadPoolExecutor` | Computation, parsing, compression, and other runnable CPU work. It uses a shared work queue and supports priorities. | Let long blocking calls consume all workers. |
| `IOThreadPoolExecutor` | Event-driven I/O; each worker owns an `EventBase`. | Run expensive CPU work in event-loop callbacks. |

Separate pools protect I/O tail latency from CPU saturation. Size CPU pools
from measured service demand; size event-loop pools around concurrency and
core needs, assuming callbacks do not block.

### Keep-alive handles and affinity

`Executor::KeepAlive<>` is a capability to submit work while the executor is
alive. APIs such as `getKeepAliveToken` and `co_withExecutor` use it to make the
scheduling lifetime explicit.

Executor affinity is not thread affinity. A task bound to a CPU pool can resume
on a different worker after suspension. Event-base-affine objects are stricter:
most of their methods must be called on their owning event-loop thread.

### Other executor forms

| Executor | Role |
| --- | --- |
| `InlineExecutor` | Runs submitted work immediately on the caller. Useful only when reentrancy and stack growth are acceptable. |
| `ManualExecutor` / drivable executors | Queue work until explicitly driven; valuable for deterministic tests and synchronous bridges. |
| `SerialExecutor`, `SequencedExecutor`, and strand-style executors | Preserve sequencing over another execution resource without implying a dedicated thread. |
| `ScheduledExecutor` | Adds time-based scheduling to the executor model. |
| `MeteredExecutor` | Wraps execution with admission/queue controls and measurements. |
| Global CPU/I/O executors | Convenient process-wide defaults; explicit injection is easier to test and isolate. |

An executor wrapper changes scheduling semantics, not the nature of the work.
For example, serialization does not make blocking work safe on an I/O thread.

## 7. `EventBase`: the I/O reactor

`EventBase` waits for file-descriptor readiness and timers, then invokes
callbacks. One event base drives one thread at a time. Most methods are
thread-affine; cross-thread calls must use explicitly thread-safe scheduling
methods such as `runInEventBaseThread()`.

```text
poll for fd/timer readiness
        -> run short callbacks
        -> update registrations/state
        -> poll again
```

The event-loop commandment is: callbacks must finish quickly. Move CPU-heavy
work to a CPU executor and express the handoff through a Future or coroutine.

Common I/O building blocks include:

- `AsyncTransport` as a transport interface;
- `AsyncSocket` for asynchronous stream sockets;
- `AsyncServerSocket` for accepting connections;
- `AsyncTimeout` and timer facilities;
- `IPAddress`, `SocketAddress`, and URI helpers;
- SSL/TLS transports and contexts;
- `IOBuf`/`IOBufQueue` for payloads.

UDP sockets, signal handlers, pipes, and coroutine transport adapters extend
the same event-base model. Linux builds may offer epoll and io_uring backends;
code should not assume those optimized paths exist on every target.

Many async transports use delayed-destruction patterns because callbacks may
still be on the stack when shutdown begins. Follow each transport's documented
close and destruction protocol rather than applying ordinary immediate
`delete` reasoning.

These are lower-level building blocks, not a complete HTTP/RPC framework.

## 8. Futures: callback-based asynchronous composition

### `Promise`, `SemiFuture`, `Future`, and `Try`

```text
producer owns Promise<T>
       | setValue / setException
       v
shared result state
       ^
consumer owns SemiFuture<T> --via(executor)--> Future<T>
```

- `Promise<T>` is the producer side and may be fulfilled once.
- `SemiFuture<T>` is a movable consumer handle without an attached executor.
- `.via(executor)` produces `Future<T>` with continuation execution control.
- `Future<T>` chains work using `thenValue`, `thenTry`, and `thenError`.
- `Try<T>` carries either the value or captured exception into exception-aware
  continuations.

Prefer returning `SemiFuture<T>` from libraries so the caller chooses where
continuations run. Attach an executor before chaining. Without explicit
executor control, completion and callback-registration races can determine the
thread that runs a callback.

Aggregation helpers express parallel joins:

- `collectAll` waits for every operation and preserves individual outcomes;
- fail-fast or value-only collection variants use different error semantics;
- `collectAny`/`collectN` represent first/some completion.

Read each combinator's exception and cancellation contract; similar names do
not imply identical cleanup of unfinished work.

Blocking `.get()` and `.wait()` belong at synchronous program boundaries.
Calling them on a worker/event-loop thread needed for completion can deadlock.

### Future ownership, failure, and interruption

Future handles are generally consumed as chains are built, which is why Folly
examples repeatedly use `std::move(future)`. A `Promise` must be completed once;
destroying the producer without a result makes the consumer observe a broken
promise. Exceptions travel through the shared state until a `thenTry`,
`thenError`, or blocking boundary handles them.

Future interruption is advisory. Raising an interrupt only has an effect when
the producer registered an interrupt handler, and completion can race with the
request. This is the same broad principle as coroutine cancellation: request,
cooperate, and still join or otherwise account for completion.

### Futures versus coroutines

Use Futures when an API is callback-native, when a continuation graph is the
natural representation, or when integrating a Future-heavy subsystem. Use
coroutines when direct control flow, loops, scoped local state, and exception
handling make the operation easier to read. Both still require explicit
executor, cancellation, and lifetime design; coroutine syntax does not remove
those responsibilities.

## 9. Coroutines: direct-style asynchronous composition

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

## 10. Fibers: stackful cooperative concurrency

Folly fibers are lightweight, cooperatively scheduled execution contexts with
their own stacks. They can make blocking-looking code yield to other fibers on
the same thread. `fibers::Baton` is the common one-shot handoff that suspends a
waiting fiber and allows another fiber or thread to wake it.

Use fibers when integrating an established fiber-based stack or when stackful
adaptation materially simplifies callback code. Prefer coroutines for most new
async APIs: compiler-visible suspension, typed awaitables, and structured
lifetime patterns are easier to compose. A fiber that performs a real blocking
system call still blocks its underlying thread.

Do not mix Futures, coroutines, and fibers arbitrarily. Pick a primary async
model per subsystem and use deliberate adapters at boundaries.

## 11. Context propagation, configuration, and observability

### Request context

Async execution breaks ordinary thread-local assumptions because one logical
request can cross threads. `RequestContext` attaches request-scoped data that
Folly-aware Futures, tasks, and fibers can capture and restore across async
boundaries. It is useful for tracing and request metadata, but hidden context
is still hidden coupling; use explicit parameters for core domain inputs.

### Observers and settings

`observer::Observer<T>` represents a value that is recomputed when its
dependencies change. Readers take a stable snapshot; a later refresh does not
mutate an existing snapshot. Prefer `with()` when a value is only needed inside
one callback because it keeps the snapshot alive without letting references
escape.

Observer variants optimize different read patterns:

- `AtomicObserver` for atomically readable value types;
- `TLObserver` for thread-local caching at additional memory cost;
- read-mostly, hazard-pointer, and core-cached variants for specialized hot
  read paths.

A hazard-protected observer snapshot should not cross a coroutine suspension
unless its contract explicitly permits it. Settings APIs provide related
runtime configuration mechanisms. Treat refresh as asynchronous: one request
should usually use one consistent snapshot.

### Logging

The logging subsystem provides categories, levels, handlers, formatters,
asynchronous file writers, configuration parsing, rate limiting, and bridges to
Google logging. `XLOG`-style macros avoid unnecessary formatting when a level
is disabled.

Logging is still I/O. An asynchronous writer moves cost off a request path but
introduces queueing, flush, overload, and shutdown policy. Never assume a log
statement is free or guaranteed to persist after an abrupt exit.

### Metrics, tracing, and debugging

- `BucketedTimeSeries`, `MultiLevelTimeSeries`, histograms, quantile
  estimators, `TDigest`, and streaming statistics support in-process
  aggregation.
- Thread-cached counters reduce contention but trade away immediate global
  freshness.
- Async-stack integration preserves logical coroutine/Future ancestry across
  physical stack breaks.
- Symbolization, demangling, exception tracing, and stack-trace helpers aid
  diagnostics, with capabilities varying by platform and build configuration.

Metrics structures do not export themselves. A service still needs naming,
collection, scraping/flush, cardinality, and shutdown policies.

### Initialization, testing, and benchmarking

- `folly::Init` coordinates process initialization and common flag/logging
  setup. Create it near `main` before relying on parsed flags or global runtime
  facilities.
- `Benchmark` and `BENCHMARK` macros provide microbenchmark registration,
  calibration, baselines, and formatted results.
- Test utilities include temporary resources, deterministic/manual executors,
  Future helpers, and coroutine-aware GoogleTest macros.

Microbenchmarks explain primitive cost, not end-to-end service behavior. Test
correctness under cancellation, shutdown, contention, and allocator pressure,
then validate latency distributions in a representative workload.

### Time, rate control, randomness, and hashing

Folly includes high-resolution duration helpers, stopwatches, token buckets,
timeout queues, schedulers, random generation, fingerprints, and multiple hash
utilities. Use cryptographic facilities only when the API explicitly promises
cryptographic properties; a fast hash or random helper is not automatically
suitable for secrets, signatures, or adversarial input.

## 12. Build, versioning, and portability

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

## 13. A decision guide

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

## 14. Failure modes worth memorizing

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

## 15. Worked design: an asynchronous aggregation pipeline

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

## 16. A practical learning sequence

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

## 17. Subsystem inventory

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

## 18. Official references

- [Folly repository and build/compatibility notes](https://github.com/facebook/folly)
- [Official component overview](https://github.com/facebook/folly/blob/main/folly/docs/Overview.md)
- [Official documentation directory](https://github.com/facebook/folly/tree/main/folly/docs)
- [Executors](https://github.com/facebook/folly/blob/main/folly/docs/Executors.md)
- [Futures](https://github.com/facebook/folly/blob/main/folly/docs/Futures.md)
- [Coroutines](https://github.com/facebook/folly/blob/main/folly/coro/README.md)
- [`Task` and `TaskWithExecutor`](https://github.com/facebook/folly/blob/main/folly/coro/Task.h)
- [`now_task`](https://github.com/facebook/folly/blob/main/folly/coro/safe/NowTask.h)
- [`safe_task` and task aliases](https://github.com/facebook/folly/blob/main/folly/coro/safe/SafeTask.h)
- [Coroutine collection](https://github.com/facebook/folly/blob/main/folly/coro/Collect.h)
- [`AsyncScope` and `CancellableAsyncScope`](https://github.com/facebook/folly/blob/main/folly/coro/AsyncScope.h)
- [Cancellation tokens](https://github.com/facebook/folly/blob/main/folly/CancellationToken.h)
- [Coroutine timeouts](https://github.com/facebook/folly/blob/main/folly/coro/Timeout.h)
- [Synchronized](https://github.com/facebook/folly/blob/main/folly/docs/Synchronized.md)
- [Hazard pointers and RCU](https://github.com/facebook/folly/blob/main/folly/docs/Hazptr.md)
- [F14 containers](https://github.com/facebook/folly/blob/main/folly/container/F14.md)
- [`fbstring` implementation and storage categories](https://github.com/facebook/folly/blob/main/folly/FBString.h)
- [Arena and `SysArena`](https://github.com/facebook/folly/blob/main/folly/memory/Arena.h)
- [`ThreadCachedArena`](https://github.com/facebook/folly/blob/main/folly/memory/ThreadCachedArena.h)
- [`AtomicHashMap`](https://github.com/facebook/folly/blob/main/folly/AtomicHashMap.h)
- [`ConcurrentSkipList`](https://github.com/facebook/folly/blob/main/folly/ConcurrentSkipList.h)
- [`IOBuf` contract](https://github.com/facebook/folly/blob/main/folly/io/IOBuf.h)
- [`Cursor`, `Appender`, and `QueueAppender`](https://github.com/facebook/folly/blob/main/folly/io/Cursor.h)
- [`EventBase`](https://github.com/facebook/folly/blob/main/folly/io/async/EventBase.h)
- [`AsyncSocket`](https://github.com/facebook/folly/blob/main/folly/io/async/AsyncSocket.h)
- [`AsyncGenerator`](https://github.com/facebook/folly/blob/main/folly/coro/AsyncGenerator.h)
- [Channels](https://github.com/facebook/folly/tree/main/folly/channels)
- [Observers](https://github.com/facebook/folly/blob/main/folly/observer/Observer.h)
- [Compression](https://github.com/facebook/folly/blob/main/folly/compression/Compression.h)
- [`result<T>`](https://github.com/facebook/folly/blob/main/folly/result/README.md)
