# Fibers

[← Folly guide index](README.md)

## Fibers: stackful cooperative concurrency

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

