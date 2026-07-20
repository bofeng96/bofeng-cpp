# Meta Folly: a practical mental model and guide to its critical primitives

This guide targets Folly `2026.07.06.00`, the version installed with Homebrew
when it was written. Folly publishes frequently and makes no ABI-compatibility
guarantee from commit to commit. Treat names and signatures as versioned; treat
the architectural relationships in this guide as the durable knowledge.

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

Default to standard containers. Switch only after measuring or when an API
requires the Folly type. Inline containers increase the size of every owning
object, so a larger `N` is not automatically better.

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

## 9. Coroutines: direct-style asynchronous composition

Folly coroutines use C++ language suspension while preserving Folly executor,
cancellation, and async-stack integration.

### `Task<T>` and executor binding

`folly::coro::Task<T>` is a move-only, single-use, lazy computation. Calling a
task-returning function creates a coroutine frame but does not run its body.

```cpp
folly::coro::Task<Response> fetch(Request request) {
  auto bytes = co_await read(request);
  co_return decode(bytes);
}
```

Awaiting an unbound child task binds it to the parent task's executor.
`co_withExecutor(keepAlive, task)` explicitly binds a root task and returns a
still-lazy `TaskWithExecutor<T>`. `start()` eagerly schedules that bound task
and returns a `SemiFuture<T>`.

### Async concurrency and synchronization

Important coroutine primitives include:

- `sleep` and timed-wait helpers;
- `Baton`, `Mutex`, `SharedMutex`, `Semaphore`, and channels designed to
  suspend coroutines instead of blocking worker threads;
- `collectAll`/`collectAny`-style coroutine combinators;
- `AsyncScope` for dynamically adding concurrent work and later joining it;
- async generators and stream/channel abstractions;
- `blockingWait` only for synchronous boundaries and tests.

Suspension is not blocking: the coroutine saves state and releases the worker
to run other work. It also is not parallelism: two tasks run concurrently only
when explicitly started/collected/scoped that way.

### Cancellation

`CancellationSource` requests cancellation. Copies of its `CancellationToken`
are passed to operations. `CancellationCallback` bridges cancellation into an
underlying callback API.

Cancellation is cooperative, asynchronous, and race-prone by nature:

- the request does not forcibly stop a thread;
- an operation must observe the token or register a callback;
- completion may win the race with cancellation;
- cleanup and joining still matter after cancellation is requested.

Prefer scopes in which all child work is joined before captured state is
destroyed. Detached work demands an explicit lifetime and error policy.

### Task lifetime rules

- Moving/awaiting/binding a task consumes its single ownership.
- A member coroutine normally retains `this`; the object must outlive it.
- References and views captured in a coroutine frame must outlive every
  suspension that can use them.
- Exceptions propagate through `co_await` and are rethrown at a blocking edge
  unless converted into a result type.
- Never hold a normal thread mutex across `co_await`.

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

Async execution breaks ordinary thread-local assumptions because one logical
request can cross threads. `RequestContext` attaches request-scoped data that
Folly-aware Futures, tasks, and fibers can capture and restore across async
boundaries. It is useful for tracing and request metadata, but hidden context
is still hidden coupling; use explicit parameters for core domain inputs.

Other production primitives include:

- `folly::Init` for process initialization and integration with flags/logging;
- the logging subsystem and glog integration;
- `observer::Observer<T>` for derived, asynchronously refreshed configuration
  snapshots;
- settings APIs for runtime configuration;
- histogram, quantile, timeseries, and thread-cached statistics;
- tracing and async-stack support;
- `Benchmark` and `BENCHMARK` macros for microbenchmarks;
- `File`, file utilities, and `Subprocess` for OS integration;
- `Singleton`, which manages difficult process-lifetime ordering but does not
  make global state good design.

Observers represent versioned snapshots, not mutable shared objects. Read a
snapshot, use it consistently for an operation, and avoid assuming a refresh
happens synchronously.

