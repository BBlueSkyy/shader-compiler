# Temporary SSBO inference observation

Base: `c10731c2d5ab00dbc115e651edbb88e221dcf9cc`.
This branch records why the existing Compute range inference fails and a bounded
SSA address DAG before global accesses are replaced. It does not extend inference,
change `has_size`, pointer recognition, access widths, range limits or generated IR.
The host must read `Info.compute_ssbo_diagnostics` and supply its shader hash;
this repository alone does not know the host shader/pipeline identity.

`tests/ssbo_diagnostics/run.py --fmt PATH --range PATH --boost PATH` compiles real
IR objects and the production pass, comparing observed ranges and fallbacks with
the base implementations. It also tests shared expressions, unsupported operators,
a cyclic Phi and bounded diagnostics. The base git object must be available (for
shallow clones, fetch this commit's parent). These synthetic expressions are NOT
identified Zelda shaders. Device logs are required before proposing any fix.
