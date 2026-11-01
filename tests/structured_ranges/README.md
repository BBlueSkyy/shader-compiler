# Structured Compute SSBO bounds

The normal affine analysis remains the first path. For an unknown, unsized Compute pointer, a second metadata-only analysis proves positive-step loop induction and correlates conditional Phi inputs with the conditions at each memory access. It never changes translated instructions or pointer/descriptor classification.

## Proof boundary

* Reconstruct the emitted ASL control-flow edges and require exact agreement with the IR predecessor/successor graph and Phi inputs. Require a rooted, reachable graph; every cycle must cross an explicit Repeat edge.
* A loop header must dominate its unique latch, have exactly one entry and one Repeat backedge, and have a literal U32 entry value. The recurrence is `phi + positive_literal`; the necessary continuation condition compares that exact next value against a literal upper threshold. Other header counters are bounded only when their own self-recurrence has the same entry, start and step. Reject unsigned wrapping or crossing the signed comparison domain.
* Boolean facts are must-facts, intersected at joins. A Phi input can be omitted only when its incoming edge is unreachable or contradicts the access context. Both Repeat edges discard facts: the SPIR-V backend's persistent safety counter can exit while the guest condition is true. Unproved cyclic Phis remain unknown.
* Scalar ranges are upper bounds, not exact values. They cannot multiply the symbolic pointer base. Only exact constant expressions may do that. Nonnegative scalar/coordinate contributions are conservatively combined; overflowing extents stay unavailable to `GetSize()`.
* Include the full width of every recognized read/write. All accesses to a descriptor must succeed. Atomics, unknown steps/thresholds, unsupported expressions and graphs, ambiguous pointer coefficients, and exhausted analysis budgets retain the existing unknown-size fallback.

Analysis limits are 512 blocks, 32,768 instructions, 65,536 arguments, 64 facts, 64 solver sweeps, 64 expression depth, 4,096 offset visits per access and 128 boolean visits per query. Identity chains are checked before using recursive IR helpers. Losing a fact weakens the proof; an incomplete induction/bound returns unknown. These limits affect compilation only.

## Run synthetic safety tests

Supply pinned fmt, boost and range-v3 sources, e.g. the parent Strato `app/libraries` directory:

```sh
python3 tests/structured_ranges/run.py --deps /path/to/Strato/app/libraries
SANITIZERS=address,undefined ASAN_OPTIONS=detect_leaks=0 python3 tests/structured_ranges/run.py --deps /path/to/Strato/app/libraries
```

The tests execute the actual global-memory pass on constructed IR/ASL, including exhaustive loop maxima, lockstep counters, split multiplication, signed predicates, overflow, vector widths, writes, unknown atomics, correlated/unrelated Phi paths, safety-counter exits, malformed graphs, bounded boolean DAGs and raw 8-byte pointers with affine Compute ranges. A pointer multiplied by an induction variable must remain unknown even if its upper bound is one.

## Replay the captured regression

The hex fixtures preserve the already captured 4,912-byte Maxwell input and baseline 30,896-byte SPIR-V. The replay adapter supplies only the recorded compilation inputs and throws on unexpected memory/texture requests. No game/hash/offset certificate exists in production code.

```sh
cmake -S tests/structured_ranges -B build/structured-real -G Ninja \
  -DDEPS=/path/to/Strato/app/libraries \
  -DSIRIT_SOURCE=/path/to/sirit
cmake --build build/structured-real -j2
python3 tests/structured_ranges/verify_real.py --executable build/structured-real/real
```

Requires SPIRV-Tools (`spirv-val`, `spirv-dis`) with Vulkan 1.1 + SPIR-V 1.4 support. The verifier asserts the real descriptor extent is 4,608 bytes, the second remains four bytes, validates both modules and compares all executable SPIR-V instructions. Only capability order and an exact permutation of constant IDs/declarations may differ. A deliberate arithmetic mutation must fail the comparer.

For the captured pattern, the proof derives 32 records, stride 144 and the last 16-byte access at offset 128: `(32-1)*144+128+16 = 4608`. This is derived from IR constants, recurrences and path conditions, never CBUF size words. The host still adds pointer alignment padding and respects the device range limit. Unknown cases keep the conservative fallback. This is compile-time validation, not an FPS or Marvel hardware benchmark.
