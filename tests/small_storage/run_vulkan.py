#!/usr/bin/env python3
"""Execute generated modules with the exact SDK 1.4.363.1 validation layer."""
import argparse
import os
from pathlib import Path
import subprocess
p = argparse.ArgumentParser()
p.add_argument('--sdk', type=Path, required=True, help='SDK x86_64 directory with include, lib and share')
p.add_argument('--modules', type=Path, required=True)
p.add_argument('--icd', type=Path, help='software ICD manifest for deterministic host tests')
a = p.parse_args()
sdk = a.sdk.resolve()
modules = a.modules.resolve()
exe = modules.parent / 'small-storage-vulkan'
subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++20', '-O1', '-fno-strict-aliasing',
    '-I'+str(sdk/'include'), str(Path(__file__).with_name('vulkan.cpp')), '-l:libvulkan.so.1', '-o', str(exe)], check=True)
env = os.environ.copy()
env['VK_LAYER_PATH'] = str(sdk/'share/vulkan/explicit_layer.d')
if a.icd: env['VK_ICD_FILENAMES'] = str(a.icd.resolve())
subprocess.run([exe, modules], env=env, check=True, timeout=180)
