#!/usr/bin/env python3
"""Rebuild the probe binaries from native/probe.c into bin/<abi>/probe.

Mirrors the build approach used by the onn-4k-pro-2026-unlock-root project:
same NDK version, same compiler flags, reproducible output.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

TARGETS = {
    'armeabi-v7a': 'armv7a-linux-androideabi28-clang',
    'arm64-v8a': 'aarch64-linux-android28-clang',
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ndk', required=True, type=Path, help='Android NDK r28c directory')
    args = parser.parse_args()
    toolchain = args.ndk / 'toolchains/llvm/prebuilt/linux-x86_64/bin'
    for abi, compiler_name in TARGETS.items():
        compiler = toolchain / compiler_name
        out_dir = ROOT / 'bin' / abi
        out_dir.mkdir(parents=True, exist_ok=True)
        output = out_dir / 'probe'
        command = [str(compiler), '-std=c11', '-D_GNU_SOURCE', '-O2', '-Wall', '-Wextra',
                   '-Werror', '-fPIE', '-pie', str(ROOT / 'native' / 'probe.c'),
                   '-o', str(output)]
        subprocess.run(command, check=True)
        sha = hashlib.sha256(output.read_bytes()).hexdigest()
        print(f'{abi}: {output} sha256={sha}')


if __name__ == '__main__':
    main()
