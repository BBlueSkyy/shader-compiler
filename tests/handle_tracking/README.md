# Texture handle tracking regression tests

The shared host suite in `tests/storage_subword` runs this executable by default
with `TEST_HANDLE_TRACKING=ON`. It compiles the production `TexturePass` and runs
it on real bindless query IR under undefined-behavior sanitization.

Thirteen handle expressions cover masks before and after shifts, nested masks
and shifts, split texture/sampler handles, transformations around an OR, a zero
mask, shifts of 31 and 32 bits, and immediate or constant-buffer mask sources.
Each case compares the environment's translation-time handle with the emitted
descriptor evaluated by the runtime helper. A further case verifies that four
different transformations of the same source remain separate descriptors.

These are host compiler tests. They do not execute guest shaders or establish
game rendering correctness on a Vulkan device.

The suite additionally checks semantic provenance across memory-value boundaries,
selected CBUF-pair lanes, unsupported expressions and descriptor preservation.
The shared suite validates 57 supported-expression SPIR-V modules with
`spirv-val`. See [PROVENANCE.md](PROVENANCE.md) for the captured failure,
reference comparison and explicit lack of dynamic SSBO-handle support.
