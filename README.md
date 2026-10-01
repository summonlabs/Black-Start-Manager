# Black Start Manager

Black Start Manager is the DCCP boundary 56 runtime for governing the restart of facility
infrastructure from a fully or materially de-energized state. It owns the restoration
session, the dependency graph over restoration obligations, readiness gates, the evidence
that gates accept, bounded requests to adjacent owners, durable attempt identity, authority
and generation binding, stale-session fencing, and the final return-to-service proof.

The core question this boundary answers is:

> From this de-energized facility state, which infrastructure may be restored next, under
> which dependency, readiness, and authority evidence, what must remain isolated, and when
> may the facility advance to the next restoration stage?

A black-start plan is not actuation authority. Black Start Manager never switches a
generator, PDU, or UPS, never authorizes a feed, never actuates cooling, never recovers a
network path, never starts workload, and never speaks a vendor building- or power-management
protocol. It decides what may be requested next, records the bounded request, and refuses to
treat an acknowledgement as proof that anything physically changed.

## What it owns

- **Restoration session identity and lifecycle** — establishment under a bound facility
  state, hold, resume, replan, abort, completion, supersession, and the deterministic
  explanation of what may happen next.
- **The derived restoration plan** — stages, the dependency graph over restoration
  obligations, per-obligation readiness requirements, priorities, bounded attempt budgets,
  and the return-to-service proof, compiled into a deterministic execution order with a
  content digest.
- **Readiness gates** — stage entry and exit evidence, per-obligation evidence, prerequisite
  satisfaction, in-stage precedence, and the completion gate.
- **Evidence freshness and provenance** — live versus recovered evidence, expectations that
  an observation must assert, independent sources, cross-checked requirements, and the rule
  that recovered evidence never becomes current by itself.
- **Bounded requests and durable attempts** — one durable identity per attempt, a
  deterministic idempotency key, write-ahead intent, acknowledgement separated from
  observation and verification, explicit resolution of unresolved attempts, and bounded
  retries.
- **Authority and generation binding** — the exact facility epoch, topology digest, policy
  revision, incident generation, authority generations and attestations, and prerequisite
  generations a session committed to, plus stale-session fencing and explicit
  re-establishment.
- **Durable state** — an append-only journal of framed, chained records, periodic snapshots,
  one authoritative manifest, and recovery that refuses ambiguous or corrupt state.

## What it explicitly does not own

Generator, PDU, and UPS switching; feed authorization; transfer and synchronization
decisions; cooling actuation; network path recovery; workload or accelerator startup;
incident lifecycle; work-order and change management; generic facility recovery outside
black-start scope; vendor BMS/EPMS/DCIM protocols; and the internal execution, scheduling,
routing, or device actuation of any adjacent owner. Those belong to the adjacent owners this
boundary only requests effects from and accepts readiness evidence from.

## Architecture

    include/black_start_manager/   public headers (the whole boundary surface)
    src/                           implementation, including src/detail/ internals
    tools/                         bsm (operator command line), bsm_plant (owner host)
    tests/                         proof obligations, one executable per class
    examples/                      runnable walkthroughs
    bench/                         durability benchmark
    downstream/consumer/           out-of-tree consumer of the installed package
    docs/                          design and durable format notes

The public API is a small set of strongly typed value types and one manager:

| Header | Contents |
| --- | --- |
| `error.hpp` | `ErrorCode`, `Error`, `Result<T>`, `Status` |
| `canonical.hpp` | the canonical JSON value model, parser, writer, and field decoders |
| `digest.hpp` | SHA-256, CRC-32C, hexadecimal, and operating-system randomness |
| `ids.hpp` | validated identifiers and fixed-width identities with distinct tag types |
| `checked.hpp` | checked arithmetic that refuses overflow and underflow |
| `clock.hpp` | logical time: `Clock`, `ManualClock`, `SystemClock` |
| `authority.hpp` | authority references, facility binding, binding deltas |
| `evidence.hpp` | evidence requirements and records, provenance, deficits |
| `plan.hpp` | plan documents, obligations, stages, and the compiled derived plan |
| `attempt.hpp` | attempt identity, request envelopes, idempotency keys, controller replies |
| `session.hpp` | session records, stage transitions, holds, replans, fences, assessments |
| `controller.hpp` | the adjacent-owner interface, the synthetic plant, and test policies |
| `remote_controller.hpp` | the out-of-process owner client and host |
| `store.hpp` | the durable store: journal, snapshot, manifest, recovery |
| `manager.hpp` | `SessionManager`: the boundary's control surface |
| `report.hpp` | canonical reports |

