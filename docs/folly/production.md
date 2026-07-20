# Context, configuration, and observability

[← Folly guide index](README.md)

## Context propagation, configuration, and observability

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