## 12. A decision guide

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
| New Folly async workflow | `coro::Task<T>` | An existing subsystem is Future- or fiber-native. |
| CPU work | `CPUThreadPoolExecutor` | Work is I/O readiness handling: use an I/O executor/event base. |
| Network payload | `IOBuf` chain | A foreign API requires contiguous memory: coalesce at that edge. |
| SPSC handoff | `ProducerConsumerQueue` | More producers/consumers require MPMC semantics. |
| Read-mostly shared lifetime | mutex/`shared_ptr` | Measurements justify hazard pointers or RCU complexity. |

## 13. Failure modes worth memorizing

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

## 14. Reading the repository's `folly-task` example correctly

The local example is one narrow slice through the larger stack:

```text
TaskExample::makeMessage(name)                 lazy Task<string>
        |
        | co_await loadAnswer()
        v
TaskExample::loadAnswer()                      lazy child Task<int>
        |
        | co_await folly::coro::sleep(delay)
        v
co_return 42 -> format message -> co_return string

Synchronous application boundary:
Task -> co_withExecutor -> TaskWithExecutor -> start -> SemiFuture -> get
```

The child inherits the parent's CPU executor. After `sleep`, the coroutine
resumes on that executor but not necessarily the same worker thread. The
`TaskExample` instance stays alive because its member coroutines retain access
to it. The test uses `blockingWait` as a synchronous boundary; production async
code should normally keep composing with `co_await`.

This example teaches task composition and executor binding. It does not cover
Folly's event-driven I/O, buffer model, Futures, cancellation, structured
concurrency, concurrent containers, or production support layers—all of which
are covered above.

## 15. A practical learning sequence

1. Learn `StringPiece`/ranges, `Function`, checked conversion, `Expected` or
   `result`, and F14 trade-offs.
2. Build a small shared component with `Synchronized`; use an SPSC or MPMC
   queue only when its topology matches.
3. Learn `Executor`, then contrast CPU and I/O thread pools.
4. Use `SemiFuture`/`Future` once to understand promise state and executor-aware
   continuations.
5. Build the same flow with `coro::Task`, cancellation, timeout, and a join
   scope.
6. Learn `EventBase` thread affinity and move CPU work off the loop.
7. Parse and build a protocol using `IOBuf`, `IOBufQueue`, `Cursor`, and
   `Appender` without unnecessary coalescing.
8. Add logging, metrics, configuration snapshots, and benchmarks.
9. Study hazard pointers, RCU, fibers, and specialized locks only when a real
   system or profile requires them.

## 16. Official references

- [Folly repository and build/compatibility notes](https://github.com/facebook/folly)
- [Official component overview](https://github.com/facebook/folly/blob/main/folly/docs/Overview.md)
- [Official documentation directory](https://github.com/facebook/folly/tree/main/folly/docs)
- [Executors](https://github.com/facebook/folly/blob/main/folly/docs/Executors.md)
- [Futures](https://github.com/facebook/folly/blob/main/folly/docs/Futures.md)
- [Coroutines](https://github.com/facebook/folly/blob/main/folly/coro/README.md)
- [Synchronized](https://github.com/facebook/folly/blob/main/folly/docs/Synchronized.md)
- [Hazard pointers and RCU](https://github.com/facebook/folly/blob/main/folly/docs/Hazptr.md)
- [F14 containers](https://github.com/facebook/folly/blob/main/folly/container/F14.md)
- [Arena and `SysArena`](https://github.com/facebook/folly/blob/main/folly/memory/Arena.h)
- [`ThreadCachedArena`](https://github.com/facebook/folly/blob/main/folly/memory/ThreadCachedArena.h)
- [`AtomicHashMap`](https://github.com/facebook/folly/blob/main/folly/AtomicHashMap.h)
- [`ConcurrentSkipList`](https://github.com/facebook/folly/blob/main/folly/ConcurrentSkipList.h)
- [`IOBuf` contract](https://github.com/facebook/folly/blob/main/folly/io/IOBuf.h)
- [`Cursor`, `Appender`, and `QueueAppender`](https://github.com/facebook/folly/blob/main/folly/io/Cursor.h)
- [`result<T>`](https://github.com/facebook/folly/blob/main/folly/result/README.md)
