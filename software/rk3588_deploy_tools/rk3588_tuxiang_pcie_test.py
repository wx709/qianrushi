#!/usr/bin/env python3
import mmap
import os
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

WIDTH = 2048
HEIGHT = 4096
BYTES_PER_PIXEL = 2
FRAME_BYTES = WIDTH * HEIGHT * BYTES_PER_PIXEL

REG_CTRL = 0x00
REG_STATUS = 0x04
REG_FRAME_CFG = 0x08
REG_VERSION = 0x0C
REG_H2C_WORDS = 0x10
REG_H2C_SAMPLES = 0x14
REG_H2C_LINES = 0x18
REG_C2H_WORDS = 0x1C
REG_C2H_FRAMES = 0x20
REG_BACKPRESSURE = 0x24
REG_PH_EVENTS = 0x28
REG_USER_CLK_HZ = 0x2C
REG_LINE_CYCLES = 0x30
REG_LINE_COUNT = 0x34
EXPECTED_VERSION = 0x20260619


def find_resource0() -> Path:
    for dev in Path("/sys/bus/pci/devices").iterdir():
        vendor = dev / "vendor"
        if vendor.exists() and vendor.read_text().strip().lower() == "0x10ee":
            res = dev / "resource0"
            if res.exists():
                return res
    raise RuntimeError("Xilinx FPGA resource0 not found")


class Bar:
    def __enter__(self):
        self.fd = os.open(str(find_resource0()), os.O_RDWR | os.O_SYNC)
        self.mm = mmap.mmap(self.fd, 0x10000, mmap.MAP_SHARED,
                            mmap.PROT_READ | mmap.PROT_WRITE)
        return self

    def __exit__(self, exc_type, exc, tb):
        self.mm.close()
        os.close(self.fd)

    def read32(self, off: int) -> int:
        return struct.unpack_from("<I", self.mm, off)[0]

    def write32(self, off: int, val: int) -> None:
        struct.pack_into("<I", self.mm, off, val & 0xFFFFFFFF)
        self.mm.flush(off & ~0xFFF, 0x1000)


def make_test_frame() -> bytearray:
    data = bytearray(FRAME_BYTES)
    p = 0
    for y in range(HEIGHT):
        base = (y * 17) & 0xFFFF
        for x in range(WIDTH):
            v = (base + (x * 3) + ((x ^ y) & 0x3F) * 19) & 0xFFFF
            data[p] = v & 0xFF
            data[p + 1] = (v >> 8) & 0xFF
            p += 2
    return data


def write_all(path: str, data: bytes) -> None:
    with open(path, "wb", buffering=0) as f:
        mv = memoryview(data)
        done = 0
        chunk = 1 << 20
        while done < len(mv):
            done += f.write(mv[done:done + chunk])


def read_all(path: str, size: int, out: bytearray, errors: list) -> None:
    try:
        with open(path, "rb", buffering=0) as f:
            done = 0
            chunk = 1 << 20
            while done < size:
                block = f.read(min(chunk, size - done))
                if not block:
                    raise RuntimeError(f"{path} returned EOF at {done}/{size}")
                out[done:done + len(block)] = block
                done += len(block)
    except BaseException as exc:
        errors.append(exc)


def main() -> int:
    subprocess.run(["sudo", "-n", "/usr/local/sbin/ensure_fpga_pcie_ready.sh"],
                   check=True)
    for node in ("/dev/xdma0_h2c_0", "/dev/xdma0_c2h_0", "/dev/xdma0_control"):
        if not Path(node).exists():
            raise RuntimeError(f"missing {node}")

    with Bar() as bar:
        version = bar.read32(REG_VERSION)
        frame_cfg = bar.read32(REG_FRAME_CFG)
        print(f"version=0x{version:08x}")
        print(f"frame_cfg=0x{frame_cfg:08x} depth={frame_cfg & 0xffff} lines={(frame_cfg >> 16) & 0xffff}")
        if version != EXPECTED_VERSION:
            raise RuntimeError(f"version mismatch: 0x{version:08x}")
        if frame_cfg != ((HEIGHT << 16) | WIDTH):
            raise RuntimeError(f"frame_cfg mismatch: 0x{frame_cfg:08x}")
        bar.write32(REG_CTRL, 0x00)
        time.sleep(0.001)
        bar.write32(REG_CTRL, 0x02)
        time.sleep(0.001)
        bar.write32(REG_CTRL, 0x10)
        time.sleep(0.001)
        bar.write32(REG_CTRL, 0x01)
        time.sleep(0.01)

    src = make_test_frame()
    dst = bytearray(FRAME_BYTES)
    errors = []
    reader = threading.Thread(target=read_all, args=("/dev/xdma0_c2h_0", FRAME_BYTES, dst, errors))
    t0 = time.perf_counter()
    reader.start()
    write_all("/dev/xdma0_h2c_0", src)
    reader.join()
    elapsed_ms = (time.perf_counter() - t0) * 1000.0
    if errors:
        raise errors[0]

    nonzero = any(dst)
    same_as_input = (dst == src)
    with Bar() as bar:
        regs = {
            "status": bar.read32(REG_STATUS),
            "h2c_words": bar.read32(REG_H2C_WORDS),
            "h2c_samples": bar.read32(REG_H2C_SAMPLES),
            "h2c_lines": bar.read32(REG_H2C_LINES),
            "c2h_words": bar.read32(REG_C2H_WORDS),
            "c2h_frames": bar.read32(REG_C2H_FRAMES),
            "backpressure": bar.read32(REG_BACKPRESSURE),
            "ph_events": bar.read32(REG_PH_EVENTS),
            "user_clk_hz": bar.read32(REG_USER_CLK_HZ),
            "line_cycles": bar.read32(REG_LINE_CYCLES),
            "line_count": bar.read32(REG_LINE_COUNT),
        }
    print(f"elapsed_ms={elapsed_ms:.3f}")
    print(f"output_nonzero={nonzero} same_as_input={same_as_input}")
    for k, v in regs.items():
        print(f"{k}=0x{v:08x} ({v})")
    line_ms = (regs["line_cycles"] * 1000.0 / regs["user_clk_hz"]) if regs["user_clk_hz"] else -1
    print(f"fpga_line_core_ms={line_ms:.6f}")

    out_dir = Path("/home/elf/fpga_dianji_tuxiang_test/tuxiang_pcie")
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "tuxiang_input_2048x4096_u16.bin").write_bytes(src[:1 << 20])
    (out_dir / "tuxiang_output_2048x4096_u16_head.bin").write_bytes(dst[:1 << 20])
    if not nonzero:
        raise RuntimeError("processed output is all zero")
    if regs["c2h_frames"] < 1:
        raise RuntimeError("C2H frame counter did not increment")
    if regs["h2c_lines"] < HEIGHT:
        raise RuntimeError(f"H2C line counter too small: {regs['h2c_lines']}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BaseException as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise
