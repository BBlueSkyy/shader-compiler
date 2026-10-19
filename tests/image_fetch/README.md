# Texture fetch SPIR-V regression tests

This target uses the production SPIR-V backend through the shared standalone
build in `tests/storage_subword`. Build and run that target as documented there;
`TEST_IMAGE_FETCH` defaults to `ON`.

The fixture generates 234 fetch modules: SPIR-V 1.0/1.3/1.4, seven texture shapes
(1D, 1D array, 2D, 2D array, 3D, texel buffer, and the backend's 2D rectangle
representation), absent/immediate/computed offsets, and ordinary/sparse fetches.
Non-buffer shapes exercise both combined image/sampler descriptors and samplerless
image descriptors. Nine additional modules cover dimension queries with either
descriptor kind and LOD queries that retain their sampler.
All modules are validated with `spirv-val` for Vulkan 1.1 (with SPIR-V 1.4 where
needed), without enabling maintenance8 or its validator override.

The verifier also evaluates the emitted integer coordinate expressions to
check signed offsets and preservation of array layers. It checks that the
nonzero integer LOD survives on mipmapped images, is removed for buffers, and
that no `Offset` image operand remains. The fixture calls `EmitSPIRV` on IR;
it does not hand-author a replacement SPIR-V module.

The MK11 log contained two rejected instructions of the form
`OpImageFetch ... Lod|Offset ...`. The restriction applies to `Offset`, not
`Lod`: Vulkan without maintenance8 restricts dynamic `Offset` to gather
instructions. An SPIR-V ID referring to a constant still requires the
`ConstOffset` operand rather than `Offset`. Fetch can instead add the offset
to its integer coordinates, including computed offsets, without requiring
another Vulkan feature. Array layer coordinates are excluded from that sum.

Negative control: link this fixture against the parent compiler at
`0bf9347778dbd35423b4ce2fdc746c722252bfae`, or configure the shared standalone
target with `COMPILER_ROOT` pointing at that checkout. The original 126-module
matrix produces 42 valid no-offset modules and 84 modules rejected specifically
for `Image Operand Offset can only be used with OpImage*Gather operations`.
Older validators report VUID-StandaloneSpirv-Offset-04663; newer validators use
VUID-RuntimeSpirv-Offset-10213 and mention the maintenance8 override.

References: [SPIR-V image operands](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html#Image_Operands)
and [Vulkan SPIR-V environment](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html).
