# Hardware

## Current FPGA Image Processing Project

Use:

```text
hardware/fpga_tuxiang_pcie_current/
```

Current verified bitstream:

```text
hardware/bitstreams/current/xilinx_dma_pcie_ep_20260706_current.bit
```

Current Flash programming package:

```text
hardware/flash/tuxiang_pcie_flash_20260706_package/
```

The active FPGA pipeline is the ShengTeng Pro PCIe/XDMA image-processing design.
It receives LC-OCT image data from RK3588 through PCIe/XDMA, processes it in PL,
and returns processed frames to RK3588 for display and YOLO/RKNN analysis.
