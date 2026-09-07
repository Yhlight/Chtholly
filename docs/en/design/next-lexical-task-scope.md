# Next Lexical Task Scope

Status: implemented for source semantics, path-sensitive ownership,
nonblocking runtime groups, LowIR frame plan v11, LLVM lowering, and native
normal, error, and cancellation execution.

## Source Form And Boundary

The contextual statement is:

```chtholly
task scope {
  work();
}
```

It is valid only in an `async fn` body and is forbidden in `defer`. `task` and
`scope` remain ordinary identifiers outside this statement position. The
statement exposes no group handle. There is no source detach, manual executor,
manual cancellation request, or unstructured task constructor.

An async call in the body creates an eager child and attaches it to the
innermost active lexical group after successful construction and before its
Task value is exposed. Nested scopes create distinct groups. Children created
by dynamic loop iterations attach independently; the implementation does not
derive a static child count from syntax.

## Exit Matrix

| Exit from the scope | Child action | Continuation |
| --- | --- | --- |
| fallthrough, `return` success, `break`, `continue` | close and drain without cancellation | continue the selected edge |
| declared error or `?` propagation | request cancellation, close, drain | preserve and publish the original error |
| cancellation | request cancellation, close, drain | continue cancellation |

Drain means nonblocking quiescence: the coroutine arms group completion and
suspends its task when children remain. It never blocks an executor thread.
An external cancellation observed while any drain is pending requests child
cancellation. A cancel-drain does not reinterpret the child cancellations it
requested as a replacement for an already-selected error edge; an independent
owner cancellation still wins before terminal commit.

Group quiescence is ordered before the scope's `defer` bodies, local
destruction, and lifetime-extended temporary destruction. This is expressed
as a structured control-flow edge into the existing cleanup graph, not as a
special destructor or synchronous runtime join.

## Task Ownership

A success-only child may be discarded directly inside `task scope`; this
transfers its completion obligation to the group. A fallible child must still
be consumed explicitly with `wait`, so an error cannot disappear into implicit
group ownership. Explicitly named Task values remain move-only.

Every Task records lexical-group provenance in place-state analysis. Moving or
assigning a Task transfers both its completion obligation and provenance. A
Task cannot be returned, stored through an external place, or assigned to a
local declared outside its owning group. These rules prevent a handle from
remaining usable after group release.

Because drain is a possible suspension, a reference live from before the
checkpoint to a later use is rejected with
`chtholly.next.sem.async.reference-across-task-scope`. This reuses the
coroutine CFG liveness proof rather than approximating liveness from lexical
nesting.

## Compiler Representation

`SemCoroutineTaskScope` owns the checked body block. LowIR makes group
ownership and every transition explicit: create, attach, request cancellation,
close, arm completion, normal-drain, error-drain, cancellation-drain, query,
and release.

Normal, error, and cancellation drains are distinct LowIR checkpoint kinds.
This prevents LLVM from guessing exit intent from adjacent instructions and
preserves error-versus-cancellation precedence when cleanup graphs are shared.

Frame plan v11 records task-group exit intent, accepted cancellation causes,
child escalation policy, and the acknowledgement boundary. An inner normal
drain that observes unexpected child cancellation completes its own
quiescence, makes owner cancellation sticky, and resumes the structured
continuation that cancellation-drains the enclosing groups. Verification
reconstructs these facts from instruction kind, cross-edge liveness, frame
values, wake slots, regions, and semantic origin. A group is released on the
ready path, resumed path, cancellation cleanup, and frame destruction exactly
once.

Runtime ABI v1 uses opaque task groups with retained members and nonblocking
completion callbacks. Closing is sticky, cancellation requests linearize with
late attachment, and completion becomes ready only after the closed group has
zero active children. Protocol failure uses coroutine trap reason 8. No group
type or operation is public source ABI.