Everything the library publishes is canonical JSON with sorted keys, no floating point, and
no dependence on map iteration order, hash order, thread timing, or wall-clock time. Two
processes that replay the same records produce byte-identical reports, assessments, and
digests.

## Lifecycle and the readiness gate

1. **Establish.** The operator compiles a plan and presents the facility state it is bound
   to plus the control authority that owns the restoration process. The binding must cover
   every owner the plan will ask, or establishment is refused (`binding_incomplete`).
2. **Assess.** `assess` is a pure function of the committed session record, the compiled
   plan, the observed facility binding, and the current logical tick. It returns the derived
   obligation states, per-obligation evidence deficits with the exact rejected evidence and
   why, stage entry and exit deficits, the deterministic eligibility order, and a primary
   block code with a short explanation.
3. **Request.** An obligation may be requested when its stage is current, its stage entry
   evidence is satisfied, every prerequisite is satisfied, in-stage precedence holds, no
   attempt is unresolved, an attempt remains within the bounded budget, the acknowledged
   effect is not waiting for verification, no current evidence contradicts the requirement,
   and the presented authority is the bound owner's authority at the bound generation.
4. **Observe and verify.** Readiness evidence must be live, current for the session's plan,
   binding and facility epoch, sourced from the bound authority generation, independent, and
   inside its age window. A requirement can demand that an observation assert specific
   readiness; an observation that asserts something else never satisfies it. A requirement
   for two or more sources is satisfied only by that many independent sources that agree,
   and contradictory current sources fail the requirement instead of being averaged away.
5. **Advance.** A stage is left only when all of its required obligations are satisfied and
   its exit evidence is current, and the next stage's entry evidence is current before it is
   entered. The transition is published durably with the evidence that justified it.
6. **Return to service.** Completion requires every required obligation to be satisfied by
   current evidence, the return-to-service evidence to be current, no attempt to be in
   flight, and the binding to be current. The proof digest over the satisfying evidence is
   recorded with the terminal record.

Missing, stale, recovered, or insufficient evidence never blocks a *request*: producing that
evidence is what the request is for. Contradictory current evidence does block a request,
and an acknowledged effect blocks a repeat request until the readiness observation is
recorded, because repeating it would be a duplicate consequential mutation.

## Authority, epochs, and fencing

Every session is bound to one facility epoch, one topology digest, one policy revision, one
incident generation, and one generation and attestation per adjacent authority. Authority is
never inferred from existence, observation, acknowledgement, an earlier successful decision,
recovered state, a cached value, a matching name, or apparent health: it is compared,
generation by generation, against what the session committed to.

- An **authority advance** (a higher generation, an added authority, a prerequisite
  generation bump) is not a contradiction. The session is refused with
  `authority_stale` until the operator re-establishes authority explicitly with
  `reestablish_authority`, which adopts the observed state in one durable record.
- A **contradiction** (a lower generation, a removed or renamed owner, a changed
  attestation at the same generation, a changed epoch, topology, or incident) fences the
  session with a specific fence code. It may be replanned or aborted, never re-established.
- **Replanning** binds a new plan and a new facility state to the same session. It refuses
  to move to a different de-energization epoch or incident, and it refuses a topology change
  unless the caller passes `accept_topology_change`. In-flight attempts are never fenced by
  a replan: the owner may have applied the effect, so they stay resolvable by key.
- **Recovery fences stale attempt tokens.** After a manager restart, every live evidence
  record is demoted to recovered evidence and every in-flight attempt becomes explicitly
  unresolved. Process authority is not inherited: the new incarnation must re-establish it
  before any consequential request, and an unresolved attempt is resolved by querying the
  owner, never by resending the request.

## Durable state and recovery

The store keeps one append-only journal of framed records, periodic snapshots of the whole
authoritative state, and one manifest that names exactly one authoritative generation.

- A record is **committed** when its frame is written and flushed. Each frame carries a
  header checksum, a payload checksum, and a chain digest over the previous chain, the
  header, and the payload.
- A generation is **published** when the manifest that names it is replaced atomically. The
  sequence is: write the snapshot, flush it, read it back and decode it, rename it into
  place, create the next journal segment, write the manifest, flush it, read it back and
  verify it, then replace the manifest. Everything before the replacement is provisional.
- **Recovery** verifies the manifest digest, the manifest's artifact names, the snapshot
  digest and its sequence, and the whole chain. A torn tail from an interrupted append is
  discarded conservatively and reported; a damaged frame with valid frames after it, an
  unsupported version in a well-formed header, a digest mismatch, a missing artifact, or an
  ambiguous directory is refused rather than guessed.
