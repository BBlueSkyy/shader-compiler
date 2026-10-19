#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run real TexturePass regressions, then validate modules for supported expressions."""
from pathlib import Path
import struct
import subprocess
import sys

executable, validator, directory = sys.argv[1:]
root = Path(directory)
root.mkdir(parents=True, exist_ok=True)
for path in root.glob('*.spv'):
    path.unlink()
subprocess.run([executable, str(root)], check=True)
modules = sorted(root.glob('*.spv'))
assert len(modules) == 57, len(modules)  # 19 supported cases, three SPIR-V versions.
for path in modules:
    version = int(path.stem.split('_')[1])
    target = 'vulkan1.1spv1.4' if version == 0x10400 else 'vulkan1.1'
    subprocess.run([validator, '--target-env', target, str(path)], check=True)
    words = struct.unpack('<' + 'I' * (path.stat().st_size // 4), path.read_bytes())
    offset = 5
    queries = 0
    while offset < len(words):
        size, opcode = words[offset] >> 16, words[offset] & 0xffff
        assert size
        if opcode == 17:  # OpCapability: no descriptor indexing feature required.
            assert words[offset + 1] < 5300 or words[offset + 1] > 5312
        queries += opcode == 103  # OpImageQuerySizeLod must survive lowering.
        offset += size
    assert queries, path
print(f'Validated {len(modules)} modules emitted after production TexturePass.')
