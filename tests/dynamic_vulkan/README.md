# Optional Vulkan execution regression

`dynamic_vulkan_runner` runs `EmitSPIRV` output from the production compiler's
`tests/dynamic_textures/generate.cpp`. It never replaces that output with GLSL,
handwritten SPIR-V, or a CPU implementation of the texture operation. The CPU
only initializes resources and checks readback against known values.

This test requires a Vulkan 1.2-capable loader, development headers, C++20 compiler, and a
Vulkan ICD supporting sampled image dynamic/nonuniform indexing,
`runtimeDescriptorArray`, `descriptorBindingPartiallyBound`, and
`shaderImageGatherExtended` (declared by the production backend). A software ICD
such as Mesa lavapipe is sufficient for host regression testing. A host pass
does not establish correctness or performance on an Android GPU. No driver
binaries or package archives belong in this repository.

Generate and validate modules with the shared standalone host build documented
in `tests/storage_subword` (`TEST_DYNAMIC_TEXTURES` defaults to `ON`):

```sh
cmake --build build-host --target dynamic_texture_tests
build-host/dynamic_texture_tests build-host/dynamic-modules
python3 tests/dynamic_textures/verify.py \
  build-host/dynamic_texture_tests /path/to/spirv-val build-host/dynamic-modules
```

Build and execute this independent, optional runner from the compiler root:

```sh
cmake -S tests/dynamic_vulkan -B build-vulkan \
  -DDYNAMIC_SAMPLE_SPV="$PWD/build-host/dynamic-modules/sample.spv"
cmake --build build-vulkan
ctest --test-dir build-vulkan --output-on-failure
build-vulkan/dynamic_vulkan_runner build-host/dynamic-modules/fetch.spv fetch
build-vulkan/dynamic_vulkan_runner build-host/dynamic-modules/query.spv query
build-vulkan/dynamic_vulkan_runner build-host/dynamic-modules/transform.spv
```

To execute all fifteen sample/transform/phi/fetch/query modules across SPIR-V
1.0, 1.3, and 1.4, configure with
`-DDYNAMIC_MODULE_DIR="$PWD/build-host/dynamic-modules"` instead of
`DYNAMIC_SAMPLE_SPV`, then build and run CTest.

Set `VK_ICD_FILENAMES=/absolute/path/to/your/icd.json` to select a particular ICD.
For validation-layer execution, set `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`
and, if needed, `VK_LAYER_PATH` to the directory containing its manifest. The
runner installs a debug messenger and fails on any validation ERROR, including
errors during resource destruction. A requested but unavailable layer fails
instance creation; it does not silently run without validation.
Missing Vulkan devices or required features produce exit status **77** (CTest
skip). A sample mismatch, unexpected fault, or Vulkan call failure fails the
regression. The program accepts `sample` (default), `fetch`, or `query` as its
second argument to select expected results for the generated module.

The sample test dispatches one workgroup of 64 invocations. Every lane loads its
packed TIC/TSC handle from set 0's input SSBO. Six TICs select two compact slots
in each of three separate Float, Uint, and Sint image arrays. Every image has
RGBA32 values and two mip levels; slots have different dimensions so queries
also distinguish descriptors. Distinct sampler minimum LODs make per-lane
sampler selection observable. Integer values retain their raw register bits.
Only the first two of each image array's four descriptors are written.

Checks include:

- Three image component classes and different image/sampler indices in one
  workgroup, with all four result channels checked.
- A second dispatch after changing only TIC-to-compact-slot metadata, using the
  same pipeline and descriptors.
- Via-header sampler selection from TIC, with deliberately invalid high TSC
  bits proving those bits are ignored in this mode.
- TIC bounds, TSC bounds, invalid sampler, invalid TIC map, and shape-group
  guards: each must return zero and capture the expected first-fault code and
  raw handle.
- Linear sampler rejection for integer images and float entries without the
  filterability flag, plus a valid filterable Float+linear case when supported.
  Mixed invalid lanes must capture a consistent first-fault code/handle pair.
- Fetch and query execution with invalid TSC bits, zero sampler count, and
  invalid sampler flags, proving samplerless operations ignore sampler state.

The metadata layout is the production compiler/runtime ABI: words 0..5 contain
TIC count, TSC count, via-header flag, fault code, fault handle, and group count;
words 6..7 are reserved; words starting at 8 hold each group's TIC-map offset;
then come sampler-validity words, followed by TIC maps. A map entry packs
`compact_index << 2 | component` (Float=0, Uint=1, Sint=2); bit 31 records Float
format linear-filter support and must be masked before decoding the compact
index. `0xffffffff` is invalid. Sampler flags use bit 0 for validity and bit 1
for linear filtering. Unsupported image/linear-sampler combinations fault with
code 6 before a descriptor access.
The module's workgroup size and descriptor interface must match this fixture.
