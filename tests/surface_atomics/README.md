# Surface atomic translation tests

The C++ fixture passes packed Maxwell instructions through the production decoder
and `TranslatorVisitor`. It checks 612 combinations of SURED/SUATOM, bound/bindless
handles, all six dimensions, U32/S32/SD32 sizes, and all representable operations.
It checks source/destination register placement, byte offsets, signed min/max IR,
array layer extraction, rejected unsupported sizes/types/clamps, and RZ edges.

156 representative cases continue through the production SSA, constant propagation,
texture descriptor, dead code and shader information passes into the SPIR-V backend.
Unsigned array layers with bit 15 set and high bits set are checked after folding.
Unused atomic results must retain read/write descriptor effects and atomic IR.
The Python verifier checks actual SPIR-V operation numbers, R32ui image format,
Device scope and Relaxed semantics, and runs `spirv-val` for every module.

```sh
cmake -S tests/surface_atomics -B build/surface-atomics \
  -DDEPS=/path/to/Strato/app/libraries -DCMAKE_BUILD_TYPE=Debug
cmake --build build/surface-atomics -j4
ctest --test-dir build/surface-atomics --output-on-failure
```

Requires fmt, range-v3, Boost headers, sirit and SPIR-V headers. `EXTRA_INCLUDE`
can point to system Boost headers. With system SPIR-V headers, pass
`-DSIRIT_USE_SYSTEM_SPIRV_HEADERS=ON` and their CMake prefix if necessary.
If `spirv-val` is absent at configure time, CTest runs the C++ fixture only;
the validation test is registered when Python and `spirv-val` are available.
Use `-DSPIRV_VAL_EXECUTABLE=/path/to/spirv-val` for a nonstandard installation.

The fixture uses a minimal compute program and an integer descriptor environment,
not a game shader capture or GPU execution. INC/DEC translation is covered, but
SPIR-V emission remains explicitly unsupported and is excluded from the 156 modules.
