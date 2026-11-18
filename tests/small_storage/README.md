# Small storage SPIR-V regression tests

The backend adapts Yuzu PR #12652, commit [`2a0d707`](https://ni.4a.si/anonymous/yuzu/commit/src?id=2a0d707ce1d4880dfcbd34c4d6572917a501f675), with the native/helper condition corrected as in [Eden #778](https://github.com/eden-emulator/Issue-Reports/issues/778) and [PR #4739](https://git.eden-emu.dev/eden-emu/eden/pulls/4739).

Native paths require integer arithmetic, the specific enabled SSBO/UBO storage feature, and descriptor aliasing. The host must include its UBO layout requirements in the uniform feature flags. Missing SSBO storage uses U32 extraction on reads and an atomic U32 compare/exchange loop on writes. Signed writes insert the same low bits as unsigned writes. Signed loads sign-extend. Offsets retain the existing byte and floor-to-halfword semantics; a misaligned U16 does not become a cross-word store.

Helpers are generated only for bindings with actual fallback writes, using the same profile predicates as their callers. Each helper references its global SSBO directly, avoiding a new `variablePointersStorageBuffer` requirement. An atomic initial load avoids a non-atomic/atomic data race between adjacent partial-word stores. Device scope and relaxed ordering preserve the existing ordinary-store ordering contract. This does not establish a guest memory barrier or make concurrent overlapping full-word/non-atomic stores defined.

Each CAS helper checks `index < OpArrayLength(ssbo)` before forming its element pointer or executing atomics. Invalid words return immediately, including null descriptors and host ranges shorter than one complete word. No retry budget is used: valid writes keep the full CAS retry semantics. The pinned Sirit has no `OpArrayLength` API, so `EmitContext::Assemble` lowers a structured entry guard into each generated CAS helper using fresh IDs from the SPIR-V module bound. Modules without these helpers are returned unchanged; Sirit remains pinned. The lowering verifies the instruction stream, helper entries and completed guard count before returning a module.

The parent host must bind U32-aligned bases and padded full-word ranges (including guest tails). Strato enforces at least four-byte SSBO alignment. An intentionally truncated 1–3-byte host tail has no complete U32 word; writes to it are discarded by the guard. Guest ranges and host descriptor bounds are distinct; no new guest OOB semantics are claimed.

## Generate and validate

Supply the parent Strato's initialized fmt, range-v3, boost and Sirit dependencies:

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tests/small_storage/run.py \
  --deps /path/to/strato/app/libraries --build-dir /tmp/small-storage \
  --baseline-ref cb0ea66d573b3b58415b51f8dac0f18d82557b01
```

Requires C++20, Python and SPIRV-Tools. `--boost-include` accepts packaged Boost headers. `--spirv-tools` accepts the SDK's `bin` directory. Default sanitizers are ASan + UBSan; `--sanitizers undefined` selects UBSan only. Build caching uses compiler commands and compiler-generated header dependency files.

The 2,048 production-backend modules cover all 128 combinations of arithmetic, SSBO, UBO and aliasing support, at SPIR-V 1.0 and 1.4. Fixtures include mixed/8-only/16-only writes, read-only small accesses, direct/indirect CBUF access, invalid addresses and multiple bindings. Each module passes `spirv-val` for Vulkan 1.1 or Vulkan 1.1 + SPIR-V 1.4, with standard UBO layout enabled. Capability/helper assertions prohibit unavailable storage capabilities and variable pointers. Dynamic, excessive and undeclared fallback bindings must throw before producing a module.

The optional baseline check substitutes only the old memory emitter into the current backend and demonstrates that the old unconditional native writer aborts on an undefined native pointer type (or produces invalid SPIR-V in assertion-disabled builds) for arithmetic-without-storage (mask 67). It is a targeted negative control, not a replay of MK11 or a full baseline build.

## Execute with SDK 1.4.363.1 validation

```sh
python3 tests/small_storage/run_vulkan.py --sdk /path/to/1.4.363.1/x86_64 \
  --modules /tmp/small-storage/modules --icd /path/to/lvp_icd.json
```

Requires the exact Khronos 1.4.363 layer from SDK 1.4.363.1, a Vulkan loader and an ICD exposing native arithmetic/storage features for A/B comparisons. The harness requires the layer version and enables Synchronization Validation. The tested official Linux SDK archive SHA-256 is `b7867d8ca658563813af666f34b93db87f1b1c7d9d69d97e924c210c7ba33eb8`.

The harness creates 96 compute pipelines and executes 120 dispatches (the original 84 plus 36 bounds cases) under Vulkan 1.1 and 1.2. Six feature profiles include no small integer features, arithmetic only, native U8 only, native U16 only, full native and disabled aliasing. Vulkan 1.1 cases enable extensions only as needed; arithmetic-only cases omit `VK_KHR_8bit_storage`. Vulkan 1.2 cases use the core features. Native and fallback paths are checked against every word of an independent expected buffer, including concurrent writes to distinct bytes/halfwords of one word, signed truncation/extension, adjacent sentinels, multiple bindings and the last valid byte.

Invalid-address modules are dispatched on profiles where both small widths use CAS, with `robustBufferAccess` and `robustBufferAccess2` disabled. Additional cases use null descriptors (with `nullDescriptor` enabled), a three-byte range, a one-word range, a truncated last word and a nonzero aligned descriptor offset. Every invocation must finish within the fence timeout, preserve every untouched word and avoid validation errors. Native OOB stores are not dispatched by this test; their behavior is outside the CAS change. These host tests do not measure FPS/frametime or establish MK11 crash resolution on Adreno.
