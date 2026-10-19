# GPU dynamic texture handle kernels

`dynamic_texture_tests OUTPUT_DIR` uses the production SPIR-V backend to emit
64-lane compute kernels. `sample.spv`, `fetch.spv`, `query.spv`,
`transform.spv`, `offset.spv`, and `phi.spv` use SPIR-V 1.3; suffixed modules exercise 1.0
and 1.4. `transform` reconstructs the full SSBO-loaded handle using masks,
shifts, and OR; `offset` exercises a nonconstant gather offset passed to the
helper. `phi` samples a handle selected by a Phi in a divergent branch,
checking that generated helpers preserve IR predecessor labels. The optional
`../dynamic_vulkan` runner executes all sample, transform, phi, fetch, and query
variants. The offset/gather variants receive structural SPIR-V validation only.

Each lane reads `set=0,binding=0` u32 input[local_invocation_id.x] and writes
four register words to `set=0,binding=1` u32 output[4*lane..4*lane+3]. Sample
coordinates are (0.5,0.5), LOD zero. Integer texture channels preserve register
bits, rather than numerically converting to float. Query writes unsigned
(width,height,0,mip_count); fetch reads texel (0,0), LOD zero.

## Descriptor set 1 ABI

| Binding | Resource |
| --- | --- |
| 0 | Separate sampler runtime array, indexed by TSC |
| 1 | Read/write storage buffer of u32 metadata words |
| 2 + 3*group + 0 | Float sampled-image runtime array |
| 2 + 3*group + 1 | Uint sampled-image runtime array |
| 2 + 3*group + 2 | Sint sampled-image runtime array |

Metadata words:

| Word | Value |
| --- | --- |
| 0 | TIC count |
| 1 | Effective TSC count |
| 2 | viaHeader (0 or 1) |
| 3 | First fault code (initialized to zero per descriptor-table snapshot) |
| 4 | First fault's complete raw handle |
| 5 | Group count |
| 6,7 | Reserved |
| 8 + group | Absolute word offset of that group's TIC map |
| 8 + group_count + TSC | Sampler flags (bit 0 valid, bit 1 uses linear filtering) |
| map_offset + TIC | (compact_image_index << 2) or component tag (Float=0, Uint=1, Sint=2), plus bit 31 if the image supports linear filtering; 0xffffffff invalid |

TIC is `handle & 0xfffff`. TSC is `viaHeader ? TIC : handle >> 20`.
Bounds, sampler validity, and image-map validity checks precede all descriptor
loads. Samplerless fetch/query skip TSC validation and sampler access. A fault
returns zero and atomically claims metadata word 3; only the winner writes
word 4. Host diagnostics consume both words after GPU completion. Faults
persist for the immutable table snapshot, including across dispatches.

Fault codes are 1 TIC bounds, 2 TSC bounds, 3 invalid sampler, 4 invalid TIC
map tag, 5 missing group, and 6 incompatible linear-filter sampler/image pair.
Compact image indices use bits 2 through 30; bit 31 is excluded before indexing. There is no fallback image or sampler access.
The runtime owns trusted metadata offsets and compact descriptor bounds.

The initial runtime/compiler contract supports non-depth 2D textures in compute
only. Dynamic TXQ has no instruction shape, so uses the 2D group under this
contract; the runtime explicitly rejects TICs with other shapes. Sparse accesses,
texture buffers, depth sampling, image writes and atomics are unsupported.
Both Environment::SupportsDynamicTextureHandles and
Profile::support_dynamic_texture_handles default to false. The static CBUF
path remains the first choice for fully representable handles.

`verify.py` runs spirv-val on all kernels and checks runtime array bindings,
nonuniform decoration, typed control-flow dispatch, samplerless behavior, and
atomic fault guards. `tests/handle_tracking` also checks opt-in preservation of
GPU-loaded handles and Phi values with zero host descriptor reads.
