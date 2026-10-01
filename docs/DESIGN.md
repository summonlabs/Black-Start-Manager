# Black Start Manager: design

This document records the engineering decisions behind the boundary, the invariants the
implementation enforces, and the audits that were performed by reading the code rather than
by running it.

## 1. The decision the boundary makes

An adjacent owner answers "can I?" questions. Black Start Manager answers a different one:

> Given the facility state I am bound to, the plan I committed to, the evidence I hold, the
> attempts I have made, and the authority I can prove, what may be requested next — and
> what must stay isolated?

Every answer is derived, never stored. The committed record holds only facts: identity,
binding, plan digest, stage publications, admitted evidence, attempts, holds, replans,
fences, and the terminal record. Satisfaction, eligibility, gate readiness, deficits, and
the primary block are recomputed from those facts on every assessment, so two processes that
replay the same records cannot disagree about what the manager knows.

## 2. Determinism

- One canonical encoding (sorted keys, integers only, UTF-8 validated, no insignificant
  whitespace). Every digest is taken over canonical bytes.
- All identifiers are validated; distinct tag types make a session impossible to pass where
  an obligation is expected.
- Every ordering is explicit: stage order is derived from the obligation dependency graph
  and declared ranks, obligations inside a stage are ordered by priority then declaration
  then identity, evidence and attempts are ordered by the sequence they were committed in.
- Logical time comes from an injected clock; canonical output never contains wall-clock
  time. Checked arithmetic refuses overflow instead of wrapping.
- The derived plan, the session record, the assessment, and the report all have stable
  digests, so a test can assert that two independent derivations are byte-identical.

## 3. The compiled plan

Compilation validates the declaration and then derives the execution order:

1. Identity and structural validation: unique ids, known references, bounded sizes, evidence
   requirements with a source count between one and the supported maximum, a required owner
   always accompanied by its authority domain, no capability on an evidence-only obligation,
   and at least one readiness requirement per obligation (a plan step that proves nothing is
   a checklist item, and checklist items are what this boundary exists to replace).
2. Cycle detection over the obligation dependency graph by depth-first search, reporting the
   exact cycle path.
3. Stage precedence derived from obligation dependencies. A dependency that the declared
   ranks place later is refused (`plan_stage_order_conflict`) rather than silently
   reordered: a declaration that contradicts its own graph is a modelling defect, and hiding
   it would move the defect into a restoration.
4. In-stage ordering by priority, declaration order, then identity, with a check that a
   dependency inside one stage is ordered before its dependent. Without that check the
   strict in-stage rule would deadlock deterministically.
5. Per-obligation canonical digests and one digest over the whole derived form.

The compiled form round-trips: `from_json` reconstructs the declared document from the
declaration indices and recompiles it, verifying every stage index, obligation index, and
obligation digest, so a tampered derived form is refused with `integrity_mismatch`.

## 4. Evidence semantics

A record is admitted only when the plan requires that kind and subject, when the source is
part of the session's binding, when the source's authority generation is the bound one, when
the observation is not from the future, and when the value is a canonical scalar or object.
Its identity is the digest of its immutable content, so admitting the same observation twice
returns the same record (`replayed`) instead of a second one.

A record is *current* when it is live, its plan and binding digests and facility epoch match
the session, it is inside its age window, it is independent (a manager-derived value never
qualifies), its source generation is still the bound one, and — when the requirement says
so — it asserts the required readiness. Object expectations match field by field; an
observation that says "not ready" can therefore never be mistaken for "ready".

Requirement satisfaction needs the required number of independent current sources that
agree. Contradictory current sources fail the requirement with the offending records
reported, rather than being averaged away. A required owner or domain must be represented
among the sources; further sources are allowed, which is what makes a cross-check
independent.

Recovery demotes every live record to recovered and never promotes it back. Freshness is
re-established by observing again inside the acting incarnation.

## 5. Attempts and idempotency

A consequential request is a state machine with one durable identity per attempt:

    Requested ──▶ Acknowledged ──▶ (readiness evidence) ──▶ satisfied
        │                 
        ├──▶ Unresolved ──resolve──▶ Acknowledged | NotApplied | Refused | Failed
        ├──▶ Refused | Failed
        └──▶ Fenced (superseded plan, session, or terminal record)

The intent record is written and flushed before the request leaves the process, and the
idempotency key is a digest of the session, obligation, attempt sequence, plan digest,
binding digest, capability, and canonical parameters. A crash between the intent and the
answer therefore cannot produce a second mutation: the successor resolves the key against
the owner. The owner's own durable ledger is what makes the answer trustworthy across the
owner's restart as well.

An attempt that resolution proves was never applied does not consume the bounded budget,
because nothing consequential happened. An acknowledged attempt blocks a repeat request
until the readiness observation is recorded. An unresolved attempt blocks its obligation
until it is resolved. A replan never fences an in-flight attempt.

## 6. Authority and fencing

The session commits to a facility binding: facility, epoch, topology digest, policy revision,
incident and incident generation, one authority reference per (domain, owner) with a
generation and an attestation digest, and prerequisite generations per obligation.

