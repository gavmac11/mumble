# Shared file-transfer sending rate

The previous timer forced at least one chunk per 50 milliseconds and gave each job its own budget. Four private negative controls fail at the default rate, below one chunk per tick, at 32 KiB/s and with concurrent jobs.

One elapsed-time credit pool now charges complete ciphertext chunks across active sends. Round-robin dispatch shares the budget. Low rates accumulate credit before sending, late timer bursts are capped, and unlimited dispatch yields after a bounded batch. Owning job references and cancellation checks preserve an abort during synchronous transport callbacks. The UI describes the combined limit.

Six new regression rows pass; the focused run has eight passes including initialization/cleanup. Concurrent loopback jobs both reach Ready and save exact original bytes. The full Release client/server build and all 40 enabled suites pass in 49.15 seconds. The first broad run found an older fixture counting inactive timers; it was corrected to identify the active handshake timer, and the final full run is retained. No production timeout or assertion was relaxed.

This does not negotiate the server budget, prove recipient acknowledgements, bound GUI/TLS queues or qualify a slow healthy receiver. The byte budget covers ciphertext, excluding outer protocol/TLS overhead. Native cross-platform and the exact Opus patch review remain pending.
