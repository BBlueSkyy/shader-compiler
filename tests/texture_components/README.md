# Sampled texture numeric type regression tests

The shared host suite in `tests/storage_subword` runs this target by default
with `TEST_TEXTURE_COMPONENTS=ON`.

The production backend emits 36 modules covering SPIR-V 1.0, 1.3, and 1.4,
ordinary and sparse results, buffer and texture fetches, implicit and explicit
LOD samples, gathers, and gradient samples. Every module contains Float, Uint,
and Sint descriptors together, for 108 image operations in total.

The verifier validates each module for Vulkan 1.1, using its SPIR-V 1.4 variant
where needed. It checks descriptor image scalar types, ordinary and sparse
result types, and bitcasts back to the compiler's float-typed register IR.
Numeric conversions are rejected because they would change integer register
bits. These checks do not exercise device format support or game rendering.