Every operation that acts compares the committed binding with the observed one:

| Delta | Consequence |
| --- | --- |
| Higher authority generation, added authority, prerequisite bump, incident generation bump | `authority_stale`; an explicit re-establishment adopts the observed state |
| Lower generation, removed or renamed owner, changed attestation at the same generation, changed epoch, topology, policy, or incident | Fenced with the specific fence code; only a replan or an abort is available |

Evidence carries the binding digest and facility epoch it was observed under, so adopting a
new binding invalidates the evidence that was taken under the old one without any extra
bookkeeping.

Recovery also clears the "authority re-established" flag, because a new incarnation must
prove it holds the control authority it is acting under. Resolution of an unresolved attempt
is the one act allowed before re-establishment, because that is the recovery path.

## 7. Persistence

See `docs/FORMAT.md` for the byte layout. The design rules are:

- One commit point per record: write the frame, flush it. Nothing else makes a record
  authoritative.
- One commit point per generation: replace the manifest, which names exactly one snapshot
  and one journal segment with the snapshot's chain.
- Staged publication: write, flush, read back, decode, verify, then rename.
- Conservative recovery: a damaged *tail* is discarded and reported; damage anywhere else is
  refused, and a complete header with an unsupported version is never treated as a tail.
- Orphans from an interrupted publication are removed when a manifest exists that does not
  name them; a directory with artifacts but no manifest is ambiguous and refused.

## 8. Concurrency and the deadlock audit

The concurrency model is a single-writer process with two mutexes:

- `operation_mutex` serializes whole mutating operations and is held for the entire
  operation, including durable writes and controller calls.
- `state_mutex` guards the in-memory session table and is held only for short critical
  sections: copying a session out, applying an already durable record, or building a
  snapshot payload.

The audit below was performed by reading every call path, not by running tests.

| Hazard | Finding |
| --- | --- |
| Read → write reacquisition of the same lock | Not present. Read paths (`assess`, `report`, `status`) take only `state_mutex`, and no read path calls a mutating operation. |
| Write-lock re-entry | Not present. No mutating operation calls another public mutating operation; the private helpers they share (`commit`, `apply_journal_record`) never take `operation_mutex`. |
| Callbacks or event emission while internal locks are held | Not present by construction. `state_mutex` is released before any controller call, facility-state query, clock read, or filesystem write. The controller callback reentrancy test exercises this: a callback calling `assess` succeeds, and a callback calling a mutating operation is refused with `reentrant_operation` instead of deadlocking. |
| Nested acquisition with inconsistent ordering | Not present. The only two locks are always taken in the order operation → state, and `state_mutex` is never held while acquiring anything else. |
| Lock inversion | Not present. The store holds its own internal writer lock (`detail::ProcessLock`) for the process lifetime and never blocks on manager locks; the manager takes it once, at open. |
| Joining workers while holding locks needed by those workers | Not present. The library spawns no threads. The test support's service thread owns its own objects and is joined only after the shutdown flag is set and the listener is closed. |
| Shutdown waiting on work that cannot progress | Not present. `close` takes the operation mutex, closes the file handles, and releases the kernel lock; it waits for no other party. |
| Cancellation paths with reversed lock order | Not present. There is no cancellation path; a failed operation returns its error with the locks released by scope. |
| Helper methods that reacquire an already-held mutex | Not present. Helpers take a reference to the impl and never lock; locking happens only in the public entry points and in `commit`/`copy` helpers with explicit short scopes. The mutexes are non-recursive, so a mistake here would deadlock immediately and deterministically rather than silently. |
| References, views, or pointers outliving moved, temporary, or republished state | Audited and repaired. The derived-plan compiler held references into a document that had already been moved into the plan, which silently mis-assigned obligations to stages; the document is now copied and the plan owns it. `BSM_TRY_ASSIGN` binds a value out of a temporary `Result` whose storage outlives the binding. Session contexts are copied out of the map under the lock, so no reference into the map escapes it. |

## 9. Invariants the implementation enforces

- A session is terminal if and only if it carries a terminal record.
- A hold exists if and only if the session is held.
- Stage transitions advance monotonically in index and tick.
- An attempt's identity is the digest of its session, obligation, sequence, plan, and
  binding; a settled attempt carries the reply for its own key.
- Evidence identity is the digest of its immutable content, and its recorded digest must
  recompute.
- The manifest's digest covers everything except itself, and the snapshot it names must
  match its own sequence and digest.
- The journal chain links every record to its predecessor; a break is corruption.
- Counters, ticks, generations, and sequence numbers are range-checked on the way in and on
  the way out.

## 10. What was deliberately left out

- No transport timeout on the adjacent-owner call. A timeout would turn an unresponsive
  owner into an authority decision; instead the attempt stays unresolved and the manager
  says so.
- No automatic retry of an unresolved attempt. Resolution is a separate, explicit act.
- No multi-facility session. One session owns one facility binding.
- No vendor protocol, no device driver, no telemetry.
