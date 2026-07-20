# Context, configuration, and observability

[← Folly guide index](README.md)

## Contents

- [Production model](#production-model)
- [A progressive production-infrastructure tutorial](#a-progressive-production-infrastructure-tutorial)
- [Shutdown and failure policy](#shutdown-and-failure-policy)
- [Testing and common mistakes](#testing-and-common-mistakes)

## Production model

An asynchronous service needs more than execution and networking. Request
identity must follow work across threads, configuration must update coherently,
and operators need logs, metrics, traces, and diagnostic state that agree about
what happened.

```text
request boundary
   -> request context and deadline
   -> configuration snapshot
   -> Future/Task/executor work
   -> logs + metrics + trace events
   -> terminal outcome and cleanup
```

## A progressive production-infrastructure tutorial

### Step 1: propagate infrastructure context

`RequestContext` carries request-scoped infrastructure data such as trace IDs,
logging metadata, and baggage through many Folly scheduling boundaries.

```cpp
#include <folly/io/async/Request.h>

auto captured = folly::RequestContext::saveContext();

executor->add([captured = std::move(captured)] {
   folly::RequestContextScopeGuard guard(captured);
   continueRequest();
});
```

The scope guard installs the captured context and restores the previous one on
exit. Many Folly Future, coroutine, and executor paths propagate context
automatically, but custom queues, foreign runtimes, and manually created
threads may require an explicit bridge.

Do not use ambient context for core domain inputs or ownership. Function
parameters remain clearer for user IDs, request values, dependencies, and
results. Context is best for cross-cutting metadata whose plumbing would
otherwise obscure every API.

Avoid storing large objects or mutable unsynchronized state in request context.
Clear or replace context at request reuse boundaries so one request cannot leak
metadata into another.

### Step 2: expose configuration as immutable snapshots

Observers model a value that changes over time while readers obtain coherent,
immutable snapshots:

```cpp
#include <folly/observer/SimpleObservable.h>

folly::observer::SimpleObservable<Config> source{Config{}};
auto config = source.getObserver();

auto snapshot = config.getSnapshot();
serveWith(*snapshot);

source.setValue(loadUpdatedConfig());
```

An existing snapshot remains valid and unchanged after an update. Take one
snapshot for an operation when all decisions must use the same version; do not
re-read halfway through and accidentally mix configurations.

Derived observers track dependencies:

```cpp
auto limits = folly::observer::makeObserver([config] {
   return compileLimits(**config);
});
```

The dependency is recorded when the creator reads `config`; recomputation
happens when dependencies update. Creator functions should be deterministic,
bounded, and free of cycles. Define what happens when an external refresh
fails—usually retain the last known-good value and report the failure rather
than publishing a partial configuration.

### Step 3: optimize configuration reads only after measuring

Normal `Observer<T>` snapshots use shared ownership. Specialized readers trade
freshness, memory, or lifetime restrictions for lower read cost:

| Reader | Appropriate use | Trade-off |
| --- | --- | --- |
| `AtomicObserver<T>` | Frequently read lock-free atomic-compatible values | Limited value types; cache refresh path exists. |
| `ReadMostlyAtomicObserver<T>` | Extremely hot reads where slightly stale values are acceptable | Asynchronous freshness. |
| `TLObserver<T>` | Hot reads of larger values on a controlled thread population | Per-thread memory and stranded snapshots. |
| `HazptrObserver<T>` | Highly scalable short reads | Snapshot lifetime restrictions; do not carry across suspension. |
| `ReadMostlyTLObserver<T>` | Read scalability with snapshots that may live longer | Per-thread memory. |

Select these from a read-path profile and a written staleness contract, not
because they sound more concurrent.

### Step 4: produce structured, bounded logs

```cpp
#include <folly/logging/xlog.h>

XLOG(INFO) << "request completed; status=" << status;
XLOGF(DBG1, "decoded {} records", recordCount);
```

`XLOG` derives a category from the source location unless configured otherwise.
Disabled log sites avoid normal message construction, but expensive values
computed before the macro still incur cost.

Log terminal facts and actionable transitions. Include stable request/trace
identifiers through context or structured fields. Never log secrets, raw
credentials, or unbounded payloads.

Rate-limit repetitive failures and sampling-heavy debug events. Logging every
overload or disconnect can amplify the incident by consuming CPU, locks, disk,
and network bandwidth.

### Step 5: measure work at meaningful boundaries

For an asynchronous operation, separate:

- admission/rejection count;
- queue delay;
- active execution time;
- end-to-end latency;
- success, domain failure, exception, timeout, and cancellation;
- request/response bytes;
- current concurrency and queue depth.

A single average hides tail latency and overload. Use histograms or quantiles
with intentional bucket/range choices. Bound label cardinality: user IDs,
request IDs, raw URLs, and exception messages generally do not belong in metric
keys.

Record completion exactly once at the terminal owner. Counting at several
continuation layers can turn one request into multiple successes or failures.

### Step 6: connect traces to executor and I/O boundaries

A trace should represent logical causality, not merely OS-thread activity.
Annotate major queue waits, downstream calls, retries, timeouts, and executor
switches. Propagate the trace identity through request context, but pass span
ownership explicitly enough that every span has one completion.

Async stack and symbolization facilities help reconstruct logical stacks that
ordinary thread stacks cannot show. They are diagnostic tools, not substitutes
for clear ownership and event naming.

### Step 7: initialize and benchmark correctly

```cpp
#include <folly/init/Init.h>

int main(int argc, char** argv) {
   folly::Init init(&argc, &argv);
   return runApplication();
}
```

Initialize before reading flags or depending on configured logging. Keep
library code independent from process-global initialization where possible;
the executable boundary owns process policy.

Use Folly Benchmark for microbenchmarks, but prevent dead-code elimination,
warm relevant caches deliberately, measure several input sizes, and compare
against a baseline. A microbenchmark cannot establish service-level benefit
without queueing, allocation, contention, and tail-latency measurements.

## Shutdown and failure policy

Observability must survive long enough to report shutdown failures but must not
keep the async runtime alive forever. A typical sequence is:

1. stop admission and publish draining state;
2. request cancellation and join work;
3. record terminal outcomes;
4. close transports and stop executors;
5. flush bounded logging/metrics buffers;
6. stop observer refresh sources and diagnostic workers;
7. destroy process infrastructure.

Define behavior when telemetry backends fail. Request processing should rarely
block indefinitely on logging or metrics export. Use bounded buffers, dropping
policies, and internal health counters.

## Testing and common mistakes

Tests should install known context, publish controlled observer updates, and
capture logs/metrics through test sinks. Verify context restoration after
exceptions, snapshot consistency during updates, bounded labels, and one
terminal metric per request.

Common mistakes include:

1. using `RequestContext` as a hidden dependency container;
2. assuming every custom scheduling boundary propagates context;
3. taking several configuration snapshots inside one atomic decision;
4. doing blocking or failure-prone work in observer creators;
5. choosing thread-local observer caching without measuring memory;
6. computing expensive log arguments even when the site is disabled;
7. placing unbounded identifiers in metric labels;
8. counting both attempts and requests without distinguishing them;
9. ending spans from multiple racing completion paths;
10. destroying telemetry infrastructure before async work reports completion.

## Further reading

- [`RequestContext`](https://github.com/facebook/folly/blob/main/folly/io/async/Request.h)
- [`Observer`](https://github.com/facebook/folly/blob/main/folly/observer/Observer.h)
- [`SimpleObservable`](https://github.com/facebook/folly/blob/main/folly/observer/SimpleObservable.h)
- [`XLOG`](https://github.com/facebook/folly/blob/main/folly/logging/xlog.h)
- [`folly::Init`](https://github.com/facebook/folly/blob/main/folly/init/Init.h)
- [Benchmark support](https://github.com/facebook/folly/blob/main/folly/Benchmark.h)
