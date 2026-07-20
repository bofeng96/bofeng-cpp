# Synchronization and concurrent data

[← Folly guide index](README.md)

## Contents

- [Choose the simplest correct model](#choose-the-simplest-correct-model)
- [A progressive synchronization tutorial](#a-progressive-synchronization-tutorial)
- [Queues and handoff](#queues-and-handoff)
- [Concurrent maps and ordered structures](#concurrent-maps-and-ordered-structures)
- [Atomics and memory ordering](#atomics-and-memory-ordering)
- [Hazard pointers and RCU](#hazard-pointers-and-rcu)
- [Testing and common mistakes](#testing-and-common-mistakes)

## Choose the simplest correct model

Start with ownership and message passing. If state must be shared, start with a
mutex. Move to specialized queues, atomics, hazard pointers, or RCU only when
the required access pattern and measurements justify their narrower contracts.

```text
Can one thread/task own the state?
  yes -> send operations or values to that owner
  no  -> can one mutex protect a coherent invariant?
          yes -> Synchronized<T> or mutex
          no  -> select a specialized primitive from the exact topology
```

“Concurrent” and “lock-free” do not mean “safe for every operation,” “fair,” or
“faster under this workload.” Progress guarantees, reclamation, capacity, and
iteration semantics are separate properties.

## A progressive synchronization tutorial

### Step 1: bind data to its mutex with `Synchronized`

```cpp
#include <folly/Synchronized.h>

using Counts = std::unordered_map<std::string, std::size_t>;
folly::Synchronized<Counts> counts;

void record(std::string key) {
   counts.withWLock([&](Counts& values) {
      ++values[std::move(key)];
   });
}
```

`Synchronized<T>` prevents ordinary access to `T` without acquiring its
associated mutex. This makes the protected invariant visible in the type.

Use a read lock for observation:

```cpp
std::size_t lookup(const std::string& key) {
   return counts.withRLock([&](const Counts& values) {
      auto position = values.find(key);
      return position == values.end() ? 0 : position->second;
   });
}
```

The default shared mutex supports concurrent readers. A plain exclusive mutex
can be a better choice when writes are common or critical sections are tiny;
benchmark rather than assuming reader/writer locking wins.

### Step 2: make the critical section explicit

When several operations form one invariant, keep the locked pointer in a small
nested scope:

```cpp
{
   auto locked = counts.wlock();
   removeExpired(*locked);
   recomputeTotals(*locked);
}
// Lock released before logging or I/O.
```

Never retain references, pointers, iterators, or views into the protected value
after the locked pointer is destroyed. Copy or move out an independent value.

Do not hold a thread mutex across:

- `co_await` or fiber suspension;
- network or disk I/O;
- an unknown callback;
- executor submission followed by waiting;
- expensive parsing or serialization that can happen outside the lock.

### Step 3: coordinate multiple protected objects

Two independently correct locks can deadlock when acquired in opposite order.
Prefer redesigning the invariant under one `Synchronized<State>`. When objects
must remain separate, acquire them with a consistent global order or a Folly
multi-lock helper rather than hand-written nested locking.

```text
Thread A: lock accounts -> lock quotas
Thread B: lock quotas   -> lock accounts   = possible deadlock
```

Callbacks invoked while multiple locks are held must not re-enter either
component. Document lock order as part of the component contract.

### Step 4: use a baton for one-shot notification

```cpp
#include <folly/synchronization/Baton.h>

folly::Baton<> ready;

std::thread producer([&] {
   initializeData();
   ready.post();
});

ready.wait();
consumeData();
producer.join();
```

A baton expresses “not ready” followed by one posted state. It is clearer than
a condition variable for one-shot handoff and supplies the required
synchronization. Use the namespace-specific coroutine or fiber baton when the
waiter should suspend a coroutine/fiber rather than block an OS thread.

### Step 5: add bounded admission with a semaphore

A semaphore controls how many operations may enter a region. Acquire before
starting scarce work and release on every terminal path using RAII where
possible.

```text
incoming operations -> semaphore permits -> scarce dependency
                              |
                              +-> wait, reject, or time out when full
```

Choose intentionally between waiting and rejecting. A semaphore without a
bounded waiting queue can still accumulate unlimited callers elsewhere.

## Queues and handoff

Queue choice begins with topology:

| Primitive | Topology and behavior |
| --- | --- |
| `ProducerConsumerQueue<T>` | Bounded single-producer/single-consumer ring. `write`/`read` report full or empty. |
| `MPMCQueue<T>` | Bounded multiple-producer/multiple-consumer queue with blocking and non-blocking operations. |
| `UnboundedQueue<T>` | Unbounded queue with configurable producer/consumer behavior; avoids fixed capacity but can turn overload into memory growth. |

An SPSC example:

```cpp
#include <folly/ProducerConsumerQueue.h>

folly::ProducerConsumerQueue<Message> queue{1024};

if (!queue.write(std::move(message))) {
   reportOverload();
}

Message received;
if (queue.read(received)) {
   process(std::move(received));
}
```

Exactly one thread may perform producer operations and exactly one may perform
consumer operations. Adding a second producer because writes “seem atomic”
violates the contract.

For every queue, document:

1. producer and consumer counts;
2. bounded capacity and overload behavior;
3. blocking versus polling/wakeup behavior;
4. ordering requirements;
5. shutdown signaling and draining;
6. ownership of queued values.

## Concurrent maps and ordered structures

| Primitive | Appropriate shape | Important restrictions |
| --- | --- | --- |
| `AtomicHashMap` | Read-heavy integer-key lookup with concurrency-safe insertion and planned capacity | Keys have restrictions; growth is costly; erase does not reclaim a cell for reuse. |
| `ConcurrentHashMap` | More general concurrent map workloads | Learn accessor, iteration, erase, and consistency semantics before use. |
| `ConcurrentSkipList` | Concurrent sorted unique-key set | Reads are lock-free/mostly wait-free; writes use local locking; reclamation follows accessor lifetime. |

Do not choose a concurrent map merely to avoid designing ownership. A sharded
`Synchronized<F14Map>` can be simpler and faster when operations need compound
transactions or when the key space partitions naturally.

Iteration is particularly subtle: determine whether it is a snapshot, weakly
consistent view, or requires external exclusion. Never infer iteration
semantics from safe point lookup.

## Atomics and memory ordering

Use atomics for independent state such as counters, flags, and carefully
designed publication protocols—not as a replacement for protecting a
multi-field invariant.

```cpp
std::atomic<bool> stopping{false};

void requestStop() {
   stopping.store(true, std::memory_order_release);
}

bool shouldStop() {
   return stopping.load(std::memory_order_acquire);
}
```

Acquire/release is meaningful only when the flag publishes other memory. For a
statistical counter with no ordering role, relaxed operations may be enough.
Default sequential consistency is often the right starting point; weaken it
only with a written happens-before argument and tests appropriate for the
supported architectures.

Folly provides specialized atomics, distributed counters, and cache-line
helpers. They reduce contention only for the access patterns they were designed
for, and often make exact reads or destruction more expensive.

## Hazard pointers and RCU

Lock-free readers cannot safely dereference an object merely because an atomic
pointer still names it: another thread may remove and destroy it concurrently.
Hazard pointers and RCU separate logical removal from physical reclamation.

```text
reader protects or enters read section
        |
        v
load published pointer -> validate -> read

writer removes pointer -> retires object -> reclaim after no reader can see it
```

Use `folly::hazptr` when readers can protect individual objects and writers can
retire them. Use RCU when read-side critical sections and grace periods fit the
data structure. Both require disciplined domains, holders/guards, retirement,
and shutdown cleanup.

The trade is cheap scalable reads for harder proofs, delayed destruction, and
temporary memory growth. Do not build a new lock-free structure before a mutex
or existing Folly container has been measured and found inadequate.

## Testing and common mistakes

Concurrency tests should coordinate specific interleavings with batons,
barriers, or manual executors instead of relying on sleeps. Run stress tests and
thread sanitizers, but remember that a passing stress test is not a proof.

Common mistakes include:

1. accessing protected data after its locked pointer dies;
2. holding a lock across suspension, I/O, or callbacks;
3. inconsistent multi-lock order;
4. using an SPSC queue with multiple producers or consumers;
5. treating an unbounded queue as backpressure;
6. assuming lock-free means wait-free, fair, or faster;
7. erasing lock-free nodes without the required reclamation protocol;
8. using relaxed atomics without a happens-before explanation;
9. destroying queues or domains while threads still access them;
10. implementing compound map operations from individually safe calls without
    checking whether the overall operation is atomic.

## Further reading

- [`Synchronized` guide](https://github.com/facebook/folly/blob/main/folly/docs/Synchronized.md)
- [`Synchronized` API](https://github.com/facebook/folly/blob/main/folly/Synchronized.h)
- [Hazard pointers and RCU](https://github.com/facebook/folly/blob/main/folly/docs/Hazptr.md)
- [`AtomicHashMap`](https://github.com/facebook/folly/blob/main/folly/AtomicHashMap.h)
- [`ConcurrentSkipList`](https://github.com/facebook/folly/blob/main/folly/ConcurrentSkipList.h)
- [`ProducerConsumerQueue`](https://github.com/facebook/folly/blob/main/folly/ProducerConsumerQueue.h)
- [`MPMCQueue`](https://github.com/facebook/folly/blob/main/folly/MPMCQueue.h)
