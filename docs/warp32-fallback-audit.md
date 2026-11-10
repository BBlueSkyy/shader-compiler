# Guest Warp32 and unsupported-stage fallbacks

Baseline: `a987e2a5` (Strato #430), parent master `f0a95dd8`. This is a bounded
improvement to scalar fallback semantics, not a software implementation of a warp.

## Invariants

`Profile::SupportsSubgroupStage` continues to gate capabilities, builtins and every
warp operation. `FSwizzleAdd` must use guarded `GetThreadId()`. Unsupported stages
must not read `SubgroupLocalInvocationId` or emit any Vulkan subgroup operation,
including when shuffle emulation or disable-subgroup-shuffle settings are enabled.
Supported-stage code is unchanged.

## Operation audit

Here `l` is a guest lane in `[0,31]`, `A` is the active-lane mask at the instruction,
and `P` contains the input predicate for each active lane. The executing lane is
active, so `A` is nonempty. Masks of lane positions are independent of `A`.

| Operation | Guest result | #430 without subgroup | Exact cases and decision |
| --- | --- | --- | --- |
| LaneId | Position `l` within the warp | `0` | No general graphics-stage lane mapping is available. Preserve. |
| VoteAll | All active predicates true | Local predicate | Exact for a proven uniform predicate, including literals. Already exact then; preserve. |
| VoteAny | Any active predicate true | Local predicate | Same uniform case; preserve. |
| VoteEqual | Active predicates all agree | `true` | Exact for uniform predicates; preserve for unknown ones. |
| Ballot | Predicate bits at active guest positions | Local predicate ? `1` : `0` | A false uniform predicate already gives exact zero. True still needs `A`; never assume `0xffffffff`. Preserve. |
| EqMask | `1u << l` | `1` | Needs lane identity; preserve. |
| LtMask | Positions below `l` | `0` | Needs lane identity; preserve. |
| LeMask | Positions through `l` | `1` | Needs lane identity; preserve. |
| GtMask | Positions above `l` | `0` | Even real lane zero gives `0xfffffffe`, not zero. Changing this alone is inconsistent with the existing approximations; preserve. |
| GeMask | Positions from `l` through 31 | `1` | Even real lane zero gives `0xffffffff`; preserve the stable scalar approximation. |
| ShuffleIndex | Source selected by index and segment | Own value, validity false | Promote validity only when controls are literal and the complete scalar result is proved exact for all 32 lanes. |
| ShuffleUp | Source at lane minus offset, subject to clamp | Own value, validity false | Same proof; otherwise preserve. |
| ShuffleDown | Source at lane plus offset, subject to clamp | Own value, validity false | Same proof; otherwise preserve. |
| ShuffleButterfly | Source at lane XOR offset, subject to clamp | Own value, validity false | Same proof; otherwise preserve. |
| FSwizzleAdd | Local floating arithmetic with a selector from the lane's low two bits | Same arithmetic, selector for lane zero | No communication itself, but lane-dependent selector. Repeated selector slots are already exact; otherwise retain guarded lane-zero fallback. |

## Conservative shuffle proof

SHFL has two results: data and a validity predicate. Correcting only validity while
the scalar data is still approximate could change branches to consume wrong data.
Therefore the proof accepts a true predicate only if, for **every** possible lane:

1. The source is in range according to the existing guest clamp/segment rules.
2. The source is that same lane, or the data argument resolves to an IR literal.

The controls must resolve to five-bit literals. The proof uses their exact IR
values and refuses values outside `[0,31]`; it does not silently normalize dynamic
inputs or change the supported-stage emitter. A literal data value is the same in
every participating lane, so returning the local copy preserves its bits. Reading
an inactive source is architecturally undefined; this proof does not invent data
for that case. Self transfers remain defined for any active-lane mask.

Examples accepted: Index with segment mask 31 (always self); Down with offset 0 and
clamp 31; Up with offset 0 and clamp 0; Butterfly with offset 0 and clamp 31; Index
of literal data with index 9, clamp 31 and segment 0.

Counterexamples retained: Index of varying data from another lane, nonzero Up/Down
with lane-dependent validity, Butterfly of varying data from another lane, and all
dynamic control arguments. Uniformity inferred from a register name, a constant
buffer, or a draw-wide appearance is insufficient: no new uniformity analysis is
introduced. General proven-uniform expressions could extend this later.

## Why the other approximations remain

VertexIndex, InstanceIndex, primitive ID, tessellation IDs and fragment coordinates
do not supply a portable guest warp position or active mask. A guessed `ID & 31`
would make lane-dependent FSwizzleAdd and masks change unpredictably. Shared memory
does not provide a general graphics-stage replacement, and a multi-invocation
software warp would require a different execution architecture and synchronization.

Constant/uniform arguments alone do not solve Ballot(true), position masks or
lane-dependent FSwizzleAdd: the active mask or lane may still be needed. In contrast,
constant votes and repeated FSwizzleAdd selectors already have the right scalar
answer, so changing them would add no fidelity. Existing derivative-pattern folding
in `FoldFSwizzleAdd` is not broadened to other stages or unproved patterns.

## Tests and limits

`tests/warp32/warp32.h` is an independent test-only CPU oracle with explicit active
lanes, ballots, votes, position masks, the four shuffles and finite-value FSwizzleAdd
examples. Undefined shuffle sources are represented by an empty optional, not zero.
It is not an IEEE floating-point, FTZ or rounding conformance model.

`tests/warp32/run.py` tests actual SPIR-V warp emitters against all 262,144
combinations of mode, literal/varying data and five-bit controls. Dynamic controls,
non-five-bit controls, partial active masks, mixed votes and selector slots have
separate cases. SPIR-V fixtures are validated with `spirv-val`; static validation
does not prove physical-device stage support, so unsupported-stage modules are also
checked for the absence of subgroup operations, capabilities and builtins.

Validation on this branch (GCC 14, UBSan, SPIRV-Tools 2025.1):

- 262,144 constant-control comparisons passed, plus dynamic/oversized argument and
  sparse-active-mask checks. The initial identity-Down test failed on #430 and passed
  after the bounded proof was added.
- All 102 full-compiler fixtures passed universal SPIR-V 1.3 validation.
- All 78 unsupported-stage fixtures passed Vulkan 1.1 validation and contained no
  subgroup capabilities, builtins or instructions. These include all six emitted
  stages, 32/64 host-size profiles, vote support on/off, shuffle quirks on/off, and
  FSwizzleAdd-only shaders.
- All 24 supported-stage modules were byte-identical when linked against the
  baseline `a987e2a5` warp emitter. Sixteen passed Vulkan validation. Eight reproduce
  existing Vulkan interface errors: Vertex `Flat` on `SubgroupLocalInvocationId`
  (`VUID-StandaloneSpirv-Flat-06202`) and Fragment subgroup mask inputs missing
  `Flat` (`VUID-StandaloneSpirv-Flat-04744`). Those interface changes are outside
  this patch; the runner reports only these specific, baseline-identical failures.
- Supported-stage `disable_subgroup_shuffle` with a dynamic broadcast source is
  outside this fixture's validation coverage; its existing path is unchanged.

Run from the Strato checkout after recursive submodule initialization:

```sh
python3 app/libraries/shader-compiler/tests/warp32/run.py \
  --deps app/libraries --build-dir /tmp/warp32-host \
  --baseline-ref a987e2a5c8fa507c805d5867737de5976c7c2ce0
```

The baseline commit must be present in the compiler's Git object database. The PR
workflow fetches that commit explicitly for shallow checkouts. `spirv-val` and
`spirv-dis` must be on PATH, or supplied with `--spirv-tools /path/to/bin`.
`--boost-include` can select system Boost headers instead of the recursive Boost
submodule. GCC 14's diagnostic in the pinned old range-v3 can be made nonfatal with
`CXXFLAGS=-Wno-error=changes-meaning`; this does not change production sources.
The runner forces C++ assertions on even if inherited flags contain `-DNDEBUG`.

The existing subgroup-stage contracts, Eden SPIR-V semantic checks, structured SSBO
ranges, parent SSBO binding, null descriptor and sampled-layout regressions also
passed locally. On this host the ASan regressions needed leak detection disabled
(ptrace limitation); the old shared-pointer atomic APIs needed their GCC 14
deprecation warning made nonfatal. Neither compatibility adjustment changes code.

Device regression remains required on Snapdragon 870 / Adreno 650 with both Turnip
and Qualcomm proprietary drivers. These are user-reported patterns, not replayed
game shaders or claims of device validation in this PR:

| Game | Regression focus | Status |
| --- | --- | --- |
| Zelda: Echoes of Wisdom | LaneId, Ballot, masks, dynamic shuffles remain stable | Pending device test |
| Luigi's Mansion 2 HD | ShuffleIndex + guarded FSwizzleAdd | Pending device test |
| Pac-Man World Re-Pac | Same operations; preserve performance and rendering | Pending device test |
| The Patrick Star Game | ShuffleIndex + guarded FSwizzleAdd | Pending device test |
| NFS Hot Pursuit Remastered | Preserve #430 behavior | Pending device test |

## References

- [NVIDIA PTX 6.0, legacy SHFL and VOTE](https://docs.nvidia.com/cuda/archive/9.0/parallel-thread-execution/)
  provides the lane/segment/clamp and active-predicate reference. The formulas are
  also present in this compiler's existing supported-stage emitter.
- [Khronos subgroup guide](https://docs.vulkan.org/guide/latest/subgroups.html): check
  physical-device stage support separately from operation support.
- Local sources: `frontend/maxwell/translate/impl/{warp_shuffle,vote,floating_point_swizzled_add}.cpp`,
  `backend/spirv/emit_spirv_warp.cpp`, `ir_opt/constant_propagation_pass.cpp`.
