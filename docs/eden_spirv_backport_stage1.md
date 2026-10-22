# Eden SPIR-V correctness backport — stage 1

This small stage is based on the existing SHF fix branch
`fix/shf-low-source-register`, already pinned by Strato PR #380.
It does not merge Eden's shader compiler or replace Strato's architecture.

1. **Dynamic texture-rescaling bit masks**: backport Eden commit
   `028050cf044d`. Read the correct push-constant word at dynamic
   `(index + base) >> 5` and test the corresponding bit.
2. **Signed texture gather PTP offsets**: backport the signed
   `ConstOffsets` portion of Eden commit `025bc799f782`. The
   array element type is now signed 2x32 and negative offsets retain
   their intended two's-complement interpretation.
3. **Dynamic constant-buffer indexing**: align both unsigned byte-offset
   shifts with the current Eden implementation. Leave the Strato-specific
   `element_offset` and CBUF layout unchanged.

Host smoke tests: `python3 tests/eden_spirv_backport_semantics.py`.
These verify source emission contracts and independent arithmetic examples,
**not** shader execution or Vulkan driver behavior.

Limits: no APK, graphics workload, or full native CMake build is verified.
Visual improvements to SpongeBob or other games are not established. No
clamps, cache changes, pipeline changes, or global rewrite are introduced.

Licensing: the adapted Eden 2026 code carries the GPL-3.0-or-later
attribution alongside the existing GPL-2.0-or-later source notices.
