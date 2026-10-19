# Texture handle provenance: verified partial fix

## Demonstrated defect

The supplied original Maxwell for compute shader `0x55736CD732068324` is 5,400
bytes (SHA-256 `deac8f4297b8ec98e66dab4aff327760c87f38f4c8ca0487a14f8717d2f38c01`).
Its instruction fields agree with the complete captured SSA chain:

| Maxwell offset | Instruction | Meaning |
| --- | --- | --- |
| 0x38 / 0x50 | MOV from CBUF0[0x260 / 0x264] to R10 / R11 | GPU address, low / high word |
| 0x70 | LDG.E.32 R8, [R10:R11] | Load the memory **value** |
| 0x128 | LOP.OR R8, R8, CBUF6[0] | Combine the loaded value with sampler bits |
| 0x130 | TEX.B with metadata register R8 | Consume the resulting handle |

Before TexturePass, `%127 -> %133` supplies the address low word; `%138 ->
%139` aligns the bound SSBO base; `%140 = %133 - %139` is a byte offset, not a
handle. `%141 = LoadStorage32(0, %140)` supplies a new memory value. Identities
`%142 -> %212` preserve that value, `%213` ORs it with `%211 = CBUF6[0]`, and
`%219 -> %220` passes it to `BindlessImageSampleExplicitLod`.

GlobalMemoryToStorageBufferPass correctly retains the load while rebasing its
address. TexturePass's generic breadth-first search instead traversed that
address, accepted the aligned CBUF base and discarded the memory-value
dependency when rewriting the texture instruction. It could also recover one
CBUF branch after failing to represent the complete OR.

With the CBUF words reported in the capture notes, the fabricated expression is
`(0x6ec15200 & 0xfffffffc) | 0x10100000 = 0x7ed15200`: TIC 86528, TSC 2029.
The earlier 1772/2993 indices came from other observed state; this arithmetic
does not establish their exact individual raw handles.

## Implemented semantics

Tracking now resolves identities and interprets only explicitly modelled value
operations: CBUF reads, AND masks, left shifts, OR of representable sources,
and the selected lane of a U32x2 construction or CBUF pair. The second word of
a CBUF pair has byte offset +4, with overflow rejected. A failed branch cannot
be replaced by one of its dependencies. All unmodelled operations, including
loads and atomics, terminate tracking. Phi/select and other arithmetic not
modelled here are also explicitly unsupported; no incoming operand is chosen
arbitrarily. The generic BFS helper remains unchanged:
other passes intentionally use it to track **addresses**.

CBUF1 static masks retain their existing policy. Their value operand is checked
before reading the mask. A valid sibling expression can still read its own
static CBUF1 mask during collection; that is not an SSBO read or a texture
descriptor lookup. Unsupported handle collection does not query texture types,
emit new descriptors, rewrite texture accesses or modify existing descriptors.

Direct and transformed static CBUF descriptors and existing bounded CBUF arrays
keep their representation. No descriptor ABI or cache version changes. Tracking
no longer allocates a BFS queue/visited list for each expression. No GPU readback,
extra submission, synchronization or per-draw work is introduced.

## Explicit remaining limitation

**Handles loaded from SSBO/global/shared memory are not implemented by this
change.** They raise a specific unsupported-provenance exception before Vulkan
pipeline/descriptor creation. In particular, the captured MK11 shader now stops
at translation rather than executing with an invented CBUF handle. This change
alone does not make the game reach the menu or gameplay.

The current SPIR-V bindless emitters are unreachable stubs. TexturePass normally
eliminates bindless operations into descriptors whose metadata describes CBUF
locations. Strato resolves those locations on the CPU before dispatch. Adding an
SSBO address to that metadata or reading its contents on the CPU would not
preserve GPU execution order or values written by earlier invocations/dispatches.
Even an SSBO load at constant byte offset zero is not a constant handle.

A correct dynamic implementation needs coordinated compiler/runtime work:

1. Preserve the GPU-computed handle and split TIC/TSC in SPIR-V; respect the
   actual sampler-pool binding mode, including linked/header selection.
