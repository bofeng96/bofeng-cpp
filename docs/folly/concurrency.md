# Synchronization and concurrent data

[← Folly guide index](README.md)

## Thread synchronization and concurrent data

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

