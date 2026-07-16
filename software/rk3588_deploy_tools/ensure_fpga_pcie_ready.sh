#!/bin/sh
set -u

LOG=/tmp/fpga_pcie_ready.log
exec >> "$LOG" 2>&1

echo "===== $(date '+%F %T') ensure FPGA PCIe ready ====="

EXPECTED_VERSION=0x20260619
XDMA_KO=/home/elf/fpga_dianji_tuxiang_test/dma_ip_drivers/XDMA/linux-kernel/xdma/xdma.ko

has_xilinx() {
  for d in /sys/bus/pci/devices/*; do
    [ -f "$d/vendor" ] || continue
    [ "$(cat "$d/vendor" 2>/dev/null)" = "0x10ee" ] && return 0
  done
  return 1
}

find_xilinx_dev() {
  for d in /sys/bus/pci/devices/*; do
    [ -f "$d/vendor" ] || continue
    if [ "$(cat "$d/vendor" 2>/dev/null)" = "0x10ee" ]; then
      basename "$d"
      return 0
    fi
  done
  return 1
}

xdma_nodes_ready() {
  [ -e /dev/xdma0_h2c_0 ] && [ -e /dev/xdma0_c2h_0 ] && [ -e /dev/xdma0_control ]
}

load_xdma() {
  if [ ! -f "$XDMA_KO" ]; then
    echo "WARN: xdma.ko not found: $XDMA_KO"
    return 1
  fi
  if lsmod | awk '{print $1}' | grep -qx xdma; then
    if xdma_nodes_ready; then
      echo "xdma module and nodes already ready"
      return 0
    fi
    echo "xdma module loaded but nodes missing; reloading xdma"
    rmmod xdma 2>/dev/null || true
    sleep 1
  fi
  insmod "$XDMA_KO" interrupt_mode=0 config_bar_num=1 || true
  sleep 1
}

set_permissions() {
  DEV="$1"
  SYSDEV=/sys/bus/pci/devices/"$DEV"
  chgrp plugdev /dev/xdma* 2>/dev/null || true
  chmod 0660 /dev/xdma* 2>/dev/null || true
  chgrp plugdev "$SYSDEV/resource0" "$SYSDEV/resource1" 2>/dev/null || true
  chmod 0660 "$SYSDEV/resource0" "$SYSDEV/resource1" 2>/dev/null || true
}

probe_platform_and_rescan() {
  if [ -e /sys/bus/platform/devices/fe170000.pcie ]; then
    echo fe170000.pcie > /sys/bus/platform/drivers_probe 2>/dev/null || true
  fi
  echo 1 > /sys/bus/pci/rescan 2>/dev/null || true
}

wait_for_xilinx() {
  i=0
  while [ "$i" -lt 20 ]; do
    if has_xilinx; then
      return 0
    fi
    echo "probe attempt $i"
    probe_platform_and_rescan
    sleep 1
    i=$((i + 1))
  done
  return 1
}

bar_ok() {
  DEV="$1"
  SYSDEV=/sys/bus/pci/devices/"$DEV"
  python3 - "$SYSDEV" "$EXPECTED_VERSION" <<'PY'
import mmap
import os
import pathlib
import struct
import sys

sysdev = pathlib.Path(sys.argv[1])
expected = int(sys.argv[2], 16)
res = sysdev / "resource0"
try:
    fd = os.open(str(res), os.O_RDWR | os.O_SYNC)
    try:
        mm = mmap.mmap(fd, 0x10000, mmap.MAP_SHARED,
                       mmap.PROT_READ | mmap.PROT_WRITE)
        try:
            version = struct.unpack_from("<I", mm, 0x0c)[0]
            status = struct.unpack_from("<I", mm, 0x104)[0]
            count = struct.unpack_from("<I", mm, 0x108)[0]
            print(f"BAR_PROBE version=0x{version:08x} motor_status=0x{status:08x} motor_count={count}")
            if version == expected:
                sys.exit(0)
            sys.exit(1)
        finally:
            mm.close()
    finally:
        os.close(fd)
except Exception as exc:
    print(f"BAR_WARN {type(exc).__name__}: {exc}")
    sys.exit(2)
PY
}

recover_pcie() {
  DEV="$(find_xilinx_dev 2>/dev/null || true)"
  if lsmod | awk '{print $1}' | grep -qx xdma; then
    echo "reloading xdma before PCIe recovery"
    rmmod xdma 2>/dev/null || true
    sleep 1
  fi
  if [ -n "$DEV" ] && [ -e /sys/bus/pci/devices/"$DEV"/remove ]; then
    echo "removing stale PCIe endpoint $DEV"
    echo 1 > /sys/bus/pci/devices/"$DEV"/remove 2>/dev/null || true
    sleep 1
  fi
  probe_platform_and_rescan
  sleep 3
}

attempt=0
while [ "$attempt" -lt 3 ]; do
  if ! wait_for_xilinx; then
    echo "ERROR: Xilinx PCIe endpoint not enumerated"
    exit 2
  fi

  DEV="$(find_xilinx_dev)"
  SYSDEV=/sys/bus/pci/devices/"$DEV"
  echo "Xilinx endpoint: $DEV vendor=$(cat "$SYSDEV/vendor") device=$(cat "$SYSDEV/device")"

  [ -w "$SYSDEV/enable" ] && echo 1 > "$SYSDEV/enable" 2>/dev/null || true
  command -v setpci >/dev/null 2>&1 && setpci -s "$DEV" COMMAND=0006 2>/dev/null || true

  load_xdma || true
  set_permissions "$DEV"

  if xdma_nodes_ready && bar_ok "$DEV"; then
    ls -l /dev/xdma0_h2c_0 /dev/xdma0_c2h_0 /dev/xdma0_control 2>/dev/null || true
    ls -l "$SYSDEV/resource0" "$SYSDEV/resource1" 2>/dev/null || true
    echo READY
    exit 0
  fi

  echo "WARN: FPGA PCIe endpoint exists but BAR/register probe is not valid"
  attempt=$((attempt + 1))
  recover_pcie
done

echo "ERROR: Xilinx endpoint present but XDMA/BAR did not become ready"
exit 4
