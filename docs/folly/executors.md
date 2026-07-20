# Executors and scheduling

[← Folly guide index](README.md)

## Executors: where runnable work runs

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

