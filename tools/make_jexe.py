#!/usr/bin/env python3
import argparse
from pathlib import Path
import struct

parser = argparse.ArgumentParser(description='Wrap a flat i386 binary in JEXE v1.')
parser.add_argument('input', type=Path)
parser.add_argument('output', type=Path)
parser.add_argument('--entry', type=lambda x: int(x, 0), default=0)
args = parser.parse_args()

code = args.input.read_bytes()
if not code:
    raise SystemExit('input program is empty')
if len(code) > 4096 - 16:
    raise SystemExit('program is too large for the current one-page JEXE loader')
if args.entry >= len(code):
    raise SystemExit('entry point is outside the program')

header = struct.pack('<4sIII', b'JEXE', 1, args.entry, len(code))
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_bytes(header + code)
print(f'wrote {args.output} ({len(header) + len(code)} bytes)')
