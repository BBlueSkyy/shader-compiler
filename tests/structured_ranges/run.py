#!/usr/bin/env python3
"""Execute the actual global-memory pass with synthetic SSA/ASL safety fixtures."""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--deps', type=Path, required=True)
args = parser.parse_args()
deps = args.deps.resolve()
includes = [root, root / 'include', deps / 'fmt/include', deps / 'range/include']
includes += list((deps / 'boost/libs').glob('*/include'))
with tempfile.TemporaryDirectory(prefix='structured-ssbo-') as directory:
    executable = Path(directory) / 'test'
    command = [
        os.environ.get('CXX', 'c++'), '-std=c++20', '-O1', '-w', '-include', 'cstddef',
        '-fno-strict-aliasing', '-DFMT_HEADER_ONLY', '-ffunction-sections', '-fdata-sections',
        '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined'),
        '-fno-sanitize-recover=all', *[f'-I{path}' for path in includes],
        str(Path(__file__).with_name('test.cpp')),
        *map(str, (root / 'frontend/ir').glob('*.cpp')),
        '-Wl,--gc-sections', '-o', str(executable),
    ]
    subprocess.run(command, check=True)
    subprocess.run([executable], check=True, timeout=30)
