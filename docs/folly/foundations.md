# Foundation types, memory, and containers

[← Folly guide index](README.md)

## Foundation types and value handling

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
