# Byte ownership, event loops, and networking

[← Folly guide index](README.md)

## Contents

- [The networking stack](#the-networking-stack)
- [A progressive `IOBuf` tutorial](#a-progressive-iobuf-tutorial)
- [A progressive `EventBase` tutorial](#a-progressive-eventbase-tutorial)
- [Using asynchronous transports](#using-asynchronous-transports)
- [Codecs, compression, and zero-copy limits](#codecs-compression-and-zero-copy-limits)
- [Lifetime, backpressure, and shutdown](#lifetime-backpressure-and-shutdown)
- [Testing and common mistakes](#testing-and-common-mistakes)

## The networking stack

```text
protocol values
      |
      v
Cursor / Appender / QueueAppender
      |
      v
IOBuf chain <-> IOBufQueue
      |
      v
AsyncTransport / AsyncSocket / TLS transport
      |
      v
EventBase -> OS readiness and timers
```

Each layer has a different responsibility: byte ownership, protocol parsing,
transport state, or event dispatch. Keeping those responsibilities separate
makes lifetime, backpressure, and thread affinity easier to audit.

## A progressive `IOBuf` tutorial

### Step 1: create owned bytes

```cpp
#include <folly/io/IOBuf.h>

auto buffer = folly::IOBuf::copyBuffer("payload");
```

`copyBuffer` owns a copy. For incremental construction, reserve capacity and
explicitly commit written bytes:

```cpp
auto buffer = folly::IOBuf::create(4096);
auto* destination = buffer->writableTail();
const auto written = encode(destination, buffer->tailroom());
buffer->append(written);
```

`writableTail()` exposes capacity, while `append()` changes the visible data
length. Never append more than `tailroom()`, and never read beyond `length()`.

Wrapping external storage can eliminate a copy, but the buffer's owner and
free callback must remain correct until the final `IOBuf` reference is gone.
Copying is safer when lifetime is uncertain.

### Step 2: understand chains and clones

An `IOBuf` can be one node in a circular chain. Each node has its own data
range and may own or share a storage allocation.

```cpp
head->appendChain(std::move(tail));
```

Appending a chain relinks nodes rather than copying payload bytes. A clone is
also normally shallow: it creates new metadata referring to shared storage.

```cpp
auto sharedView = buffer->clone();
```

Before mutating bytes that may be shared, call the appropriate `unshare()` or
`unshareOne()` operation. Structural mutation of one chain and byte mutation of
shared storage are different concerns.

Avoid `coalesce()` by habit. It allocates/copies when a chained representation
would have worked. Coalesce only for an API that truly requires contiguous
memory, and measure the cost at that boundary.

### Step 3: parse with `Cursor`

```cpp
#include <folly/io/Cursor.h>

folly::io::Cursor cursor(buffer.get());
const auto frameLength = cursor.readBE<std::uint32_t>();

std::array<std::uint8_t, 16> identifier;
cursor.pull(identifier.data(), identifier.size());
```

`Cursor` reads across chain boundaries without first coalescing. Typed reads
make byte order explicit. Validate lengths before pulling attacker-controlled
data; a cursor prevents accidental pointer arithmetic but does not define your
protocol's maximum frame size.

Use `Cursor` for read-only traversal and `RWPrivateCursor` only when mutation
and uniqueness requirements are satisfied.

### Step 4: build output with `IOBufQueue` and `QueueAppender`

```cpp
folly::IOBufQueue queue{folly::IOBufQueue::cacheChainLength()};
folly::io::QueueAppender appender(&queue, 4096);

appender.writeBE<std::uint32_t>(payloadLength);
appender.push(payload.data(), payload.size());

std::unique_ptr<folly::IOBuf> frame = queue.move();
```

The queue owns a growing chain. `QueueAppender` ensures writable tail space and
advances lengths correctly. A growth size is an allocation policy, not a
protocol limit; enforce limits separately.

`IOBufQueue` is useful for stream framing because bytes can be appended as they
arrive and split or trimmed as complete messages are recognized.

## A progressive `EventBase` tutorial

### Step 1: run one event loop on one thread

```cpp
#include <folly/io/async/EventBase.h>

folly::EventBase eventBase;
eventBase.loopForever();
```

An `EventBase` drives readiness callbacks, timers, and queued functions. At
most one thread may run a given instance at a time. Most objects registered
with it inherit that thread affinity.

Applications usually run the loop on a dedicated I/O thread or obtain an
event base from `IOThreadPoolExecutor`, rather than blocking the main thread as
this minimal example does.

### Step 2: marshal work from another thread

```cpp
eventBase.runInEventBaseThread([&eventBase] {
   updateConnectionState(eventBase);
});
```

The callable runs on the event-base thread. Captures must survive until then.
Prefer owning values, weak ownership with an explicit expired path, or a
shutdown join that guarantees the referenced owner remains alive.

Use `runInEventBaseThreadAndWait()` only from another thread and only when
blocking is safe. Calling a wait form from the event-base thread risks
deadlock or reentrancy surprises.

### Step 3: schedule timers

```cpp
eventBase.runAfterDelay([] {
   performMaintenance();
}, 100);
```

Timer callbacks run on the event loop and therefore must stay short. Timer
registration does not automatically keep arbitrary captured objects alive or
cancel the callback during owner destruction.

For reusable timeout state, use timer objects with explicit registration,
cancellation, and owner-thread destruction rather than recursively capturing
raw `this` pointers.

### Step 4: move CPU work away from the loop

```text
EventBase callback
   -> detach/own input bytes
   -> submit parse or compute to CPU executor
   -> marshal result back to EventBase
   -> update transport state
```

Never access an event-base-affine socket from the CPU continuation. Return only
owned data, then schedule the transport mutation back on its owning loop.

## Using asynchronous transports

`AsyncTransport` is the common byte-transport abstraction. `AsyncSocket`
provides non-blocking TCP behavior integrated with `EventBase`; TLS transports
add handshake and certificate state while preserving the same broad event-loop
model.

A transport lifecycle normally follows:

```text
construct on EventBase
   -> connect callback
   -> install read callback
   -> write IOBuf chains with write callbacks
   -> apply read/write backpressure
   -> close or closeNow on owning thread
   -> release callbacks and transport
```

Writing transfers ownership of a chain:

```cpp
socket->writeChain(writeCallback, std::move(frame));
```

The write callback must outlive completion and handle both success and error.
After moving `frame`, the caller no longer owns those bytes.

Read callbacks either provide writable memory through `getReadBuffer()` and
receive a byte count, or accept movable `IOBuf` chains when the transport and
callback support that mode. In both forms:

- consume only the bytes reported;
- preserve partial protocol frames between callbacks;
- cap buffered input;
- pause reads when downstream work is saturated;
- handle EOF and read errors exactly once;
- detach the callback before destroying its owner.

`AsyncSocket` callbacks are intentionally low-level. Higher-level protocols
should encapsulate connection state, framing, deadlines, and callback
lifetimes rather than scattering them through unrelated objects.

## Codecs, compression, and zero-copy limits

Folly provides varint, hex, base64, UUID, and compression helpers around its
buffer types. Compression codecs commonly accept `IOBuf` input and produce new
buffers, while codec pools can reuse expensive contexts.

Zero-copy is not an absolute property of a pipeline. Copies may be necessary
for:

- encryption or compression output;
- APIs requiring contiguous memory;
- unsharing before mutation;
- retaining a small slice of a very large allocation;
- crossing a lifetime boundary where external storage is unsafe.

Optimize the number and size of copies, not the slogan. A deliberate bounded
copy can be cheaper than retaining megabytes of backing storage for a tiny
view.

## Lifetime, backpressure, and shutdown

For each connection, identify the owners of:

- the `EventBase` and its thread;
- the transport;
- read, write, connect, and timeout callbacks;
- queued `IOBuf` chains;
- CPU work derived from received bytes;
- cancellation and shutdown state.

Backpressure must cross layers. Pausing socket reads is useful only if queued
frames, CPU tasks, and downstream requests are also bounded. Write success
means the transport accepted/completed its contract, not necessarily that the
remote application processed the message.

A safe shutdown order is:

1. stop admitting new connections and requests;
2. cancel timers and stop new writes on each owning event loop;
3. detach read callbacks and close transports;
4. join CPU work that owns request/connection state;
5. drain required completion callbacks;
6. stop event loops and join I/O threads;
7. destroy buffers, callbacks, and service owners.

## Testing and common mistakes

Test parsers independently with fragmented chains: split every header and
payload at multiple boundaries. Test empty input, maximum lengths, malformed
lengths, shared buffers, and partial frames.

For networking integration, drive an event base deterministically where
possible and use loopback transports or socket pairs. Test connection failure,
EOF, write error, timeout, read pause/resume, and shutdown with callbacks
outstanding.

Common mistakes include:

1. assuming an `IOBuf` clone owns independent bytes;
2. mutating shared storage without unsharing;
3. coalescing every chain;
4. treating capacity as readable length;
5. trusting protocol lengths before applying limits;
6. calling event-base-affine objects from another thread;
7. blocking or doing heavy CPU work in an event callback;
8. destroying callback owners before transport completion;
9. buffering without an end-to-end backpressure policy;
10. stopping the event loop before connection cleanup callbacks can run.

## Further reading

- [`IOBuf` contract](https://github.com/facebook/folly/blob/main/folly/io/IOBuf.h)
- [`IOBufQueue`](https://github.com/facebook/folly/blob/main/folly/io/IOBufQueue.h)
- [`Cursor` and appenders](https://github.com/facebook/folly/blob/main/folly/io/Cursor.h)
- [`EventBase`](https://github.com/facebook/folly/blob/main/folly/io/async/EventBase.h)
- [`AsyncTransport`](https://github.com/facebook/folly/blob/main/folly/io/async/AsyncTransport.h)
- [`AsyncSocket`](https://github.com/facebook/folly/blob/main/folly/io/async/AsyncSocket.h)
- [Compression](https://github.com/facebook/folly/blob/main/folly/compression/Compression.h)