- A store whose manifest is missing but whose directory holds artifacts is **ambiguous** and
  refused: its authoritative generation cannot be known.
- **Single writer.** Mutation authority is an exclusive operating-system lock owned by the
  kernel. A second process is refused (`store_locked`), the abrupt death of the holder
  releases it, and the successor takes a new epoch and incarnation without inheriting the
  previous one's authority.

## Concurrency model

- Mutating operations are serialized by one operation mutex; the in-memory session table is
  guarded by a state mutex held only for short critical sections and never across file or
  controller I/O. Lock order is always operation mutex then state mutex.
- No external code is ever called while the state mutex is held. A controller callback may
  call `assess` (non-mutating) safely; a nested *mutating* call from inside a callback is
  refused with `reentrant_operation` instead of deadlocking.
- Reads (`assess`, `report`, `status`) never take the operation mutex, so they are
  available while a bounded request is in flight.
- The store treats a failed append as fatal to that operation: the in-memory state is
  applied only after the record is durable, so a crash between the two is recovered from the
  journal alone.

## Failure semantics

Every refusal is a typed `Error` with an `ErrorCode`, a message that names the gate, and a
canonical JSON detail where structure helps (the blocking obligation, the deficits with the
evidence that was rejected and why, the fence codes, the binding changes). The library never
throws across its boundary, never returns a partially applied outcome, and never mutates
state on a refusal. A crash leaves either the previous state or the committed record, and
recovery explains which.

## Validation performed

All of the following was executed on this host. **REAL** means genuinely exercised by the
operating system, filesystem, processes, or the installed package path. **SYNTHETIC** means
a modelled facility, owner, or evidence source.

- Unit, integration, and end-to-end lifecycle tests: **REAL** code paths, **SYNTHETIC**
  facility and owner. Ten CTest executables, 84 tests, all passing in Release and in Debug
  with zero first-party warnings (`/W4 /WX /permissive-`).
- Randomized state machines with printed seeds and an independently written model of the
  documented eligibility rules: 24 seeded mutations per run plus a randomized plan-document
  stream, with invariants checked after every mutation: **REAL**, SYNTHETIC facility.
- Adversarial tests: malformed, truncated, corrupt, contradictory, replayed,
  boundary-shaped, and out-of-range input; independent frame encoders in the tests:
  **REAL**.
- Persistence tests: torn tails, damaged frames, tampered manifests and snapshots,
  truncated snapshots, unsafe and pathological paths, second-writer refusal, compaction
  rotation, orphan cleanup, and conservative recovery: **REAL** filesystem and processes.
- Crash injection at named durable boundaries with the manager killed by the operating
  system and reopened by an independent process: **REAL** process death, SYNTHETIC plant.
- Multiprocess proofs: a second writer refused, an abruptly killed holder releasing the
  kernel lock, a successor taking a new epoch and incarnation, an owner whose durable ledger
  survives being killed, and no duplicate consequential effect: **REAL**.
- The operator command line driven as a real process against a real out-of-process synthetic
  owner, including a complete restoration to return-to-service proof: **REAL** processes,
  SYNTHETIC plant.
- Package validation: install into a clean prefix, then configure, build, and run an
  independent out-of-tree consumer with `find_package`: **REAL**.
- Runtime checking: the whole suite also passes under MSVC AddressSanitizer
  (`-DBLACK_START_MANAGER_ENABLE_ASAN=ON`, Debug): 10 of 10 suites, no report.
- Static analysis: MSVC `/analyze` over the library reported one first-party warning
  (C28020 on the bounds of a fixed 256-entry lookup table, where the loop condition is
  itself the proof of the bound) and one finding inside the Windows SDK's `ws2tcpip.h`.
  Both are recorded rather than suppressed; no meaningful first-party finding remains.
- Fresh-clone closure: cloned from the configured remote at the release commit, configured,
  built, and tested in Release and Debug, installed, and consumed out of tree by the
  downstream consumer.

### Not validated

- No real facility hardware, generator, PDU, UPS, cooling plant, BMS, DCIM, or EPMS was
  exercised. All plant and owner behaviour is SYNTHETIC.
- No accelerator, RDMA, InfiniBand, NVLink, or multi-host fabric was exercised.
- The out-of-process owner communicates over loopback TCP on one host; no remote or
  multi-host deployment was tested.
- The POSIX branches of the process-control test support and the socket layer were
  syntax-checked but not executed on this Windows host.

## Building

