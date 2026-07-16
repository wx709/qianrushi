#!/usr/bin/env python3
import glob
import mmap
import os
import struct

devs = []
for dev in glob.glob("/sys/bus/pci/devices/*"):
    try:
        with open(os.path.join(dev, "vendor"), "r", encoding="ascii") as f:
            if f.read().strip().lower() == "0x10ee":
                devs.append(dev)
    except OSError:
        pass

print("DEVICES=" + ",".join(devs))
if not devs:
    raise SystemExit(2)

resource0 = os.path.join(devs[0], "resource0")
names = [
    (0x08, "FRAME_CFG"),
    (0x0C, "VERSION"),
    (0x18, "H2C_LINES"),
    (0x20, "C2H_FRAMES"),
    (0x28, "PH_EVENTS"),
    (0x3C, "PROC_CTRL"),
]
fd = os.open(resource0, os.O_RDWR | os.O_SYNC)
try:
    mm = mmap.mmap(fd, 4096, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
    try:
        for offset, name in names:
            value = struct.unpack_from("<I", mm, offset)[0]
            print(f"{name}=0x{value:08x}")
    finally:
        mm.close()
finally:
    os.close(fd)
