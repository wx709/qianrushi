# FPGA Image Processing Package

Source project:

```text
D:\FPGA20251016\tuxiang_pcie
```

Target board and route:

```text
RK3588 -> PCIe/XDMA H2C -> ShengTeng Pro XC7A35T FPGA image pipeline
       -> PCIe/XDMA C2H -> RK3588 display / analysis
```

Current frame protocol:

```text
DEPTH_POINTS = 2048
FFT_SIZE     = 2048
NUM_LINES    = 4096
VERSION_ID   = 0x20260619
FRAME_CFG    = 0x10000800
```

One frame is exactly:

```text
2048 samples per line x 4096 lines x uint16 = 16,777,216 bytes
```

The current FPGA source does not contain a 7000-line frame assumption in the
active image path. H2C input is unpacked into 2048-sample A-lines, the image
core processes one A-line at a time, and `axis_line_to_frame` groups exactly
4096 processed lines into one C2H frame.

Current core algorithm:

```text
16-bit real samples
  -> xfft_in forward FFT
  -> arithmetic right shift + signed saturation to 32-bit complex
  -> frequency-domain Hilbert filter
  -> xfft_0 inverse FFT
  -> arithmetic right shift + signed saturation to 32-bit complex
  -> norm_mag
  -> log_comp
  -> 16-bit processed pixels
```

The wide-to-32 conversion points are not raw bit slices in this package. They
use controlled shift and saturation to avoid overflow wraparound that can look
like clipping/cropping.

Directory layout in the USB package:

```text
src/image_processing/   FFT/Hilbert/IFFT and output pixel processing HDL
src/pcie_xdma/          XDMA/PCIe top, BAR registers, H2C/C2H stream logic
ip/                     Vivado .xci IP configuration files
constraints/            ShengTeng Pro board constraints
scripts/                Vivado create/build helper scripts
project/                Current Vivado .xpr for reference
verification/           Syntax-check log, if present
```

Build entry points:

```text
prj/create_project.tcl
prj/build_bitstream.tcl
```

The package intentionally excludes Vivado generated folders such as `.runs`,
`.gen`, `.cache`, `.hw`, and `.Xil`.
