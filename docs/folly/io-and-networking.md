# Byte ownership, event loops, and networking

[← Folly guide index](README.md)

## Byte ownership and zero-copy I/O

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

## `EventBase`: the I/O reactor

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

