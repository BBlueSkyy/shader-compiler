# Maxwell surface atomic audit (2026-11-02)

Scope: the experimental array-coordinate commit `e194633c5fbb30073d728d170c37ec8d4507ba1b`
and SURED/SUATOM semantics. Pokémon Z-A previously threw `Invalid type 4`; the
array addition enabled progress to name input. A later Qualcomm compute pipeline
failure has no shader capture and is not established as a surface atomic problem.

## Sources and evidence

- [envytools Maxwell decoder](https://github.com/envytools/envytools/blob/master/envydis/gm107.c):
  `tabeb40_2` encodes SURED operations under mask `0x07000000`, i.e. bits 24–26.
  `tabea00_0` encodes SUATOM operations under `0x1e0000000`, i.e. bits 29–32.
  `tabeb40_1`, `tabeb40_3` and `tabeb40_4` establish dimensions, clamp and the
  SURED/SULD/SUST bound-selector polarity. This is disassembler evidence, not
  a complete hardware execution specification.
- Eden source snapshot `7a6d055094846b7787f9b8c0f238c0c68709f632`,
  `src/shader_recompiler/frontend/maxwell/translate/impl/surface_atomic_operations.cpp`:
  SURED op bits 24–26; same six dimensions, low unsigned 16-bit array layers,
  source register positions, bound/bindless handle fields, result distinction
  and existing integer subset. The existing local SULD/SUST implementation agrees
  with these coordinates. No coordinate rewrite was needed.
- [NVIDIA PTX surface reduction documentation](https://docs.nvidia.com/cuda/parallel-thread-execution/#surface-instructions-sured):
  reductions update memory without returning a value; min/max signedness depends
  on data type. PTX describes the higher-level operation, not Maxwell bit fields.
- [SPIR-V specification](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html):
  image atomic pointers, integer atomic operations and image dimensionality
  capabilities; executable verification uses SPIRV-Tools v2025.1 targeting
  Vulkan 1.1 plus SPIR-V 1.4.

## Findings and changes

The previous SURED operation field began at bit 21 and overlapped its size field
at bits 20–22. Real packed SURED MIN/U32 translated to ADD. Changing only its
operation field to bits 24–26 fixes that reproduction and keeps the size independent.
SUATOM's operation field was already correct. SURED has eight operations ADD, MIN,
MAX, INC, DEC, AND, OR, XOR; SUATOM additionally represents EXCH. CAS has a separate
opcode and is outside this change.

| Field | SURED | SUATOM |
|---|---|---|
| Operation | 24–26 | 29–32 |
| Dimension | 33–35 | 33–35 |
| Size | 20–22 | 51–53 |
| Clamp | 49–50 | 49–50 |
| Value operand | R[0–7] | R[20–27] |
| First coordinate | R[8–15] | R[8–15] |
| Result | none | R[0–7], previous value |
| Bindless selector | bit 51 clear | bit 54 set |
| Bindless handle register | 39–46 | 39–46 |
| Bound byte offset | bits 36–48 × 4 | bits 36–48 × 4 |

Dimensions 0–5 are 1D, buffer, 1D array, 2D, 2D array, 3D. Coordinates remain
`x`, `x`, `(x, low16(layer))`, `(x,y)`, `(x,y,low16(layer))`, `(x,y,z)` respectively,
in consecutive registers. Layer extraction is unsigned: high bits are discarded
and bit 15 does not sign extend. Signed spatial coordinate bit patterns remain
unchanged. Tests check both array dimensions, upper layer garbage, layers 65534
and 32769, maximum bound offset 0x7ffc, and coordinate/operand RZ boundaries.

The existing subset maps U32/S32/SD32 to R32_UINT; S32 selects signed MIN/MAX.
ADD and bitwise operations preserve all 32 operand bits. No float or 64-bit
precision is silently truncated: those sizes already throw and are tested.
The special SD32 mapping is preserved from the existing/Eden implementation;
descriptor-dependent hardware signedness is not established by this audit.

SURED emits the same side-effecting atomic IR as SUATOM, discarding its old value.
SUATOM writes that old value to its encoded destination; aliased sources are read
before writeback. DCE preserves atomic operations even when the return is unused.
TexturePass marks the image descriptor both read and written. Emission uses
`OpImageTexelPointer`, Device scope and zero/Relaxed memory semantics; atomics do
not introduce an acquire/release barrier. Relaxed scope/order are preserved,
not claimed as a newly verified Maxwell memory model.

All 1D and 1D-array modules initially failed `spirv-val` because image dimensionality
capabilities were missing. The backend now declares `Image1D` when a storage image
descriptor has either dimension. A descriptor's arrayed flag alone does not require
a new atomic extension. 32-bit R32ui image atomics require no float/int64 atomic
extensions. Vulkan support for the actual image format and usage remains the host's
responsibility; validation does not prove a particular GPU supports every format.

## Reproduction and verification

Run the commands in `tests/surface_atomics/README.md`.

- Before operation fix: `FAIL: wrong atomic opcode: reduction=true op=1 size=0 type=0 bindless=false`.
- After operation fix: 612 decoded/translated instruction cases pass, plus rejected
  unsupported sizes/types/clamps and register/offset boundary cases.
- Before Image1D capability fix: 52/156 emitted modules fail validation (all 1D/1D array).
- After capability fix: all 156 optimized modules pass operation/format/scope checks
  and `spirv-val --target-env vulkan1.1spv1.4`.

Remaining limits: INC/DEC reach explicit SPIR-V NotImplemented paths; clamp modes
other than IGN, float/64-bit sizes, dimensions 6/7 and CAS are unsupported. Bindless
coverage uses a trackable constant-buffer handle; arbitrary runtime handle selection
and descriptor arrays remain outside the supported backend subset. Tests use integer
descriptors and do not establish typed atomics over non-integer TIC formats, `.P`
variants, OOB hardware behavior, global memory ordering, or game/device execution.
The raw size enum's float/S64 numbering differs among disassembly references; this
change deliberately preserves the existing rejected encodings rather than guessing.
Neither fix is asserted to resolve the uncaptured Qualcomm pipeline failure.
