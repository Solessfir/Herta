# EnTT adoption spike

## Scope

The opt-in `EnTT adoption spike` doctest evaluates EnTT v3.16.0, revision `b4e58bdd364ad72246c123a0c28538eab3252672`, against [EngineDesign.md](EngineDesign.md) section 4.6. It is a storage and integration probe, separate from the Scene implementation. Its results support adopting EnTT privately inside `FWorld`, not adopting a runtime scheduler.

Run the existing `HertaTests` executable with `--no-skip --test-case="EnTT adoption spike"`. Normal test runs skip this probe. There are no timing pass thresholds. Creation, structural churn, eight mixed-component iteration passes, hierarchy/clone, deferred mutation, and parallel query wall-clock times are printed. Every measured workload also checks data invariants.

## Workloads

- Homogeneous and fragmented component pools at 100,000 and 1,000,000 entities. Fragmented entities have alternating velocity membership, health on every third entity, and remove/reinsert position churn on every fifth entity.
- Explicit parent-before-child hierarchy evaluation, leaf reparenting, cloning with new identities and remapped parent handles, and independent clone mutation.
- Deferred destroy/create commands applied after query destruction. Old generational handles become invalid, and copied inspector rows survive the barrier without retaining storage references.
- Swap-and-pop removal relocates another entity's component. The test compares recorded address integers and reacquires the component; it never dereferences an invalidated reference.
- Preconstructed views and pools used by joined workers for read/write access to disjoint pools, writes to disjoint pools, and writes to distinct scalar fields of one component. No structural mutations or lazy pool creation occur during worker execution.
- EnTT organizer read/write dependencies, independent component writers, read-after-write and write-after-read conflicts, repeatable graph generation, and a stable ready-node execution policy.

## Measurements

The focused Windows Shipping run used MSVC `195136260` on an Intel Core i9-14900HX with 32 logical processors. All 4,613 assertions passed. These are single-run wall-clock samples, not benchmark medians or cross-platform guarantees.

| Entities | Component mix | Creation (ms) | Churn (ms) | Eight query passes (ms) | Query matches |
| --- | --- | ---: | ---: | ---: | ---: |
| 100,000 | Homogeneous | 4.777 | Not applied | 2.686 | 100,000 |
| 100,000 | Fragmented | 4.579 | 0.621 | 1.474 | 50,000 |
| 1,000,000 | Homogeneous | 50.438 | Not applied | 30.421 | 1,000,000 |
| 1,000,000 | Fragmented | 51.011 | 7.255 | 18.649 | 500,000 |

Hierarchy/reparent/clone of 1,024 entities took 0.382 ms. The 1,000-command deferred destroy/create barrier took 0.085 ms. Prepared parallel queries over 100,000 entities took 1.443 ms, including worker startup/join. These integration timings include their invariant checks. Fragmented queries match half as many entities, so their lower total time is not evidence of superior locality.

The run confirmed hierarchy remapping, clone independence, stale-handle rejection, inspector snapshot ownership, component relocation, parallel data invariants, and organizer dependency ordering. An isolated Linux Clang 23.1.1 build also passed all 4,613 spike assertions with Herta warnings treated as errors. That run used `-O0`, so its timings are not comparable to the Windows Shipping measurements. Race-detector validation remains unperformed.

## Adoption constraints

- EnTT sparse-set iteration is not an archetype/chunk locality contract. Fragmentation and multi-pool intersections need measurement against Herta's actual gameplay workloads; this probe is not a Mass comparison.
- Queries and component references expire at structural barriers. Paged allocation does not make swap-and-pop deletion, owning-group reordering, or compacting safe for retained pointers.
- Workers may only access prepared storage with declared, nonconflicting data accesses. The probe is a functional concurrency check, not a race-detector proof. Registry structural changes, signal callbacks, and lazy pool construction are outside its parallel contract.
- Organizer infers component-level access from constness. Distinct fields within one component still conflict at that level; Herta must either serialize them or declare a narrower validated access contract. Organizer builds a graph but supplies neither execution nor deterministic runtime scheduling.
- Hierarchy, stable serialized identities, cloning policy, inspector snapshots, and deferred commands remain Herta responsibilities. Do not serialize EnTT handles or expose registry/view/component storage types in public Herta APIs.

Recommendation: accept EnTT as the private storage implementation for the initial world milestone, subject to the contracts above and normal cross-compiler verification. The measured run found no functional blocker at the intended entity scales. It does not prove production scheduling, serialization, race freedom, or end-to-end gameplay performance, and does not justify exposing EnTT through public Herta APIs.