2. Provide separate image/sampler descriptor arrays with compatible dimensions,
   numeric types and comparison semantics. Populate all potentially indexed
   resources with correct residency, view layouts, barriers and lifetime.
3. Query and enable the real indexing features and enforce per-stage/set limits.
   Divergent indices need `shaderSampledImageArrayNonUniformIndexing` and the
   corresponding SPIR-V nonuniform decorations/capabilities. Runtime-sized arrays
   additionally need `runtimeDescriptorArray`; fixed arrays do not inherently
   need it. Variable-count/update-after-bind features are design-dependent.
4. Use descriptor sets when table sizes exceed push-descriptor limits. Avoid
   binding entire pools on ordinary static draws or recompiling for each handle.

The existing eight-entry CBUF arrays are not a GPU-indexed TIC/TSC pool. Neither
Adreno 650's name nor Vulkan 1.3 alone proves the necessary optional features or
resource limits. This patch does not enable unsupported features or claim a
universal dynamic fallback. Device feature/limit evidence and runtime pool
management must accompany that future implementation.

## Reference comparison

* The Yuzu-derived BFS in this compiler is a dependency search, not semantic
  proof of a handle value. Keeping it for address analysis does not justify it
  for value reconstruction.
* Eden's inspected `texture_pass.cpp` (blob
  `fe34a67555f6ccf652c757520e71c40d9a73ac84`, retrieved 2026-10-19) also uses
  BFS via Track/TrackCached. Its larger, limit-aware dynamic **CBUF** arrays do
  not establish support for an arbitrary SSBO-loaded handle. No claim is made
  about which shader path a particular Eden build uses for this game.
* Ryujinx's preserved BindlessElimination at commit
  `53d096e392d85106a41d8edad1dcda5cce7446a2` has a distinct GenerateBindlessAccess
  path: it keeps a loaded handle, splits it on GPU, and uses separate arrays
  sized from texture/sampler pools. This demonstrates the architectural work
  missing here. Its clamping/removal policies are not copied.

Sources:

* https://github.com/eden-emulator/mirror/blob/master/src/shader_recompiler/ir_opt/texture_pass.cpp
* https://git.axenov.dev/Museum/ryujinx/src/commit/53d096e392d85106a41d8edad1dcda5cce7446a2/src/Ryujinx.Graphics.Shader/Translation/Optimizations/BindlessElimination.cs
* https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceDescriptorIndexingFeatures.html
* https://docs.vulkan.org/guide/latest/descriptor_arrays.html

## Reproducible validation

The existing `tests/storage_subword` CTest suite includes the production
TexturePass regression executable under UBSan. Its negative cases reproduce
the captured memory-address confusion; without the fix, a storage load is
accepted as a CBUF descriptor. Tests cover both OR orders, subword/vector/shared
loads, atomics, variable offsets, aliases, masks, shifts, rejected expressions,
preservation of existing descriptors/accesses, and 256 shared static accesses.

Nineteen supported cases also pass through the production SPIR-V backend,
producing 57 modules for SPIR-V 1.0/1.3/1.4. `verify.py` runs `spirv-val` on every
module and checks that texture queries survive without descriptor-indexing
capabilities. Unsupported cases produce no module.

```sh
cmake -S tests/storage_subword -B build/host -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ \
  -DHOST_DEPS=/path/to/deps -DBOOST_INCLUDE_DIR=/path/to/boost/include
cmake --build build/host --parallel 2
ctest --test-dir build/host --output-on-failure
```

The full compiler also builds with Clang and pinned Strato dependencies in a
host parent supplying the common includes used by the Android parent. Replaying
the original Maxwell with the captured driver-CBUF constants reproduces the
complete pre-TexturePass SSA exactly after normalizing host pointer prefixes.
The old TexturePass accepts that program and creates the fabricated
`0x7ed15200` descriptor. The corrected pass rejects it during collection, before
any texture descriptor query. In full-program post-order the first unsupported
instruction is a Phi; the focused captured sample regression independently
stops at LoadStorage32. Do not assume a particular first unsupported opcode in
the device log.

Host validation establishes compiler behavior, not successful rendering,
performance or Vulkan validation on the Moto G100. Preserve the existing
targeted Maxwell/IR diagnostics for device follow-up.
