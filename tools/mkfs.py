#!/usr/bin/env python3
from pathlib import Path
import struct

SECTOR=512
FS_START=65
DIR_LBA=66
DATA_LBA=67
TOTAL=64
MAX_ENTRIES=16

root=Path(__file__).resolve().parent.parent
out=Path(__file__).resolve().parent.parent/'disk'/'fs.img'
out.parent.mkdir(parents=True, exist_ok=True)

files={
    'README.TXT': b'JupiterOS v0.12\n\nJFS1 contains text files and JEXE userspace programs.\n',
    'HELLO.TXT': b'Hello from the JupiterOS filesystem!\n',
    'SYSTEM.TXT': b'JFS1 is a tiny writable filesystem used by JupiterOS v0.10-v0.12.\n',
    'HELLO.JXE': (root / 'disk' / 'HELLO.JXE').read_bytes(),
    'LOOP.JXE': (root / 'disk' / 'LOOP.JXE').read_bytes(),
}

image=bytearray(TOTAL*SECTOR)
# Superblock
sb=struct.pack('<4s6I', b'JFS1', 1, SECTOR, DIR_LBA, MAX_ENTRIES, DATA_LBA, TOTAL)
image[:SECTOR]=sb.ljust(SECTOR,b'\0')

lba=DATA_LBA
for idx,(name,data) in enumerate(files.items()):
    if idx>=MAX_ENTRIES:
        raise SystemExit('too many files')
    sectors=(len(data)+SECTOR-1)//SECTOR
    if lba+sectors>FS_START+TOTAL:
        raise SystemExit('filesystem image is full')
    off=SECTOR + idx*32
    name_b=name.encode('ascii')
    if len(name_b)>15:
        raise SystemExit('filename too long')
    entry=struct.pack('<16sIII', name_b+b'\0'*(16-len(name_b)), lba, len(data), sectors)
    image[off:off+32]=entry.ljust(32,b'\0')
    start=(lba-FS_START)*SECTOR
    image[start:start+len(data)]=data
    lba+=sectors

out.write_bytes(image)
print(f'wrote {out} ({len(image)} bytes)')
