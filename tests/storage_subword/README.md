# Storage subword SPIR-V regression tests

This standalone target compiles the production SPIR-V backend and builds IR
programs containing U8/S8/U16/S16 loads and stores. It does not require Android.
It expects source checkouts of `fmt`, `sirit` (with its SPIRV-Headers submodule),
and `range` (range-v3) below `HOST_DEPS`, Boost headers, CMake/Ninja, Python 3,
and the SPIRV-Tools `spirv-val` executable.

```sh
cmake -S tests/storage_subword -B build-subword -G Ninja \
  -DHOST_DEPS=/path/to/deps -DBOOST_INCLUDE_DIR=/path/to/boost/include \
  -DSPIRV_VAL=/path/to/spirv-val
cmake --build build-subword
ctest --test-dir build-subword --output-on-failure
```

The subword target generates 576 modules covering SPIR-V 1.0, 1.3, and 1.4, all 32 combinations
of descriptor aliasing, arithmetic int8/int16, and storage8/storage16 support,
all four signed/unsigned subword operations, and mixed-width writable/read-only
programs. Each module accesses two SSBOs at immediate and computed byte offsets 0 through 7, including unaligned
halfword addresses whose low address bit is ignored by the existing backend.

Every module passes `spirv-val` against Vulkan 1.1 (plus SPIR-V 1.4 for that
version). Structural checks enforce native access only with both arithmetic
and storage support, storage capabilities only for native accesses, no plain
loads/stores in writable modules requiring emulation, no atomics in entirely read-only modules, no
unsupported integer types, and no variable-pointer capability for emulation.
A small interpreter executes the generated integer/memory instructions and
checks truncation, byte/halfword placement, neighboring bits, and a deterministic
concurrent neighboring update that forces every emulated store to retry once.
For a writable module, any unsupported used width routes all subword accesses
through 32-bit emulation, including atomic subword reads. This prevents native
8-bit/16-bit accesses from racing against word-sized CAS operations in the same
shader. Entirely read-only modules retain plain fallback reads. Synchronization
between an entirely read-only shader and a separate overlapping writable shader
remains the caller's responsibility; these fixtures do not establish cross-stage
alias safety.

This is compiler validation and deterministic instruction execution, not an
Android device or GPU driver integration test.

The same build also runs the image capability, fetch operand, texture numeric type,
and texture handle tracking suites through shared backend objects. The handle
tracking target runs the production TexturePass on IR under UBSan. The texture
numeric type suite validates 36 mixed Float/Uint/Sint modules containing 108
fetch, sample, gather, and gradient operations, including sparse result types
and bitcasts back to the float-typed register IR.
Use `-DTEST_STORAGE_IMAGES=OFF -DTEST_IMAGE_FETCH=OFF
-DTEST_TEXTURE_COMPONENTS=OFF -DTEST_HANDLE_TRACKING=OFF` to build only the
subword suite.

For a negative control against the original pinned compiler, extract that tree
and configure another build with `-DCOMPILER_ROOT=/path/to/original/tree` and
`-DTEST_STORAGE_FEATURES=OFF`. The unavailable native profiles fail validation because the old
compiler emits missing subword types/pointers; the original compiler does not implement
the fallback path.