Requirements: CMake 3.21 or newer and a C++20 compiler. The primary exercised platform is
Windows with MSVC (Visual Studio 2022, toolset 19.44 or newer). There are no third-party
dependencies and no network access at configure, build, or test time.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Options: `BLACK_START_MANAGER_BUILD_TESTS`, `..._BUILD_EXAMPLES`, `..._BUILD_BENCHMARKS`,
`..._BUILD_TOOLS` (all ON by default in a top-level build),
`BLACK_START_MANAGER_WARNINGS_AS_ERRORS` (ON), and `BLACK_START_MANAGER_ENABLE_ASAN` (OFF).

## Installing and consuming

```sh
cmake --install build --prefix /some/prefix
```

```cmake
find_package(BlackStartManager 1.0 REQUIRED)
target_link_libraries(application PRIVATE BlackStartManager::black_start_manager)
```

An independent consumer lives in `downstream/consumer` and is exercised by the
`bsm_test_downstream_install` CTest check when
`-DBLACK_START_MANAGER_DOWNSTREAM_PREFIX=<prefix>` is set.

## Using the library

```cpp
#include <black_start_manager/controller.hpp>
#include <black_start_manager/manager.hpp>

using namespace black_start_manager;

ManualClock clock(1);                       // logical time, supplied by the caller
InProcessSyntheticController owner;         // SYNTHETIC adjacent owner
StaticFacilityState facility(binding);      // the observed facility state
ManagerOptions options;
options.store_root = "bsm-store";
options.create_store_if_missing = true;
options.clock = &clock;
options.controller = &owner;
options.facility = &facility;

Result<SessionManager> manager = SessionManager::open(options);
EstablishSessionRequest establish;
establish.plan = plan;                      // a parsed PlanDocument
establish.binding = binding;                // the facility state the session binds to
establish.authority = control;              // the bound control authority
Result<SessionView> session = manager.value().establish_session(establish);
Result<Assessment> assessment = manager.value().assess(session.value().id);
```

`Assessment` is the explanation surface: per-obligation state, the evidence deficits with
the rejected evidence and the reason, stage entry and exit deficits, the eligible
obligations in deterministic order, the completion deficits, a primary block code, and a
short sentence.

## Command line

```sh
bsm open     --store DIR --create --state @state.json --plan @plan.json --authority @control.json --tick 1
bsm assess   --store DIR --state @state.json
bsm request  --store DIR --state @state.json --obligation isolation-a \
             --adopt @control.json --authority @electrical.json --controller tcp:PORT
bsm observe  --store DIR --state @state.json --kind isolation_verified --subject isolation:a \
             --obligation isolation-a --observer electrical-owner \
             --adopt @control.json --authority @electrical.json --controller tcp:PORT
bsm run      --store DIR --state @state.json --plan @plan.json \
             --adopt @control.json --authority @control.json --authority @electrical.json \
             --controller tcp:PORT
bsm report   --store DIR --state @state.json
bsm verify   --store DIR
```

`bsm run` drives a restoration inside one incarnation: it requests the first eligible
obligation, observes the readiness requirements through every owner the caller presented,
publishes stage transitions, acquires the return-to-service proof, and completes the
session.

Because the command line performs one bounded operation per process, every command is a new
incarnation; recovered evidence is never current, which is why an acting command adopts the
observed state with `--adopt` and re-acquires the evidence it needs. `bsm_plant` is the
out-of-process synthetic owner: it keeps a durable ledger of applied request keys and of the
plant state it models, so killing and restarting it never loses the identity of a request
that was already applied.

```sh
bsm_plant --store OWNER_DIR --port 0 [--lose-reply CAP] [--refuse CAP] [--unavailable-once CAP]
```

## Durability benchmark

`bench_durability` measures completed operations, including the durability each one pays:
one flushed journal append per record, one flush plus atomic replacement per snapshot
publication, and one durable evidence admission per session operation. Provenance is REAL
filesystem durability on this host with a SYNTHETIC facility and owner. It reports
operations per second and bytes flushed per operation, and never measures submission
latency.

## Limitations

- The manager calls the adjacent owner synchronously and has no transport timeout: an owner
  that stops answering leaves the attempt unresolved, which the manager reports and refuses
  to repeat. Liveness is the deployment's responsibility, not an authority decision.
- Exactly one active session per facility is modeled. Parallel restorations of independent
  parts of a facility require separate facilities with separate bindings.
- Attempt budgets, evidence age, and deadline windows are bounded by fixed limits; a plan
  that needs larger values is refused rather than silently clamped.
- The synthetic plant models a small, fixed capability set. Adjacent owners with different
  capabilities are integrated by implementing the controller interface, not by extending the
  plant.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
