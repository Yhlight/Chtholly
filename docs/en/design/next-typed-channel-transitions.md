# Typed-channel SemIR/LowIR transitions

Status: internal ownership-transition IR (2026-09-02).

Typed channels now have append-only SemIR and LowIR instruction kinds for
send-prepare, send-commit, send-cancel, receive-acquire, receive-commit,
receive-cancel, and close. A prepare transition is intentionally inert in
PlaceState; only commit consumes the source place. Receive commit
reinitializes the destination place, while cancellation does not manufacture
an owner.

The instructions carry their semantic origin through lowering and are checked
against the corresponding LowIR origin. LLVM resolves the owner/payload
descriptor, materializes target-local lifecycle thunks, and emits the runtime
v1 transition call; a transition without a verified descriptor still fails
closed. The experimental `std::typed_channel` facade is the only source entry
point and does not alter the legacy byte-channel API.
