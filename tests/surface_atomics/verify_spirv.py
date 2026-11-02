#!/usr/bin/env python3
"""Validate real translated modules and check emitted operation/scope/semantics."""
import argparse
from pathlib import Path
import struct
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--directory', type=Path, required=True)
parser.add_argument('--validator', default='spirv-val')
args = parser.parse_args()
files = sorted(args.directory.glob('*.spv'))
assert len(files) == 156, f'expected 156 translated modules, got {len(files)}'
# SPIR-V core opcode numbers, not compiler implementation enums.
atomic_opcodes = {0: 234, 1: 236, 2: 238, 5: 240, 6: 241, 7: 242, 8: 229}
for file in files:
    words = struct.unpack('<' + 'I' * (file.stat().st_size // 4), file.read_bytes())
    instructions = []
    offset = 5
    while offset < len(words):
        count, opcode = words[offset] >> 16, words[offset] & 0xffff
        assert count and offset + count <= len(words), file
        instructions.append((opcode, words[offset + 1:offset + count]))
        offset += count
    requested = int(file.stem.rsplit('op', 1)[1])
    atomics = [(op, operands) for op, operands in instructions if 227 <= op <= 242]
    assert len(atomics) == 1 and atomics[0][0] == atomic_opcodes[requested], file
    constants = {operands[1]: operands[2] for op, operands in instructions if op == 43 and len(operands) == 3}
    operands = atomics[0][1]
    assert constants[operands[3]] == 1, f'{file}: atomic scope must be Device'
    assert constants[operands[4]] == 0, f'{file}: atomic semantics must be Relaxed'
    image_types = [operands for op, operands in instructions if op == 25]
    assert len(image_types) == 1 and image_types[0][-1] == 33, f'{file}: expected R32ui'
    subprocess.run([args.validator, '--target-env', 'vulkan1.1spv1.4', str(file)], check=True)
print(f'PASS {len(files)} SPIR-V modules: operations, Device/Relaxed atomics, R32ui, spirv-val')
