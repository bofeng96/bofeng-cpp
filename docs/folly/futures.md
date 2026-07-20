# Futures and promises

[← Folly guide index](README.md)

## Futures: callback-based asynchronous composition

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

