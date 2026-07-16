# RK3588 OCT + ShengTeng Pro PCIe Project Handover

Updated: 2026-07-06

This is the consolidated handover for the current embedded/OCT project. Use this
file as the first document on a new machine. Older handover notes and the old
`fpga_dianji_tuxiang` mixed motor/image project are historical references only;
the current active FPGA image project is `D:\FPGA20251016\tuxiang_pcie`.

## 1. Current Main Route

Current active route used for lab testing:

```text
YOLO/OCT-like dataset or OCT camera on RK3588
  -> RK3588 Qt capture panel
  -> XDMA H2C PCIe transfer
  -> ShengTeng Pro / XC7A35T image pipeline in FPGA
  -> XDMA C2H PCIe transfer
  -> RK3588 real-time raw + FPGA-processed display
  -> RK3588 YOLO/RKNN analysis video
```

Important current decision:

- ShengTeng Pro now only does PCIe image processing.
- Motor control is not part of the active ShengTeng Pro project anymore.
- The old folder `D:\FPGA20251016\qianrushi\fpga_dianji_tuxiang` was a mixed
  motor/image historical project and should not be used as the active source.
- The active ShengTeng Pro image source is:

```text
D:\FPGA20251016\tuxiang_pcie
```

## 2. Current Hardware And Login

RK3588:

```text
IP last used: 192.168.110.119
user/pass: elf/elf
Qt panel source: /home/elf/rk3568_capture/qt_panel
Qt panel binary: /home/elf/rk3568_capture/qt_panel/build/oct_qt_panel
Qt launcher: /home/elf/rk3568_capture/run_qt_panel.sh
YOLO/RKNN path: /home/elf/Desktop/yolo/03_rk3588_model
Small realtime dataset: /home/elf/Desktop/yolo/01_panel_stream_raw
```

Windows helper tools:

```text
plink/pscp: D:\APP\putty\plink.exe, D:\APP\putty\pscp.exe
Vivado used previously: D:\FPGA20251016\2025.2\Vivado\bin\vivado.bat
Code encoding skill: D:\Tools\skills\codex-local\code-encoding-hygiene\SKILL.md
```

SSH host key previously used:

```text
ssh-ed25519 255 f5:45:85:80:a7:ff:b1:43:ff:71:0c:dc:37:58:8a:47
```

## 3. Active FPGA Project: tuxiang_pcie

Location:

```text
D:\FPGA20251016\tuxiang_pcie
```

Important files:

```text
pcie/xilinx_dma_pcie_ep.sv          top-level XDMA endpoint wrapper
pcie/xdma_app.v                     XDMA app wrapper
pcie/ascent_pcie_ph_app.v           BAR register + H2C/C2H image datapath
pcie/ascent_h2c64_to_u16_axis.v     unpack 64-bit H2C words into 16-bit samples
pcie/axis_line_to_frame.v           converts per-line tlast to per-frame tlast
ph_pcie_ip/ph_ip.v                  streaming OCT processing wrapper
ph_pcie_ip/ph_core_pipeline.v       FFT/Hilbert/IFFT/magnitude/log path
ph_pcie_ip/axis_gray8_packer.v      packs processed pixels for C2H
ph_pcie_ip/xfft_in/xfft_in.xci      FFT IP with reset support
ph_pcie_ip/xfft_0/xfft_0.xci        IFFT/FFT IP with reset support
constraints/ascent_pro_xdma_pcie.xdc board constraints
prj/create_project.tcl              create Vivado project
prj/build_bitstream.tcl             build bitstream
```

Latest bitstream observed:

```text
D:\FPGA20251016\tuxiang_pcie\prj\tuxiang_pcie.runs\impl_1\xilinx_dma_pcie_ep.bit
size: 1,588,332 bytes
mtime: 2026-07-06 17:23:36
sha256: A711A3040D89CD1AF375CB8E140C7FBFCDD5DD39BC8CD5026339767C78354FED
```

Vivado routed status from the last documented build:

```text
WNS = 0.196 ns
TNS = 0.000 ns
WHS = 0.036 ns
THS = 0.000 ns
All user specified timing constraints met.
DRC: 0 errors.
```

Current FPGA image protocol:

```text
DEPTH_POINTS = 2048
FFT_SIZE     = 2048
NUM_LINES    = 4096
VERSION_ID   = 0x20260619
USER_CLK_HZ  = 125000000
```

One frame is:

```text
2048 samples/line x 4096 lines x uint16 = 16,777,216 bytes
```

Input convention:

- RK3588 sends one line as XDMA H2C AXIS data.
- H2C width is 64-bit little-endian.
- Each H2C word contains four 16-bit samples:
  `tdata[15:0]`, `tdata[31:16]`, `tdata[47:32]`, `tdata[63:48]`.
- H2C `tlast` marks end of one input line.

FPGA processing:

```text
64-bit H2C stream
  -> 16-bit line samples
  -> ph_ip FFT/Hilbert/IFFT/magnitude/log pipeline
  -> 64-bit processed line stream
  -> axis_line_to_frame counts 4096 lines
  -> C2H tlast marks one complete 4096-line frame
```

`axis_line_to_frame.v` is important. The old 7000-line/line-level handling caused
image stitching confusion. The active design groups exactly 4096 processed lines
into one C2H frame.

## 4. FPGA BAR Register Map

From `ascent_pcie_ph_app.v`:

```text
0x00 CTRL
     bit0 stream_enable
     bit1 soft_reset pulse
     bit2 bypass_enable
     bit4 clear_counters pulse

0x04 STATUS
0x08 FRAME_CFG     {NUM_LINES[15:0], DEPTH_POINTS[15:0]}
                   expected 0x10000800 for 4096 x 2048
0x0C VERSION       expected 0x20260619
0x10 H2C_WORDS
0x14 H2C_SAMPLES
0x18 H2C_LINES
0x1C C2H_WORDS
0x20 C2H_FRAMES
0x24 BACKPRESSURE  {h2c_backpressure[15:0], c2h_backpressure[15:0]}
0x28 PH_EVENTS     FFT/pipeline event flags; expected 0 in normal operation
0x2C USER_CLK_HZ   expected 125000000
0x30 LINE_CYCLES   measured FPGA core line latency cycles
0x34 LINE_COUNT    line latency sample count
0x38 DEBUG_FLAGS
```

Useful expected test state after one valid 4096-line frame:

```text
FRAME_CFG  = 0x10000800
VERSION    = 0x20260619
H2C_LINES  = 4096
C2H_FRAMES = 1
PH_EVENTS  = 0
```

## 5. RK3588 Qt Capture Panel Current Behavior

Active source on RK3588:

```text
/home/elf/rk3568_capture/qt_panel/mainwindow.cpp
/home/elf/rk3568_capture/qt_panel/mainwindow.h
/home/elf/rk3568_capture/qt_panel/capture_worker.cpp
/home/elf/rk3568_capture/qt_panel/build/oct_qt_panel
```

Current mode for lab progress:

- Dataset simulation mode is enabled in the panel source.
- Real motor and real OCT camera are temporarily not required for the current
  image/Yolo flow check.
- Clicking Start sends the configured serial acquisition command path if needed,
  but the image stream is currently driven by the dataset folder.
- The real-time raw image window shows dataset raw frames reduced to display
  size.
- The FPGA processed image window sends each 2048 x 4096 frame through XDMA to
  the FPGA and displays the returned processed frame.
- Display/video target is 2048 x 1024 for the UI and YOLO video path.

Important deployed fixes:

- Dataset source uses a small 60-frame folder:

```text
/home/elf/Desktop/yolo/01_panel_stream_raw
```

  This avoids scanning the full 10 GB YOLO dataset on each start.

- Dataset realtime display is about 10 fps:

```text
kDatasetDisplayIntervalMs = 100
```

- No fixed 50-frame auto-stop:

```text
kDatasetAutoStopFrames = 0
```

  User manually stops acquisition.

- Stop responsiveness was improved:
  - stop flag checked between RAW read, preview, XDMA process, saving, and frame
    pacing.
  - pacing sleep is split into 10 ms slices.
  - dataset mode skips slow Zynq serial stop.
  - user stop best-effort resets FPGA path.

- Dataset mode currently forces no per-frame RAW/PGM disk saving for realtime
  display. The panel may still cache FPGA-processed frames for YOLO analysis.

- Analysis must use FPGA-processed images:
  - `predict_batch.py` supports `--require-fpga-processed`.
  - If `fpga_processed_images` is absent, analysis exits instead of silently
    falling back to original/raw YOLO images.
  - The analysis cache target was changed from 50 to 100 FPGA-processed frames.

- "Analysis recognition" behavior:
  - The top-bar button opens an existing analysis video if present.
  - If no video exists, it runs YOLO/RKNN analysis, then opens the video.
  - The old success pop-up was removed.
  - Video player is launched with keep-open behavior so the last frame remains.

- Latency labels:
  - The fixed title stays in the image panel header.
  - The value label only contains numeric text, e.g. ` 101.15 ms`.
  - Updates are throttled to at most once every 200 ms.
  - Values are exponentially smoothed: 75% previous + 25% current.
  - Old per-frame `setUpdatesEnabled(false/true)` repaint toggling was removed.

Latest panel rebuild/restart observed:

```text
make -j2 succeeded on RK3588
running binary: /home/elf/rk3568_capture/qt_panel/build/oct_qt_panel
latest observed pid: 36642
```

## 6. RK3588 YOLO/RKNN Status

User converted/deployed the RKNN model manually. Current relevant path:

```text
/home/elf/Desktop/yolo/03_rk3588_model/
```

Panel integration notes:

- The panel calls the local YOLO/RKNN Python analysis script under this tree.
- Analysis input should be the FPGA-processed frame cache, not the raw dataset.
- A previous bug caused the video to use unprocessed images; this was fixed by
  requiring `fpga_processed_images` through `--require-fpga-processed`.
- Current cache count target is 100 frames, because 50 was not enough.

## 7. PCIe/RK3588 Bring-Up Notes

If FPGA is JTAG reprogrammed or the endpoint disappears/stales on RK3588, run:

```bash
echo elf | sudo -S /usr/local/sbin/ensure_fpga_pcie_ready.sh
```

This script was previously strengthened to avoid accepting an all-`0xffffffff`
BAR as ready. It reloads/rescans XDMA and verifies expected FPGA registers.

Expected devices:

```text
/dev/xdma0_h2c_0
/dev/xdma0_c2h_0
/dev/xdma0_control
```

`resource0` is the user BAR for the register map above. `resource1` is XDMA
internal/control space and should not be treated as the project register window.

A known independent FPGA/XDMA probe result after the `tuxiang_pcie` fixes:

```text
input frame: 2048 x 4096 x uint16 = 16,777,216 bytes
FPGA VERSION: 0x20260619
FRAME_CFG: 0x10000800 = 4096 lines x 2048 samples
H2C_LINES after transfer: 4096
C2H_FRAMES after transfer: 1
output bytes: 16,777,216
elapsed_ms: about 101 ms
PH_EVENTS: 0
```

Conclusion: the PCIe/FPGA image path is fundamentally working. Remaining UI or
video problems should first be checked in RK3588 panel logic and analysis input
selection before assuming PCIe is broken.

## 8. Historical Motor/Zynq/Closed-Loop Notes

These are historical or paused work items. They are preserved here so future AI
sessions understand why old files existed, but they are not the current active
main route.

### Old ShengTeng Pro mixed motor + image route

Old folder:

```text
D:\FPGA20251016\qianrushi\fpga_dianji_tuxiang
```

This folder contained the earlier ShengTeng Pro project combining:

- XDMA image processing
- motor control
- AD7616/DAC8830 CN4 wiring
- GPIO camera trigger experiments
- closed-loop motor PI experiments

That route was abandoned for the active project because it became too complex
and mixed too many unstable parts. The current active ShengTeng Pro project is
`D:\FPGA20251016\tuxiang_pcie`, image processing only.

### Zynq motor/trigger route

There was a later dual-FPGA idea:

```text
RK3588 -> serial -> Zynq controls motor and OCT external trigger
RK3588 -> PCIe -> ShengTeng Pro only does image processing
```

A Zynq UART baseline had commands:

```text
STATUS
CFG FREQ_HZ=2100 HIGH=23810 LOW=23809 THRESH=13000 PULSES=0
START TARGET=65535 CYCLES=1 HOLD_MS=100 HOLD=10000000
STOP
CLEAR
```

But the motor side was paused because the motor control program was not stable
enough. Current image/Yolo progress should not wait on motor validation.

### Code-domain motor closed-loop design

The historical design used direct AD7616 code-domain conversion, not voltage as
an FPGA intermediate variable:

```text
AD7616 adc_code
  -> code validity check
  -> direct piecewise code-to-position conversion
  -> position filter / tracking differentiator
  -> velocity estimate
  -> compare with target velocity / target trajectory
  -> PI / feed-forward correction
  -> DAC code
  -> DAC8830 output
```

Corrected calibration formula:

```text
if code < 7486:
    pos_mm = 0.00002418489 * code - 0.0024635
elif code < 12916:
    pos_mm = 0.00002434954 * code - 0.0037063
elif code < 18346:
    pos_mm = 0.00002464787 * code - 0.0076747
elif code < 23776:
    pos_mm = 0.00002509192 * code - 0.0156921
else:
    pos_mm = 0.00002547721 * code - 0.0249402
```

Keep this only as future reference if motor control resumes. It should not be
mixed back into `tuxiang_pcie` unless the project route explicitly changes.

## 9. Current Known Problems / Next Work

Current user-reported status before cleanup:

- Real-time image display is now basically correct after the 2026-06-20 white-line
  input-source fix below.
- The processed-image analysis video should use FPGA output, not raw images.
  This has been fixed logically with `--require-fpga-processed`; verify in a
  fresh capture/analysis run.
- The latency label flicker was reduced by throttling and smoothing numeric
  updates; verify visually on the RK3588 display.
- Start delay still existed earlier. The largest known causes were full dataset
  scanning, GUI event backlog, and disk writes. Those were mitigated; if delay
  remains, inspect the first frame path in `CaptureWorker::run()` and XDMA reset
  wait, not only the handover notes.
- Stop delay was mitigated by more stop checks and no per-frame saving in
  dataset mode. If it reappears, check whether saving or analysis cache writing
  was re-enabled.

Suggested next checks on a new machine:

1. SSH to RK3588 and confirm panel source matches this handover.
2. Confirm `FRAME_CFG=0x10000800` and `VERSION=0x20260619` from `resource0`.
3. Run one independent XDMA 2048 x 4096 frame test before debugging the GUI.
4. Start the panel, collect about 100 processed frames, stop manually.
5. Click analysis recognition and confirm the video comes from
   `fpga_processed_images`.
6. If the video is stitched or wrong, check the FPGA frame convention first:
   4096 input line tlast events must produce exactly one C2H frame tlast.

## 10. Encoding And Editing Rules

This project has had mojibake problems in Qt/C++ source. Before source edits,
use the local skill:

```text
D:\Tools\skills\codex-local\code-encoding-hygiene\SKILL.md
```

Rules:

- Prefer ASCII comments and patch anchors in source.
- Keep Chinese UI text only when needed, but do not use Chinese strings as patch
  context.
- Avoid PowerShell `Set-Content` whole-file rewrites on source files.
- Use `apply_patch` with stable ASCII anchors for local edits.
- Put long Chinese explanations in this handover, not in source comments.

## 11. 2026-06-20 Dataset Source Correction

User reported that both the realtime raw image and FPGA processed image showed
white horizontal lines. I initially regenerated the small dataset from direct
2D skin PNG images, but that was not the correct project data model. The correct
input to the panel/FPGA is OCT-like line-scan RAW:

```text
2048 depth samples x 4096 lines x uint16 low-12 RAW
```

The realtime raw window should display the assembled line-scan frame, and the
FPGA processed window should display the result of processing those same lines.
It should not use direct 2D skin photos as FPGA input.

Root cause had two parts:

- The small dataset folder used by the panel,
  `/home/elf/Desktop/yolo/01_panel_stream_raw`, contained RAW files that decoded
  into dense horizontal stripe images. An independent RAW preview generated from
  the folder reproduced the white-line image outside the Qt panel, so the input
  source itself was wrong.
- FPGA BAR/resource0 was stale at the same time. Reading resource0 returned
  `0xffffffff` for all registers, so the FPGA processing side was not actually
  ready even though XDMA device files existed.

Final correction applied on RK3588:

```text
active OCT-like line-scan RAW folder:
/home/elf/Desktop/yolo/01_panel_stream_raw

source pool:
/home/elf/Desktop/yolo/01_pseudo_lcoct_raw/{train,test,valid}

wrong direct-skin RAW folder was moved aside as:
/home/elf/Desktop/yolo/01_panel_stream_raw_direct_skin_wrong_<timestamp>
```

The active folder contains 60 OCT-like RAW files, each:

```text
2048 x 4096 x uint16 = 16 MB
low-12 line-scan values
```

Important: do not regenerate `/home/elf/Desktop/yolo/01_panel_stream_raw` from
direct PNG skin photos for FPGA testing. Use the existing pseudo LCOCT RAW files
or regenerate OCT-like line-scan RAW from the proper conversion script.

PCIe/FPGA recovery command used:

```bash
echo elf | sudo -S /usr/local/sbin/ensure_fpga_pcie_ready.sh
```

After recovery, resource0 readback was normal:

```text
resource=/sys/bus/pci/devices/0002:21:00.0/resource0
FRAME_CFG=0x10000800
VERSION=0x20260619
PH_EVENTS=0x00000000
DEBUG_FLAGS=0x000000a8
```

## 14. 2026-06-25 ShengTeng Pro Flash Updated With Current PROC_CTRL Build

The current verified `tuxiang_pcie` bitstream was written into the board
configuration Flash after the PROC_CTRL optimization work.

Source bitstream:

```text
D:\FPGA20251016\tuxiang_pcie\prj\tuxiang_pcie.runs\impl_1\xilinx_dma_pcie_ep.bit
size: 1,635,732 bytes
mtime: 2026-06-25 19:16:05
```

Generated MCS:

```text
D:\FPGA20251016\qianrushi\_codex_generated_temp\tuxiang_pcie_flash_20260625_230035\xilinx_dma_pcie_ep.mcs
format: MCS
interface: SPIx4
size: 16M
payload range: 0x00000000..0x0018F503
```

Flash programming flow:

```text
Vivado 2025.2
cfgmem part: mt25ql128-spi-x1_x2_x4
bridge bit: D:\FPGA20251016\2025.2\Vivado\data\xicom\cfgmem\bitfile\spi_xc7a35t_pullnone.bit
JTAG target: localhost:3121/xilinx_tcf/Digilent/210299767327
FPGA: xc7a35t_0
```

Important note: direct `program_hw_cfgmem` failed with `Failure to set flash
parameters` until the SPI bridge bit was explicitly programmed first. The
successful script was:

```text
D:\FPGA20251016\qianrushi\_codex_generated_temp\tuxiang_pcie_flash_20260625_230035\program_flash_prebridge.tcl
```

Programming result:

```text
Mfg ID: 20
Memory Type: ba
Memory Capacity: 18
Erase Operation successful.
Blank Check Operation successful.
Program/Verify Operation successful.
Flash programming completed successfully.
```

Flash boot verification:

```text
boot_hw_device completed.
Done pin status: HIGH
BOOT_STATUS=00000000000000000000000000000001
CONFIG_STATUS=01010000000100000111100111111100
```

RK3588 verification after Flash boot/re-enumeration:

```text
echo elf | sudo -S /usr/local/sbin/ensure_fpga_pcie_ready.sh
resource=/sys/bus/pci/devices/0002:21:00.0/resource0
FRAME_CFG=0x10000800
VERSION=0x20260619
PH_EVENTS=0x00000000
DEBUG_FLAGS=0x000000a8
PROC_CTRL=0x0000100a
```

Independent one-frame XDMA test after Flash boot:

```text
input frame: 2048 x 4096 x uint16 = 16,777,216 bytes
output bytes: 16,777,216
H2C_LINES=0x00001000 (4096)
C2H_FRAMES=0x00000001
PH_EVENTS=0x00000000
```

Panel was restarted after restoring the correct line-scan RAW source and PCIe
readback was normal:

```text
/home/elf/rk3568_capture/qt_panel/build/oct_qt_panel
latest observed pid after correction: 40961
```

If the white-line problem appears again, first generate an independent preview
from `/home/elf/Desktop/yolo/01_panel_stream_raw` and read `resource0` before
editing Qt or FPGA code.

## 12. qianrushi Cleanup Status

```text
D:\FPGA20251016\qianrushi\fpga_dianji_tuxiang
```

This old mixed motor/image folder is not the active project. The active FPGA
image project is outside this folder:

```text
D:\FPGA20251016\tuxiang_pcie
```

The old `fpga_dianji_tuxiang` folder and generated qianrushi temporary files
were approved for cleanup after this consolidated handover was written. If this
file is being read on a later machine and the old folder still exists, it should
be treated only as historical material, not as the active build source.

## 13. 2026-06-20 ShengTeng Pro Flash Programmed With tuxiang_pcie

The active ShengTeng Pro image-processing bitstream has been written into the
board configuration Flash.

Source bitstream:

```text
D:\FPGA20251016\tuxiang_pcie\prj\tuxiang_pcie.runs\impl_1\xilinx_dma_pcie_ep.bit
size: 1,618,100 bytes
mtime: 2026-06-20 13:20:30
```

Timing from the routed report:

```text
WNS = 0.196 ns
TNS = 0.000 ns
WHS = 0.036 ns
THS = 0.000 ns
All user specified timing constraints are met.
```

Generated MCS:

```text
D:\FPGA20251016\qianrushi\_codex_generated_temp\tuxiang_pcie_flash_20260620\xilinx_dma_pcie_ep.mcs
format: MCS
interface: SPIx4
size: 16M
payload range: 0x00000000..0x0018B023
```

Flash programming flow:

```text
Vivado 2025.2
cfgmem part: mt25ql128-spi-x1_x2_x4
bridge bit: D:\FPGA20251016\2025.2\Vivado\data\xicom\cfgmem\bitfile\spi_xc7a35t_pullnone.bit
JTAG target: localhost:3121/xilinx_tcf/Digilent/210299767327
FPGA: xc7a35t_0
```

Programming result:

```text
Mfg ID: 20
Memory Type: ba
Memory Capacity: 18
Erase Operation successful.
Blank Check Operation successful.
Program/Verify Operation successful.
Flash programming completed successfully.
```

Flash boot verification:

```text
boot_hw_device completed.
Done pin status: HIGH
BOOT_STATUS=00000000000000000000000000000001
```

RK3588 verification after Flash boot/re-enumeration:

```text
echo elf | sudo -S /usr/local/sbin/ensure_fpga_pcie_ready.sh
resource=/sys/bus/pci/devices/0002:21:00.0/resource0
FRAME_CFG=0x10000800
VERSION=0x20260619
PH_EVENTS=0x00000000
DEBUG_FLAGS=0x000000a8
```

Current RK3588 panel process after this operation:

```text
/home/elf/rk3568_capture/qt_panel/build/oct_qt_panel
latest observed pid: 41555
```

## 15. 2026-07-01 FFT Wide-Precision PL Path Build

The clipping/cropping fix is now in the PL FFT/Hilbert/IFFT path, not in the
stream framing. The old RK3588-side 32-bit assumption no longer forces an early
truncate right after the forward FFT.

FPGA source changes:

```text
D:\FPGA20251016\tuxiang_pcie\ph_pcie_ip\ph_core_pipeline.v
D:\FPGA20251016\tuxiang_pcie\ph_pcie_ip\hilbert_filter.v
D:\FPGA20251016\tuxiang_pcie\ph_pcie_ip\axis_cplx_scale_sat_to_32.v
D:\FPGA20251016\tuxiang_pcie\ph_pcie_ip\xfft_0_w29\xfft_0_w29.xci
D:\FPGA20251016\tuxiang_pcie\prj\create_project.tcl
D:\FPGA20251016\tuxiang_pcie\prj\build_bitstream.tcl
D:\FPGA20251016\tuxiang_pcie\pcie\xdma_app.v
```

Algorithm path:

```text
16-bit input -> FFT 28-bit comp
  -> Hilbert at wide PL precision
  -> IFFT with 29-bit component input
  -> 41-bit component output
  -> final arithmetic shift + signed saturation to 16-bit
  -> norm/log/output
```

Important details:

```text
forward FFT IP xfft_in:    C_INPUT_WIDTH=16, C_OUTPUT_WIDTH=28, M_AXIS_TDATA=64
new IFFT IP xfft_0_w29:    C_INPUT_WIDTH=29, C_OUTPUT_WIDTH=41, M_AXIS_TDATA=96
final scale/saturate:      axis_cplx_scale_sat_to_32, FINAL_SCALE_SHIFT=24
VERSION_ID:                0x20260619
```

Generated bitstream:

```text
D:\FPGA20251016\tuxiang_pcie\prj\tuxiang_pcie.runs\impl_1\xilinx_dma_pcie_ep.bit
size: 1,623,124 bytes
mtime: 2026-07-01 19:05:40
```

Vivado verification:

```text
synthesis: complete
implementation/write_bitstream: complete
WNS = 0.009 ns
TNS = 0.000 ns
WHS = 0.040 ns
THS = 0.000 ns
All user specified timing constraints are met.
DRC: 0 errors.
```

JTAG download verification:

```text
JTAG target: localhost:3121/xilinx_tcf/Digilent/210299767327
FPGA: xc7a35t_0
Vivado program_hw_devices completed
End of startup status: HIGH
```

RK3588 current WiFi address:

```text
IP: 192.168.110.119
hostname: elf2-desktop
login: elf works over SSH
```

Current RK3588 validation status:

```text
PCIe endpoint enumerates as 0002:21:00.0, vendor/device 10ee:9040.
ensure_fpga_pcie_ready.sh now succeeds without modification because FPGA
VERSION_ID remains 0x20260619.
/dev/xdma0_control, /dev/xdma0_h2c_0, /dev/xdma0_c2h_0 are present.
```

Smoke test result:

```text
input=..._2048x4096_low12.raw
version=0x20260619
frame_cfg=0x10000800
output bytes=16777216
h2c_lines=0x00001000 (4096)
c2h_frames=0x00000001 (1)
ph_events=0x00000000 (0)
user_clk_hz=0x07735940 (125000000)
line_cycles=0x00002b0d (11021)
```

Conclusion:

```text
The wide-precision PL path is in place, the current bitstream is programmed,
RK3588 PCIe/XDMA is healthy, and one independent full-frame smoke test passed.
```

## 16. 2026-07-01 FPGA-Aligned YOLO RKNN Deployment

The FPGA-processed retraining result was deployed to RK3588 without changing the
Qt panel logic.

Windows source model:

```text
C:\Users\mechrevo\Desktop\yolo\competition_same_patient_lcoct_yolo_20260701_fpga_processed\runs\yolov8n_competition_same_patient_fpga_aligned\weights\best.onnx
```

RKNN conversion:

```text
toolkit: rknn-toolkit2 2.3.2 on RK3588
target_platform: rk3588
quantization: disabled
temporary output: /tmp/new_fpga_yolo_model/yolov8n_competition_fpga_aligned.rknn
deployed output: /home/elf/Desktop/yolo/03_rk3588_model/yolov8n_competition_fpga_aligned.rknn
size: 7,904,181 bytes
```

Deployed RK3588 layout:

```text
/home/elf/Desktop/yolo/01_panel_stream_raw
  75 competition_*_2048x4096_low12.raw test frames

/home/elf/Desktop/yolo/02_ip_reference_yolo_dataset/test/images
  75 FPGA-processed PNG images

/home/elf/Desktop/yolo/02_ip_reference_yolo_dataset/test/labels
  75 YOLO label files

/home/elf/Desktop/yolo/03_rk3588_model
  best.onnx
  data.yaml
  predict_batch.py
  yolov8n_competition_fpga_aligned.rknn
```

Cleanup performed:

```text
Old /home/elf/Desktop/yolo was deleted and rebuilt.
Old yolo_fpga_processed_dataset_20260619.tar.gz was deleted.
Temporary fpga_processed_TEST dataset was deleted.
Current competition_same_patient_lcoct_yolo_20260701 source dataset remains on
the RK3588 desktop because it is the source data for this deployment.
```

Deployment smoke test:

```text
python3 predict_batch.py \
  --input-dir /home/elf/Desktop/yolo/02_ip_reference_yolo_dataset/test/images \
  --output-csv deploy_smoke.csv \
  --output-jsonl deploy_smoke.jsonl \
  --max-frames 5 \
  --display never \
  --require-fpga-processed

engine: rknn:yolov8n_competition_fpga_aligned.rknn
records: 5
scores observed: about 0.986 to 0.988
RC: 0
```

Smoke-test output videos/CSVs were removed after verification so the panel does
not accidentally open a stale analysis video. The temporary conversion directory
under `/tmp/new_fpga_yolo_model` was also removed after deployment. The Qt panel
process was restarted:

```text
/home/elf/rk3568_capture/qt_panel/build/oct_qt_panel
latest observed pid: 488418
```

## 17. 2026-07-01 ShengTeng Pro Flash Updated With Wide-Precision Build

The FFT/Hilbert/IFFT wide-precision `tuxiang_pcie` bitstream was written into
the ShengTeng Pro configuration Flash. This replaces the earlier Flash image;
the board now boots the wide-precision image-processing design without needing
JTAG download.

Source bitstream:

```text
D:\FPGA20251016\tuxiang_pcie\prj\tuxiang_pcie.runs\impl_1\xilinx_dma_pcie_ep.bit
size: 1,623,124 bytes
mtime: 2026-07-01 19:05:40
VERSION_ID: 0x20260619
```

Generated MCS:

```text
D:\FPGA20251016\qianrushi\_codex_generated_temp\tuxiang_pcie_flash_20260701_wide\xilinx_dma_pcie_ep.mcs
format: MCS
interface: SPIx4
size: 16M
```

Flash programming flow:

```text
Vivado 2025.2
cfgmem part: mt25ql128-spi-x1_x2_x4
bridge bit: D:\FPGA20251016\2025.2\Vivado\data\xicom\cfgmem\bitfile\spi_xc7a35t_pullnone.bit
JTAG target: localhost:3121/xilinx_tcf/Digilent/210299767327
FPGA: xc7a35t_0
successful script:
D:\FPGA20251016\qianrushi\_codex_generated_temp\tuxiang_pcie_flash_20260701_wide\program_flash_prebridge.tcl
```

Programming result:

```text
Erase Operation successful.
Blank Check Operation successful.
Program/Verify Operation successful.
Flash programming completed successfully.
boot_hw_device completed.
Done pin status: HIGH.
```

RK3588 verification after Flash boot/re-enumeration:

```text
sudo -n /usr/local/sbin/ensure_fpga_pcie_ready.sh
BAR_PROBE version=0x20260619
/dev/xdma0_control present
/dev/xdma0_h2c_0 present
/dev/xdma0_c2h_0 present
READY
```

Independent one-frame XDMA smoke test after Flash boot:

```text
input=/home/elf/Desktop/yolo/01_panel_stream_raw/competition_0426_ISIC_5718490_2048x4096_low12.raw
version=0x20260619
frame_cfg=0x10000800
output bytes=16777216
elapsed_ms=108.090
h2c_lines=0x00001000 (4096)
c2h_frames=0x00000001 (1)
ph_events=0x00000000 (0)
user_clk_hz=0x07735940 (125000000)
line_cycles=0x00002b6a (11114)
```

## 18. 2026-07-02 Qt Panel FPGA/YOLO Analysis Fix

Symptom reported on the RK3588 panel:

```text
Realtime raw image and FPGA processed image looked identical.
"Analyze/recognize" did not work from the panel.
```

Root cause found:

```text
1. The panel dataset/line-image mode still had a temporary bypass around
   ensureFpgaPcieReady(), so it could continue when the PCIe BAR/XDMA state was
   stale. The ready log previously showed BAR_PROBE version=0xffffffff before
   recovery.
2. In that fallback path, the UI could display a local preview as the processed
   image. This made the panel look alive while no true FPGA processed cache was
   available for YOLO analysis.
3. The dataset finite-run completion flag in capture_worker.cpp was inverted:
   a normal finite run emitted ok=false, so auto-test/auto-analysis treated a
   successful 100-frame capture as capture_failed.
```

RK3588 source files changed:

```text
/home/elf/rk3568_capture/qt_panel/mainwindow.cpp
/home/elf/rk3568_capture/qt_panel/capture_worker.cpp
```

Fixes deployed:

```text
mainwindow.cpp:
- Dataset mode now also calls ensureFpgaPcieReady() before capture starts.
- Python analysis is blocked with a clear message if the diagnosis directory
  has no fpga_processed_images input.
- Hidden --auto-dataset-test mode now auto-stops at 100 frames; normal panel
  use remains continuous/manual-stop.

capture_worker.cpp:
- Dataset mode no longer silently substitutes local fallback preview when FPGA
  processing fails. FPGA failure now stops the capture with an explicit error.
- Finite dataset completion now emits finished(!stopped, ...), so successful
  100-frame capture can proceed to auto-analysis.
```

Build result:

```text
/home/elf/rk3568_capture/qt_panel/build/oct_qt_panel
built on RK3588: 2026-07-02 07:27 UTC
binary sha256:
9d0de389cbcbaee1b78d15878327dcf8bc30ad2f0049cac2f087b45a1bdfa381
```

Verification:

```text
auto test:
DISPLAY=:0 XAUTHORITY=/run/user/1000/gdm/Xauthority \
  /home/elf/rk3568_capture/run_qt_panel.sh \
  --auto-dataset-test \
  --auto-test-output-dir /tmp/panel_fpga_model_test_20260702c

result:
status=analysis_done
analysis_exit_code=0
diagnosis_dir=/home/elf/rk3568_capture/captures/20260702_第002次诊断
analysis_dir=/home/elf/rk3568_capture/captures/20260702_第002次诊断/analysis
```

Latest verified capture/analysis contents:

```text
fpga_processed_images/*.pgm: 100
fpga_processed_images/*.txt: 100
analysis records: 100
raw_video_frames: 100
processed_video_frames: 100
engine: rknn:yolov8n_competition_fpga_aligned.rknn
rknn model:
/home/elf/Desktop/yolo/03_rk3588_model/yolov8n_competition_fpga_aligned.rknn
```

Representative FPGA metadata:

```text
fpga_used=true
fpga_proc_ctrl=0x0000108a
fpga_norm_shift=10
fpga_out_shift=8
fpga_log_gain_q4_4=16
fpga_log_offset=0
pcie_roundtrip_ms=94.7743
fpga_line_core_ms=0.089184
```

Current expected user workflow:

```text
1. Launch the Qt panel normally.
2. Start acquisition. The raw and FPGA processed panes should be visibly
   different when FPGA/XDMA is ready.
3. Stop after enough frames have displayed.
4. Click analysis/recognition. The panel should use the cached
   fpga_processed_images and the deployed RKNN model, not raw images.
```

## 19. 2026-07-02 Dataset Source Correction For Qt Panel

After a user check on the RK3588 desktop, the realtime panel dataset source was
found to still point at the temporary deployment cache:

```text
/home/elf/Desktop/yolo/01_panel_stream_raw
```

That was not the intended current test-set source. The correct realtime source
is the test split inside the competition dataset on the RK3588 desktop:

```text
dataset root:
/home/elf/Desktop/competition_same_patient_lcoct_yolo_20260701

YOLO test images:
/home/elf/Desktop/competition_same_patient_lcoct_yolo_20260701/test/images

FPGA input raw line images used by the panel:
/home/elf/Desktop/competition_same_patient_lcoct_yolo_20260701/raw/test
```

Dataset check:

```text
test/images/*.png: 75
raw/test/*_2048x4096_low12.raw: 75
PNG-to-RAW basename matches: 75/75
first test raw:
competition_0426_ISIC_5718490_2048x4096_low12.raw
last test raw:
competition_0500_ISIC_7625300_2048x4096_low12.raw
```

RK3588 Qt source updates:

```text
/home/elf/rk3568_capture/qt_panel/mainwindow.cpp
  dataset_root_dir =
  ~/Desktop/competition_same_patient_lcoct_yolo_20260701/raw/test

/home/elf/rk3568_capture/qt_panel/capture_worker.cpp
  fallback dataset root =
  ~/Desktop/competition_same_patient_lcoct_yolo_20260701/raw/test
```

The hidden auto-test frame count was also set to 75 so the remote verification
uses exactly the current test split once.

Verification after rebuild:

```text
auto test:
/home/elf/rk3568_capture/run_qt_panel.sh \
  --auto-dataset-test \
  --auto-test-output-dir /tmp/panel_competition_test_source_20260702

result:
status=analysis_done
analysis_exit_code=0
diagnosis_dir=/home/elf/rk3568_capture/captures/20260702_第004次诊断

fpga_processed_images/*.pgm: 75
fpga_processed_images/*.txt: 75
analysis records: 75
raw_video_frames: 75
processed_video_frames: 75
engine: rknn:yolov8n_competition_fpga_aligned.rknn
```

Representative metadata from the corrected run:

```text
first source_dataset_raw:
/home/elf/Desktop/competition_same_patient_lcoct_yolo_20260701/raw/test/competition_0426_ISIC_5718490_2048x4096_low12.raw

last source_dataset_raw:
/home/elf/Desktop/competition_same_patient_lcoct_yolo_20260701/raw/test/competition_0500_ISIC_7625300_2048x4096_low12.raw

fpga_used=true
fpga_proc_ctrl=0x0000108a
```

## 20. 2026-07-02 Correct OCT Stripe Source And No-Crop RK Cache

The section above records an intermediate correction to `raw/test`. That was
later found to be the wrong visual source for the Qt panel. The current user
requirement is:

```text
Realtime raw pane: competition dataset stripe_preview/test OCT line images
FPGA input: the same stripe_preview/test OCT line image, expanded to 16-bit
FPGA output cache: full 2048 x 4096, no vertical reduction/cropping
YOLO/RKNN analysis input: fpga_processed_images cache
```

Correct current source on RK3588:

```text
/home/elf/Desktop/competition_same_patient_lcoct_yolo_20260701/stripe_preview/test
```

Important distinction:

```text
stripe_preview/test/*.png  = OCT stripe/line images for live raw display and FPGA input
raw/test/*.raw             = low12 raw source, not the current panel display source
test/images/*.png          = YOLO enhanced/skin-patch images, not live raw display
```

RK3588 Qt source updates:

```text
/home/elf/rk3568_capture/qt_panel/mainwindow.cpp
  dataset_root_dir =
  ~/Desktop/competition_same_patient_lcoct_yolo_20260701/stripe_preview/test

/home/elf/rk3568_capture/qt_panel/capture_worker.cpp
  fallback dataset root =
  ~/Desktop/competition_same_patient_lcoct_yolo_20260701/stripe_preview/test

  readDatasetStripePreviewFile():
    reads 2048 x 4096 8-bit grayscale PNG
    displays it directly in the raw pane
    expands each pixel to 16-bit by left-shifting 4 bits before XDMA H2C

  FPGA output validation:
    requires 2048 x 4096 x 16-bit output
    throws an error on any output-size mismatch
    saves processed PGM at full 2048 x 4096

  Analysis cache:
    raw_images gets the original stripe_preview PNG, full 2048 x 4096
    fpga_processed_images gets the FPGA PGM, full 2048 x 4096

  Live display:
    raw/FPGA QLabel previews use IgnoreAspectRatio so the 2048 x 4096
    stripe image visually fills the panel; this is display-only and does not
    crop or resize saved/analysis data.
```

Verification after rebuild:

```text
auto test:
DISPLAY=:0 XAUTHORITY=/run/user/1000/gdm/Xauthority \
  /home/elf/rk3568_capture/qt_panel/build/oct_qt_panel \
  --auto-dataset-test \
  --auto-test-output-dir /tmp/oct_stripe_auto_test2

result:
status=analysis_done
analysis_exit_code=0
diagnosis_dir=/home/elf/rk3568_capture/captures/20260702_第007次诊断

raw_images/*.png: 75
fpga_processed_images/*.pgm: 75
fpga_processed_images/*.txt: 75
analysis records: 75
raw_video_frames: 75
processed_video_frames: 75
engine: rknn:yolov8n_competition_fpga_aligned.rknn
```

Representative file and metadata checks:

```text
raw PNG:
2048 x 4096, 8-bit grayscale, source from stripe_preview/test

FPGA processed PGM:
width=2048
height=4096
maxval=65535
bytes=16777235
source_dataset_stripe_preview=/home/elf/Desktop/competition_same_patient_lcoct_yolo_20260701/stripe_preview/test/competition_0426_ISIC_5718490_stripes.png
fpga_input_width=2048
fpga_input_height=4096
fpga_output_uncropped=true
fpga_used=true
fpga_proc_ctrl=0x0000108a
```

FPGA PCIe register check after the verified run:

```text
FRAME_CFG=0x10000800
VERSION=0x20260619
H2C_LINES=0x00001000
C2H_FRAMES=0x00000001
PH_EVENTS=0x00000000
LINE_COUNT=0x00001000
PROC_CTRL=0x0000108a
```

## 21. 2026-07-03 LCOCT Inverse Stripe/Raw FPGA Training Package

User uploaded this source dataset on the RK3588 desktop:

```text
/home/elf/Desktop/lcoct_inverse_500_visual_stripe_20260702
```

The `stripe_preview` and `raw` folders were both processed through the active
ShengTeng Pro FPGA pipeline. The result was exported as two YOLO-compatible
datasets:

```text
/home/elf/Desktop/lcoct_inverse_500_visual_stripe_20260702_fpga_processed_20260703
  stripe_preview_fpga_processed/
    train/images + train/labels: 350 + 350
    valid/images + valid/labels: 75 + 75
    test/images  + test/labels:  75 + 75
  raw_fpga_processed/
    train/images + train/labels: 350 + 350
    valid/images + valid/labels: 75 + 75
    test/images  + test/labels:  75 + 75
```

Output image format:

```text
8-bit grayscale PNG
2048 x 4096
filenames normalized to match YOLO label stems
original source filenames recorded in processing_records.csv
```

Validation:

```text
stripe_preview_fpga_processed records: ok=500, bad=0
raw_fpga_processed records:            ok=500, bad=0
FRAME_CFG=0x10000800
VERSION=0x20260619
PH_EVENTS=0x00000000 for all records
image/label stems matched for all train/valid/test splits
```

Package copied to the Windows desktop yolo folder:

```text
C:\Users\mechrevo\Desktop\yolo\lcoct_inverse_500_visual_stripe_20260702_fpga_processed_20260703.zip
C:\Users\mechrevo\Desktop\yolo\lcoct_inverse_500_visual_stripe_20260702_fpga_processed_20260703.zip.sha256
```

Package size and checksum:

```text
zip bytes: 527875862
sha256: 87cdd192cf6197d4e18fe3c8d992c702d6d4467119e7e15b151b73e51e9c12f0
```

After processing, the Qt panel was restarted on the active RK desktop X session:

```text
DISPLAY=:1
process: /home/elf/rk3568_capture/qt_panel/build/oct_qt_panel
```

## 22. 2026-07-04 6.20 FFT/Hilbert Core Flow Restore Build

The user and teammates reported that the active FPGA output looked more like a
gray-level remap than the expected OCT/Hilbert reconstruction. The 2026-06-20
FPGA core flow was identified as the correct algorithmic reference, except for
its earlier fixed-point truncation/cropping issue.

What was changed:

```text
D:\FPGA20251016\tuxiang_pcie\ph_pcie_ip\ph_core_pipeline.v
```

The core pipeline was changed away from the 2026-07-01 wide-precision branch:

```text
old 2026-07-01 branch:
  FFT -> 64-bit Hilbert -> xfft_0_w29 IFFT -> final wide-to-16 scale/sat

new 2026-07-04 build:
  FFT -> scale/saturate to 32-bit complex
      -> Hilbert(DATA_W=32)
      -> old xfft_0 IFFT
      -> scale/saturate to 32-bit complex
      -> norm/log/output
```

Important detail: this restores the 2026-06-20 core flow position of
`Hilbert(DATA_W=32)` and old `xfft_0`, but does not directly restore the old
bare `axis_cplx_trunc_64_to_32` hard slice. The two 64-to-32 conversion points
now use `axis_cplx_scale_sat_to_32` to reduce hard truncation risk.

Backup of pre-edit HDL:

```text
D:\FPGA20251016\qianrushi\_codex_generated_temp\fpga_hdl_backup_20260703_233713
```

Vivado build log:

```text
D:\FPGA20251016\tuxiang_pcie\prj\build_bitstream_620flow_nocrop_20260703_234521.log
```

Build result:

```text
SYNTH_STATUS=synth_design Complete!
IMPL_STATUS=write_bitstream Complete!
route estimated timing: WNS=0.127 ns, TNS=0.000, WHS=0.026, THS=0.000
DRC before bitgen: 0 errors
```

Generated bitstream:

```text
D:\FPGA20251016\tuxiang_pcie\prj\tuxiang_pcie.runs\impl_1\xilinx_dma_pcie_ep.bit
size: 1,616,348 bytes
mtime: 2026-07-03 23:57:05
```

JTAG download status:

```text
NOT DOWNLOADED TO FPGA YET
NOT FLASHED
```

Reason: this work was done while the user was connected remotely. The Windows
machine did not enumerate any Digilent/Xilinx/FTDI/JTAG USB device, and Vivado
hardware manager could not find a hardware target.

JTAG checks attempted:

```text
D:\FPGA20251016\qianrushi\_codex_generated_temp\tuxiang_pcie_jtag_check\program_620flow_nocrop_20260703.log
D:\FPGA20251016\qianrushi\_codex_generated_temp\tuxiang_pcie_jtag_check\refresh_list_hw_20260704.log

refresh_hw_server result:
TARGET_COUNT=0
Vivado warning: No hardware targets exist on the server [localhost:3121]
```

Windows PnP check also showed no present Digilent/Xilinx/FTDI/JTAG-like USB
device. This points to the downloader/cable not being visible to the PC, not a
Tcl script problem.

RK3588 was still reachable over WiFi and the PCIe endpoint was still alive:

```text
ssh: elf@192.168.110.119, password elf
hostname: elf2-desktop
PCIe endpoint: 0002:21:00.0 Xilinx Corporation Device 9040
```

Current RK3588 BAR state before downloading the new build:

```text
FRAME_CFG=0x10000800
VERSION=0x20260619
H2C_LINES=0x00001000
C2H_FRAMES=0x00000001
PH_EVENTS=0x00000000
DEBUG_FLAGS=0x000000a8
PROC_CTRL=0x0000108a
```

Next physical-site step:

```text
1. Check ShengTeng Pro board power and JTAG/USB downloader connection.
2. Confirm Windows Device Manager/PnP shows a Digilent/Xilinx/FTDI/JTAG device.
3. Re-run JTAG download only:
   D:\FPGA20251016\qianrushi\_codex_generated_temp\tuxiang_pcie_jtag_check\program_tuxiang_pcie_bit.tcl
4. After download, re-check RK3588 BAR registers and run the one-frame XDMA test.
5. Only after visual/RK validation should this bitstream be flashed.
```

## 23. 2026-07-04 6.20-Flow Build JTAG Downloaded And One-Frame Verified

After the user plugged the JTAG downloader into the Windows PC, the downloader
was visible again:

```text
USB Serial Converter
USB\VID_0403&PID_6014\210299767327
```

The 2026-07-04 6.20-flow restore bitstream was downloaded by JTAG only:

```text
bitstream:
D:\FPGA20251016\tuxiang_pcie\prj\tuxiang_pcie.runs\impl_1\xilinx_dma_pcie_ep.bit

program log:
D:\FPGA20251016\qianrushi\_codex_generated_temp\tuxiang_pcie_jtag_check\program_620flow_nocrop_20260704_after_plugin.log

JTAG target:
localhost:3121/xilinx_tcf/Digilent/210299767327

Vivado result:
PROGRAM_DEVICE: xc7a35t_0 PART=xc7a35t
PROGRAM_DONE
```

Important status:

```text
Downloaded to FPGA by JTAG: yes
Flashed to configuration memory: no
```

RK3588 BAR check immediately after JTAG download:

```text
FRAME_CFG=0x10000800
VERSION=0x20260619
H2C_LINES=0x00000000
C2H_FRAMES=0x00000000
PH_EVENTS=0x00000000
DEBUG_FLAGS=0x000000a8
PROC_CTRL=0x0000100a
```

Independent one-frame XDMA test:

```text
command:
echo elf | sudo -S python3 /home/elf/rk3588_tuxiang_pcie_test.py

version=0x20260619
frame_cfg=0x10000800 depth=2048 lines=4096
elapsed_ms=101.200
output_nonzero=True
same_as_input=False
h2c_words=0x00200000 (2097152)
h2c_samples=0x00800000 (8388608)
h2c_lines=0x00001000 (4096)
c2h_words=0x00200000 (2097152)
c2h_frames=0x00000001 (1)
ph_events=0x00000000 (0)
line_count=0x00001000 (4096)
fpga_line_core_ms=0.089056
```

BAR check after the one-frame test:

```text
FRAME_CFG=0x10000800
VERSION=0x20260619
H2C_LINES=0x00001000
C2H_FRAMES=0x00000001
PH_EVENTS=0x00000000
LINE_COUNT=0x00001000
DEBUG_FLAGS=0x000000a8
PROC_CTRL=0x0000100a
```

The Qt panel was restarted on the RK3588 desktop:

```text
DISPLAY=:0
process:
/home/elf/rk3568_capture/qt_panel/build/oct_qt_panel
pid observed: 80135
```

The panel was running, but the FPGA counters did not advance further without a
new UI-triggered capture action. Visual validation from the RK desktop is still
needed before flashing this bitstream.

Recommended next step:

```text
1. Use the RK3588 panel to start a fresh capture and visually compare:
   left realtime OCT stripe image vs right FPGA processed image.
2. Confirm processed image is no longer only a gray remap and has no RK-side crop.
3. Click/analyze with the deployed model if needed.
4. Only after the visual result is accepted, generate MCS and flash this exact
   JTAG-tested bitstream.
```

## 24. RK3588 panel short capture screenshot and cleanup, 2026-07-04

User could not see the RK3588 desktop remotely, so a controlled panel capture
was triggered over SSH and the panel window was screenshotted.

Important notes:

```text
Panel binary still expects:
/home/elf/Desktop/competition_same_patient_lcoct_yolo_20260701/stripe_preview/test

The real remaining dataset on the RK3588 desktop is:
/home/elf/Desktop/lcoct_inverse_500_visual_stripe_20260702/stripe_preview/test
```

A lightweight compatibility symlink was created so the existing panel binary can
find the current dataset without copying data:

```text
/home/elf/Desktop/competition_same_patient_lcoct_yolo_20260701
-> /home/elf/Desktop/lcoct_inverse_500_visual_stripe_20260702
```

Short capture result:

```text
Window screenshot copied to Windows:
D:\FPGA20251016\qianrushi\_codex_generated_temp\rk_panel_screenshots_20260704\rk_panel_live_panel_20260704.png

Screenshot showed frame 7 displayed in the Qt panel.
Left: realtime raw OCT stripe image.
Right: FPGA processed image.
Panel latency display:
  link latency: about 94.62 ms
  FPGA per-line latency: about 0.0895 ms
```

The automatic dataset-test mode was too slow to reach its internal 75-frame
completion point before timeout, and the current binary still wrote temporary
raw/FPGA processed diagnostic images during the test. The capture was therefore
stopped manually after screenshot capture.

Cleanup completed:

```text
No oct_qt_panel or predict_batch.py process was left running during cleanup.
/home/elf/rk3568_capture/captures was cleared.
Final size: 4.0K
Temporary /tmp/rk_panel_* screenshot/test files were removed from RK3588.
```

The Qt panel was restarted idle on the RK3588 desktop after cleanup:

```text
/home/elf/rk3568_capture/qt_panel/build/oct_qt_panel --capture-tab
pid observed: 84810
/home/elf/rk3568_capture/captures remains 4.0K
```

## 25. FPGA linear envelope validation against desktop dataset originals, 2026-07-05

The user reported that the FPGA processed image still looked like only a gray
or color remap. The active pipeline was checked against the dataset on the
RK3588 desktop:

```text
/home/elf/Desktop/lcoct_inverse_500_visual_stripe_20260702
  raw/test/*_2048x4096_low12.raw
  stripe_preview/test/*_visual_stripes.png
  linear16/test/*_linear16.png
  test/images/*.png
```

Root cause found:

```text
The FFT/Hilbert/IFFT core was producing the right structure, but the previous
last stage still behaved like a compressed/under-scaled image. This made the
panel image look visually flat even though the structural correlation existed.
```

HDL changes:

```text
D:\FPGA20251016\tuxiang_pcie\ph_pcie_ip\linear_mag_comp.v
  New pipelined linear magnitude stage:
  max(abs(re), abs(im)) + 1/2 * min(abs(re), abs(im))

D:\FPGA20251016\tuxiang_pcie\ph_pcie_ip\ph_core_pipeline.v
  Final log_comp stage replaced by linear_mag_comp.

D:\FPGA20251016\tuxiang_pcie\pcie\ascent_pcie_ph_app.v
  Default PROC_CTRL changed to 0x00671C00:
    norm_shift=0
    out_shift=0
    linear_gain_q4_4=28
    linear_offset=103
```

Important build note:

```text
The first linear_mag_comp version had a long combinational path and failed
timing. It was rewritten as a registered AXI-Stream pipeline.

Final routed timing:
  WNS = +0.260 ns
  TNS = 0.000 ns
  route errors = 0
  DRC bitgen errors = 0

Bitstream:
D:\FPGA20251016\tuxiang_pcie\prj\tuxiang_pcie.runs\impl_1\xilinx_dma_pcie_ep.bit
mtime: 2026-07-05 01:57:44
```

JTAG status:

```text
Downloaded to FPGA by JTAG: yes
Program log:
D:\FPGA20251016\qianrushi\_codex_generated_temp\tuxiang_pcie_jtag_check\program_linear_mag_gain28_off103_20260705.log

Flashed to configuration memory: no
Do not flash until the user accepts the visual result.
```

RK3588 BAR check after JTAG download:

```text
FRAME_CFG=0x10000800
VERSION=0x20260619
H2C_LINES=0x00001000
C2H_FRAMES=0x00000001
PH_EVENTS=0x00000000
PROC_CTRL=0x00671c00
```

One-frame and six-sample validation:

```text
Every sample:
  H2C_LINES=4096
  C2H_FRAMES=1
  PH_EVENTS=0
  PROC_CTRL=0x00671C00

6-sample test set:
  competition_0426_ISIC_5718490
  competition_0427_ISIC_9781221
  competition_0428_ISIC_1566470
  competition_0429_ISIC_9674418
  competition_0430_ISIC_2669185
  competition_0431_ISIC_5522937

FPGA avg4 vs linear16:
  corr about 0.9943 for all six samples
  RMSE about 48.5 to 49.4

FPGA avg4 vs test/images original:
  corr about 0.904 to 0.906
```

Visual comparison files on Windows:

```text
D:\FPGA20251016\qianrushi\_codex_generated_temp\fpga_linear_mag_validate_20260705\fpga_linear_mag_gain28_off103_vs_linear16_sheet.png
D:\FPGA20251016\qianrushi\_codex_generated_temp\fpga_linear_mag_validate_20260705\skin_original_oct_fpga_linear16_comparison_0426.png
D:\FPGA20251016\qianrushi\_codex_generated_temp\fpga_linear_mag_validate_20260705\multi_compare\multi_sample_original_stripe_fpga_linear16_contact_sheet.png
D:\FPGA20251016\qianrushi\_codex_generated_temp\fpga_linear_mag_validate_20260705\multi_compare\multi_sample_original_stripe_fpga_linear16_stats.json
```

Qt panel deployment:

```text
Backed up RK panel source:
/home/elf/rk3568_capture/qt_panel/mainwindow.cpp.bak_20260704_180703
/home/elf/rk3568_capture/qt_panel/capture_worker.cpp.bak_20260704_180703

Synced patched files:
/home/elf/rk3568_capture/qt_panel/mainwindow.cpp
/home/elf/rk3568_capture/qt_panel/capture_worker.cpp

Build command:
cd /home/elf/rk3568_capture/qt_panel && cmake --build build -j2

Build result:
[100%] Built target oct_qt_panel
```

Cleanup:

```text
Removed temporary FPGA raw dumps and asset tar files from RK /tmp.
Removed old RK qt_panel screenshot/temp diagnostic files.
/home/elf/rk3568_capture/captures remains empty.
```

## 26. 2026-07-06 FPGA Image Pipeline Finalized And Flashed

Current status:

```text
The current ShengTeng Pro FPGA image-processing bitstream has been built,
programmed into SPI Flash, booted back through the FPGA, and verified from
RK3588 through PCIe/XDMA.

Next project step:
Wait for the teammate's regenerated OCT images/dataset and newly trained model.
After they are provided, convert/deploy the new model to RKNN on RK3588, remove
old temporary model/data files as needed, and keep the Qt panel's existing raw
stripe display + FPGA processed display + analysis workflow unchanged.
```

Active FPGA source:

```text
D:\FPGA20251016\tuxiang_pcie
```

Copy prepared for teammate/U disk:

```text
E:\fpga_tuxiang_pcie
```

Important HDL state in this build:

```text
ph_pcie_ip/ph_core_pipeline.v
  Keeps the 6.19 FFT -> Hilbert -> IFFT core flow.
  Uses axis_cplx_scale_sat_to_32 arithmetic shift + saturation instead of raw
  64-bit-to-32-bit slicing at FFT/IFFT width transitions.
  PROC_CTRL is connected through norm_mag/log_comp.

pcie/ascent_pcie_ph_app.v
  Default PROC_CTRL = 0x0000100a:
  norm_shift=10, out_shift=0, log_gain=16, offset=0.

ph_pcie_ip/ph_ip.v
  Busy counter underflow/overflow guard added.

ph_pcie_ip/axis_gray8_packer.v
  Compatibility wrapper retained; actual C2H output remains 16-bit through
  axis_u16_packer.
```

Vivado build:

```text
Command:
D:\FPGA20251016\2025.2\Vivado\bin\vivado.bat -mode batch -source D:\FPGA20251016\tuxiang_pcie\prj\build_bitstream.tcl

Build log:
D:\FPGA20251016\tuxiang_pcie\prj\build_bitstream_20260706_package.log

Bitstream:
D:\FPGA20251016\tuxiang_pcie\prj\tuxiang_pcie.runs\impl_1\xilinx_dma_pcie_ep.bit

Bitstream size:
1,588,332 bytes

Bitstream SHA256:
A711A3040D89CD1AF375CB8E140C7FBFCDD5DD39BC8CD5026339767C78354FED

Timing:
WNS=0.222 ns, TNS=0, WHS=0.036 ns. All constraints met.
Route errors: 0.
```

Flash programming:

```text
MCS:
D:\FPGA20251016\qianrushi\_codex_generated_temp\tuxiang_pcie_flash_20260706_package\xilinx_dma_pcie_ep_20260706_package.mcs

MCS SHA256:
FEF3F9660B18E64A52252BEA5BFA56892191F495D701841FA1F14ADED9FAEC70

Flash log:
D:\FPGA20251016\qianrushi\_codex_generated_temp\tuxiang_pcie_flash_20260706_package\program_flash_prebridge.log

Flash device:
mt25ql128-spi-x1_x2_x4

Bridge bitstream:
D:\FPGA20251016\2025.2\Vivado\data\xicom\cfgmem\bitfile\spi_xc7a35t_pullnone.bit

JTAG target:
localhost:3121/xilinx_tcf/Digilent/210299767327

Flash result:
Erase Operation successful.
Blank Check Operation successful.
Program/Verify Operation successful.
Flash programming completed successfully.
Done pin status: HIGH.
done_pin=1.
```

RK3588 post-Flash PCIe/BAR check:

```text
RK3588 SSH:
elf@192.168.110.119
password: elf
sudo password: elf

XDMA nodes present after ensure_fpga_pcie_ready.sh:
/dev/xdma0_h2c_0
/dev/xdma0_c2h_0
/dev/xdma0_control

BAR values before one-frame smoke:
FRAME_CFG=0x10000800
VERSION=0x20260619
H2C_LINES=0x00000000
C2H_FRAMES=0x00000000
PH_EVENTS=0x00000000
PROC_CTRL=0x0000100a

Note:
Use mmap to read /sys/bus/pci/devices/.../resource0. Plain read() can return
OSError: EIO.
```

One-frame XDMA smoke test after Flash boot:

```text
Command:
echo elf | sudo -S python3 /home/elf/rk3588_tuxiang_pcie_test.py

Result:
version=0x20260619
frame_cfg=0x10000800 depth=2048 lines=4096
elapsed_ms=101.251
output_nonzero=True
same_as_input=False
status=0x000000e1
h2c_words=0x00200000
h2c_samples=0x00800000
h2c_lines=0x00001000 (4096)
c2h_words=0x00200000
c2h_frames=0x00000001 (1)
backpressure=0xabac8098
ph_events=0x00000000 (0)
user_clk_hz=0x07735940 (125000000)
line_cycles=0x00002b64 (11108)
line_count=0x00001000 (4096)
fpga_line_core_ms=0.088864
```

## 27. 2026-07-06 FPGA-Real Benign YOLO RKNN Deployment

User-provided model package on Windows:

```text
C:\Users\mechrevo\Desktop\yolo\send_to_classmate_fpga_real_benign_20260706
```

Important source-data decision:

```text
Use the RK3588 desktop lcoct_inverse_* dataset for this model.
Do not mix it with the new ph2_fpga_* backup dataset.

Current active source dataset:
/home/elf/Desktop/lcoct_inverse_500_fpga_current_20260706

Current FPGA-real processed dataset:
/home/elf/Desktop/lcoct_inverse_500_fpga_real_processed_20260706

Backup dataset left untouched:
/home/elf/Desktop/ph2_fpga_current_inverse_lesion_yolo_164_20260706
```

RKNN conversion:

```text
RK3588 already has board-side rknn-toolkit2 installed:
rknn-toolkit2 2.3.2

Board-side conversion command used:
cd /home/elf/Desktop/deploy_fpga_real_benign_20260706
python3 convert_fpga_real_benign_to_rknn.py

Conversion script settings:
target_platform = rk3588
mean_values = [[0, 0, 0]]
std_values = [[255, 255, 255]]
do_quantization = False

Converted RKNN:
/home/elf/Desktop/deploy_fpga_real_benign_20260706/yolov8n_fpga_real_benign_20260706_best.rknn

RKNN SHA256:
9bc7f9fb46d45d0e3dd22d3ba0f0af3daac0354d3e0adffd226dc9820bec23b2
```

Deployed RK3588 model directory:

```text
/home/elf/Desktop/yolo/03_rk3588_model

Files:
best.onnx
yolov8n_fpga_real_benign_20260706_best.rknn
predict_batch.py
data.yaml
names.txt
model_info.txt
README_FOR_RK3588.txt
convert_fpga_real_benign_to_rknn.py
RKNN_BOARD_CONVERSION_NOTE.md
```

`RKNN_BOARD_CONVERSION_NOTE.md` was added to the deployed model directory so
future work remembers that RKNN conversion can be done directly on RK3588.

`predict_batch.py` deployment note:

```text
The RKNN output boxes are in the original 2048 x 4096 processed-image
coordinate system. The analysis video is 2048 x 1024. The deployed
predict_batch.py now draws boxes before resizing the frame for video output:

annotated = resize_for_video(draw_predictions(image, predictions))

This keeps Benign boxes visible in yolo_rknn_annotated_stream.mp4.
```

YOLO reference dataset on RK3588:

```text
/home/elf/Desktop/yolo/02_ip_reference_yolo_dataset

train -> /home/elf/Desktop/lcoct_inverse_500_fpga_real_processed_20260706/train
valid -> /home/elf/Desktop/lcoct_inverse_500_fpga_real_processed_20260706/valid
test  -> /home/elf/Desktop/lcoct_inverse_500_fpga_real_processed_20260706/test

Counts:
train images=350 labels=350
valid images=75 labels=75
test  images=75 labels=75

Class:
0 Benign
```

Panel data-source adaptation:

```text
Qt panel dataset source now points to:
/home/elf/Desktop/lcoct_inverse_500_fpga_current_20260706/input_preview/test

FPGA input RAW frames are matched from:
/home/elf/Desktop/lcoct_inverse_500_fpga_current_20260706/raw/test

The panel code now supports input_preview paths and strips the _oct_lines suffix
when matching preview PNGs to *_2048x4096_u16le_fpga_current.raw.

Legacy symlink:
/home/elf/Desktop/yolo/01_panel_stream_raw -> /home/elf/Desktop/lcoct_inverse_500_fpga_current_20260706/raw/test
```

Panel source files changed and rebuilt:

```text
/home/elf/rk3568_capture/qt_panel/capture_worker.cpp
/home/elf/rk3568_capture/qt_panel/mainwindow.cpp

Backups:
/home/elf/rk3568_capture/qt_panel/capture_worker.cpp.bak_20260706_134956
/home/elf/rk3568_capture/qt_panel/mainwindow.cpp.bak_20260706_134956

Build:
cmake --build /home/elf/rk3568_capture/qt_panel/build -j2

Build result:
[100%] Built target oct_qt_panel
```

Verification:

```text
Offline RKNN smoke:
python3 predict_batch.py \
  --input-dir /home/elf/Desktop/yolo/02_ip_reference_yolo_dataset/test/images \
  --output-csv /tmp/fpga_real_benign_infer_smoke/out.csv \
  --output-jsonl /tmp/fpga_real_benign_infer_smoke/out.jsonl \
  --recursive --max-frames 2 --display never

Result:
engine = rknn:yolov8n_fpga_real_benign_20260706_best.rknn
records = 2
example classes = Benign
```

Panel auto capture and analysis verification:

```text
Command:
DISPLAY=:0 /home/elf/rk3568_capture/qt_panel/build/oct_qt_panel \
  --auto-dataset-test \
  --auto-test-output-dir /tmp/panel_auto_fpga_real_benign_20260706

Result:
status=analysis_done
records=75
raw_video_frames=75
processed_video_frames=75
engine=rknn:yolov8n_fpga_real_benign_20260706_best.rknn
```

Windows screenshot/report files:

```text
C:\Users\mechrevo\Desktop\yolo\fpga_real_benign_deploy_report_20260706\report\capture_01_after_capture.png
C:\Users\mechrevo\Desktop\yolo\fpga_real_benign_deploy_report_20260706\report\capture_02_after_analysis_panel.png
C:\Users\mechrevo\Desktop\yolo\fpga_real_benign_deploy_report_20260706\report\analysis_video_01.png
C:\Users\mechrevo\Desktop\yolo\fpga_real_benign_deploy_report_20260706\report\analysis_video_02.png
C:\Users\mechrevo\Desktop\yolo\fpga_real_benign_deploy_report_20260706\report\analysis_detect_fixed_01.jpg
C:\Users\mechrevo\Desktop\yolo\fpga_real_benign_deploy_report_20260706\report\analysis_detect_fixed_02.jpg
C:\Users\mechrevo\Desktop\yolo\fpga_real_benign_deploy_report_20260706\report\analysis_summary.json
```

Cleanup:

```text
Temporary RK3588 diagnosis folders under /home/elf/rk3568_capture/captures were deleted.
The captures directory is currently empty.
The ph2_fpga_* backup dataset was not touched.
```

## 28. 2026-07-07 Realtime FPGA Preview Display Trials

User clarified the correct dataflow:

```text
input_preview inverse OCT line image
-> matching raw/test 2048 x 4096 u16le frame
-> FPGA processing through PCIe/XDMA
-> fpga_processed_images cache
-> analysis video stream and RKNN recognition
```

The analysis video stream was treated as correct. The bug was the realtime
FPGA pane display path: it was displaying the full 2048 x 4096 FPGA output with
the generic Qt preview scaler, while the analysis stream displays the same FPGA
PGM cache as a 2048 x 1024 video frame. The data source was already correct;
the realtime preview representation was not matching the video stream.

RK3588 source changed:

```text
/home/elf/rk3568_capture/qt_panel/capture_worker.cpp
```

New helper:

```text
makeFpgaVideoStreamPreview(...)
```

It uses the FPGA output frame directly, applies the same 0.5%-99.5% grayscale
normalization style, and downsamples 4096 rows to the video-stream preview
height of 1024 rows for display only. It does not change the saved FPGA output:

```text
saved fpga_processed_images/*.pgm remains 2048 x 4096
metadata still reports fpga_output_uncropped=true
RKNN analysis still uses fpga_processed_images
```

Build result on RK3588:

```text
cd /home/elf/rk3568_capture/qt_panel
cmake --build build -j2

[100%] Built target oct_qt_panel
```

Verification run:

```text
DISPLAY=:0 /home/elf/rk3568_capture/qt_panel/build/oct_qt_panel \
  --auto-dataset-test \
  --auto-test-output-dir /tmp/panel_realtime_match_20260707

status=analysis_done
diagnosis_dir=/home/elf/rk3568_capture/captures/20260707_第001次诊断
analysis_exit_code=0
records=75
engine=rknn:yolov8n_fpga_real_benign_20260706_best.rknn
```

First cached FPGA frame metadata confirmed the intended source mapping:

```text
source_dataset_stripe_preview=
/home/elf/Desktop/lcoct_inverse_500_fpga_current_20260706/input_preview/test/competition_0426_ISIC_5718490_oct_lines.png

source_dataset_raw=
/home/elf/Desktop/lcoct_inverse_500_fpga_current_20260706/raw/test/competition_0426_ISIC_5718490_2048x4096_u16le_fpga_current.raw

width=2048
height=4096
fpga_output_uncropped=true
fpga_used=true
```

Report copied to Windows:

```text
C:\Users\mechrevo\Desktop\yolo\panel_realtime_match_20260707_report

capture_panel_after_capture.png
capture_panel_after_analysis.png
video_fpga_01.png
video_fpga_02.png
video_fpga_last.png
video_detect_01.png
video_detect_02.png
analysis_summary.json
first_fpga_processed_meta.txt
auto_dataset_test_result.txt
```

This first trial made the realtime FPGA pane visually closer to the analysis
video, but the user then asked to try a different display mode: preserve the
full image ratio, do not squeeze/scale the FPGA data down to video height, and
do not brighten the image.

Second/current trial applied on RK3588:

```text
/home/elf/rk3568_capture/qt_panel/capture_worker.cpp
/home/elf/rk3568_capture/qt_panel/mainwindow.cpp
```

Current realtime display behavior:

```text
capture_worker.cpp:
  makeFpgaLinearFullPreview(...)
  builds a full 2048 x 4096 grayscale preview from the FPGA output.
  It does not use percentile/histogram stretching.
  Because the measured FPGA output is mostly within ~10 bits, preview pixels
  use a fixed linear 10-bit-to-8-bit mapping:

    display_u8 = min(255, fpga_u16 >> 2)

mainwindow.cpp:
  QLabel drawing now uses Qt::KeepAspectRatio and centers the image on a light
  background. It no longer uses IgnoreAspectRatio to force-fill the pane.
```

Important: this is display-only. Saved FPGA output and analysis input remain:

```text
fpga_processed_images/*.pgm: 2048 x 4096
metadata: fpga_output_uncropped=true
analysis/RKNN input: fpga_processed_images cache
```

Second trial verification:

```text
DISPLAY=:0 /home/elf/rk3568_capture/qt_panel/build/oct_qt_panel \
  --auto-dataset-test \
  --auto-test-output-dir /tmp/panel_full_linear_10bit_preview_20260707

status=analysis_done
analysis_exit_code=0
```

Measured value range for the last FPGA PGM in that test:

```text
shape=2048 x 4096
min=0
max=652
p50=376
p99.5=565
```

Report copied to Windows:

```text
C:\Users\mechrevo\Desktop\yolo\panel_full_linear_10bit_preview_20260707_report

capture_panel_after_capture.png
capture_panel_after_analysis.png
video_fpga_last.png
analysis_summary.json
first_fpga_processed_meta.txt
fpga_value_stats.txt
auto_dataset_test_result.txt
```

Cleanup after the second trial:

```text
/home/elf/rk3568_capture/captures is empty after cleanup.
```

Third/current trial applied after user asked to keep the full-ratio display but
add the same normalization/brightening style as the video stream:

```text
/home/elf/rk3568_capture/qt_panel/capture_worker.cpp

processed_preview = makeGrayscalePreview(
    processed_frame, 2048, 4096, 4096,
    2, 65535, 2048, 4096);
```

Current combined behavior:

```text
capture_worker.cpp:
  realtime FPGA pane uses full 2048 x 4096 preview with 0.5%-99.5%
  grayscale normalization.

mainwindow.cpp:
  QLabel still uses Qt::KeepAspectRatio and centers the full-ratio image.

analysis video:
  unchanged; still generated from fpga_processed_images as 2048 x 1024 video.
```

Verification:

```text
DISPLAY=:0 /home/elf/rk3568_capture/qt_panel/build/oct_qt_panel \
  --auto-dataset-test \
  --auto-test-output-dir /tmp/panel_full_aspect_norm_preview_20260707

status=analysis_done
analysis_exit_code=0
```

Report copied to Windows:

```text
C:\Users\mechrevo\Desktop\yolo\panel_full_aspect_norm_preview_20260707_report

capture_panel_after_capture.png
capture_panel_after_analysis.png
video_fpga_01.png
video_fpga_last.png
fpga_processed_stream.mp4
analysis_summary.json
first_fpga_processed_meta.txt
fpga_value_stats.txt
auto_dataset_test_result.txt
```

Cleanup after the third trial:

```text
/home/elf/rk3568_capture/captures is empty.
/tmp/panel_full_aspect_norm_preview_20260707 was deleted.
```

Current analysis script on RK3588 was left in the user-confirmed working video
configuration:

```text
/home/elf/Desktop/yolo/03_rk3588_model/predict_batch.py

PROCESSED_HEIGHT = 1024
VIDEO_WIDTH = 2048
VIDEO_HEIGHT = 1024
```

## 29. 2026-07-07 Final Realtime/Analysis FPGA Image Unification

User clarified that this was not only a display-style problem: the right-side
realtime FPGA pane and the YOLO recognition video must come from the same FPGA
processed image content. The active code was updated so both paths use the same
display transform over the FPGA output:

```text
source: FPGA returned processed frame / fpga_processed_images/*.pgm
stored cache: full 2048 x 4096, uncropped
display/video frame: normalize full FPGA frame with 0.5%-99.5% percentile range
display/video size: 2048 x 1024
display/video polarity: inverted dark-background style
```

Files changed on RK3588:

```text
/home/elf/rk3568_capture/qt_panel/capture_worker.cpp
  realtime FPGA pane now uses:
  invertGrayscaleImage(makeFpgaVideoStreamPreview(..., 2048 x 4096 -> 2048 x 1024))

/home/elf/Desktop/yolo/03_rk3588_model/predict_batch.py
  fpga_processed_stream.mp4 and yolo_rknn_annotated_stream.mp4 now load
  fpga_processed_images with the same normalized inverted display transform.
```

Build/verification:

```text
cd /home/elf/rk3568_capture/qt_panel
cmake --build build -j2
python3 -m py_compile /home/elf/Desktop/yolo/03_rk3588_model/predict_batch.py

DISPLAY=:0 /home/elf/rk3568_capture/qt_panel/build/oct_qt_panel \
  --auto-dataset-test \
  --auto-test-output-dir /tmp/panel_unified_fpga_preview_20260707

status=analysis_done
analysis_exit_code=0
```

Windows verification report:

```text
C:\Users\mechrevo\Desktop\yolo\panel_unified_fpga_preview_20260707_report

capture_panel_after_capture.png
capture_panel_after_analysis.png
analysis_videos\fpga_processed_stream.mp4
analysis_videos\yolo_rknn_annotated_stream.mp4
extracted_frames\fpga_processed_stream_frame_075.png
extracted_frames\yolo_rknn_annotated_stream_frame_075.png
video_frame_compare.json
```

Frame comparison result:

```text
frame 75:
  fpga_processed_stream vs yolo_rknn_annotated_stream full-frame MSE = 0
  correlation = 1.0

frame 4:
  full-frame differs only where YOLO boxes/text are drawn
  areas away from boxes/text: MSE = 0, correlation = 1.0
```

Cleanup:

```text
/home/elf/rk3568_capture/captures was cleared after the test.
Final size: 4.0K
```

## 30. 2026-07-07 Restore FPGA Processed Effect To Training-Package Version

User later confirmed that the Section 29 inverted/dark-background unification
was not the desired FPGA processed effect. The correct reference is the dataset
previously sent for retraining:

```text
Windows:
C:\Users\mechrevo\Desktop\yolo\lcoct_inverse_500_fpga_real_processed_20260706

RK3588:
/home/elf/Desktop/lcoct_inverse_500_fpga_real_processed_20260706
```

This section supersedes Section 29 for the active RK3588 panel and analysis
pipeline.

Actions completed:

```text
1. Re-downloaded FPGA over JTAG from the USB reference package:
   E:\fpga_tuxiang_pcie\bitstream\xilinx_dma_pcie_ep_20260706_package.bit

2. Confirmed this bitstream is identical to the local project bitstream:
   D:\FPGA20251016\tuxiang_pcie\prj\tuxiang_pcie.runs\impl_1\xilinx_dma_pcie_ep.bit

   SHA256:
   A711A3040D89CD1AF375CB8E140C7FBFCDD5DD39BC8CD5026339767C78354FED

3. Restored RK3588 panel source so realtime FPGA preview uses the same
   non-inverted full 2048 x 4096 display scaling as the retraining package.

4. Restored YOLO analysis script so FPGA processed images are not inverted
   before video/analysis display.
```

Important PROC_CTRL clarification:

```text
The old retraining manifest says proc_ctrl=0x000000a8, but that value was
recorded from BAR offset 0x38 DEBUG_FLAGS by the old batch script, not from the
actual PROC_CTRL register.

Current HDL register map:
0x38 DEBUG_FLAGS
0x3C PROC_CTRL

The correct active PROC_CTRL for the training-package effect is:
PROC_CTRL=0x0000100a

Meaning:
norm_shift=10
out_shift=0
log_gain_q4_4=16
log_offset=0
```

Files restored on RK3588:

```text
/home/elf/rk3568_capture/qt_panel/capture_worker.cpp
/home/elf/rk3568_capture/qt_panel/mainwindow.cpp
/home/elf/Desktop/yolo/03_rk3588_model/predict_batch.py
```

Build/compile verification:

```text
cd /home/elf/rk3568_capture/qt_panel
cmake --build build -j2

python3 -m py_compile /home/elf/Desktop/yolo/03_rk3588_model/predict_batch.py

Result:
[100%] Built target oct_qt_panel
PY_OK
```

File-level FPGA verification against the original retraining package:

```text
Raw input:
/home/elf/Desktop/lcoct_inverse_500_fpga_current_20260706/raw/test/
competition_0430_ISIC_2669185_2048x4096_u16le_fpga_current.raw

Reference processed PNG:
/home/elf/Desktop/lcoct_inverse_500_fpga_real_processed_20260706/test/images/
competition_0430_ISIC_2669185.png

PROC_CTRL=0x0000100a result:
FRAME_CFG=0x10000800
VERSION=0x20260619
H2C_LINES=0x00001000
C2H_FRAMES=0x00000001
PH_EVENTS=0x00000000
DEBUG_FLAGS=0x000000a8
PROC_CTRL=0x0000100a

After applying the same 0.5/99.5 percentile 16-bit-to-8-bit scaling used by
the retraining package:
equal_ratio=1.0
MAE=0.0
max_abs=0
candidate SHA256 == reference SHA256
```

Panel-level verification:

```text
Auto panel test produced 75 FPGA processed PGM cache frames.
The cached competition_0430 FPGA PGM, scaled like the retraining package,
matched the original retraining PNG exactly:

equal_ratio=1.0
MAE=0.0
max_abs=0
stats:
  min_percentile_value=0
  max_percentile_value=48
  raw_min=0
  raw_max=48
```

Windows verification artifacts:

```text
C:\Users\mechrevo\Desktop\yolo\fpga_restore_verify_20260707

capture_panel_after_capture.png
panel_competition_0430_scaled_like_training.png
direct_fpga_competition_0430_proc_0000100a.png
summary.json
```

Cleanup:

```text
/home/elf/rk3568_capture/captures was cleared after verification.
Final size: 4.0K

Temporary RK3588 verification directories were removed:
/tmp/panel_restore_verify_20260707
/tmp/fpga_restore_compare_20260707
```

## 31. 2026-07-07 Realtime FPGA Pane Now Uses Cached PGM Source

User observed that the right-side realtime FPGA pane and
`analysis/fpga_processed_stream.mp4` could still look different, and that the
raw `fpga_processed_images/*.pgm` files appear almost black when opened
directly. The PGM files are 16-bit FPGA output with low numeric range, so a
normal image viewer can show them as black even though the data is valid.

To force both display paths to use the same source, the RK3588 panel was
changed so the right-side realtime FPGA pane no longer displays the in-memory
`processed_frame` directly.

Current dataset-mode realtime path:

```text
input_preview/test image
  -> matching raw/test *_2048x4096_u16le_fpga_current.raw
  -> XDMA H2C to FPGA
  -> XDMA C2H processed_frame, 2048 x 4096 x uint16
  -> save processed_frame as diagnosis/fpga_processed_images/*.pgm
  -> read that same PGM back
  -> apply 0.5/99.5 percentile normalization
  -> resize to 2048 x 1024 realtime preview
  -> emit right-side FPGA frame
```

Current analysis-video path:

```text
diagnosis/fpga_processed_images/*.pgm
  -> predict_batch.py load_display_image()
  -> 0.5/99.5 percentile normalization
  -> resize_for_video()
  -> analysis/fpga_processed_stream.mp4
```

Files changed and rebuilt:

```text
/home/elf/rk3568_capture/qt_panel/capture_worker.cpp

Build:
cd /home/elf/rk3568_capture/qt_panel
cmake --build build -j2

Result:
[100%] Built target oct_qt_panel
```

Implementation detail:

```text
New helper path in capture_worker.cpp:
readPgmU16LeFile()
makeFpgaVideoStreamPreviewFromPgm()

For cached analysis frames, the realtime preview reads the exact
fpga_processed_images/*.pgm file that analysis later uses. After the first
100 cached frames, the panel uses a rolling __live_fpga_preview.pgm in the
same folder for realtime display so it does not keep consuming storage.
```

Verification:

```text
Auto capture produced:
processed_pgm_count=75
processed_meta_count=75

Analysis produced:
analysis/fpga_processed_stream.mp4
analysis/yolo_rknn_annotated_stream.mp4

RK3588 captures were cleared after the test:
/home/elf/rk3568_capture/captures = 4.0K
```

Windows verification artifacts:

```text
C:\Users\mechrevo\Desktop\yolo\pgm_based_realtime_verify_20260707

capture_panel_after_capture.png
fpga_processed_stream_first_frame.png
summary.json
```

## 32. 2026-07-07 Realtime Pane Ratio And Resize Behavior

User requested the left realtime raw pane to use the same display ratio as the
right realtime FPGA pane. This was changed on RK3588.

Current behavior:

```text
Left realtime raw pane:
input_preview/test image
  -> converted through makeFpgaVideoStreamPreview(...)
  -> 2048 x 1024 preview, same display ratio as right FPGA pane

Right realtime FPGA pane:
fpga_processed_images/*.pgm
  -> makeFpgaVideoStreamPreviewFromPgm(...)
  -> 2048 x 1024 preview
```

The MainWindow display layer now caches the last raw/processed `QImage` and
redraws both labels on `resizeEvent()`. This fixes the bug where fullscreen or
window resizing enlarged the outer panel frame but left the already-rendered
image pixmap at the old size.

Files changed and rebuilt on RK3588:

```text
/home/elf/rk3568_capture/qt_panel/capture_worker.cpp
/home/elf/rk3568_capture/qt_panel/mainwindow.cpp
/home/elf/rk3568_capture/qt_panel/mainwindow.h

Build:
cd /home/elf/rk3568_capture/qt_panel
cmake --build build -j2

Result:
[100%] Built target oct_qt_panel
```

Important correction:

```text
Default fullscreen launch was tested but then reverted at user request because
the panel became inconvenient to shrink. The active build does NOT call
showFullScreen() on startup.
```

Post-change checks:

```text
Remote grep confirmed no showFullScreen() remains in active mainwindow.cpp.
Realtime QImage cache + resizeEvent redraw remain active.
RK3588 test captures were cleared:
/home/elf/rk3568_capture/captures = 4.0K
```

## 33. 2026-07-07 ISIC2016 RAW Dataset FPGA Processing Package

User transferred a new dataset folder to the RK3588 desktop:

```text
/home/elf/Desktop/isic2016_bm_lcoct_inverse_500_20260707
```

The dataset transfer was treated as complete after `raw` stabilized at 500
files:

```text
raw/train = 350
raw/valid = 75
raw/test  = 75
```

All RAW files under the dataset's `raw` folder were processed through the
active ShengTeng Pro FPGA pipeline with an independent XDMA batch script:

```text
/tmp/batch_fpga_process_isic2016_20260707.py
```

Output dataset on RK3588:

```text
/home/elf/Desktop/isic2016_bm_lcoct_inverse_500_20260707_fpga_processed
```

The script removes the RAW suffix
`_2048x4096_u16le_low12.raw` and writes the corresponding same-stem `.png`.
Labels are copied byte-for-byte from the source dataset, so image/label names
remain aligned for training.

Verification result:

```text
manifest_records = 500
fpga_version = 0x20260619
frame_cfg = 0x10000800
proc_ctrl = 0x000000a8

train images=350 labels=350
valid images=75  labels=75
test  images=75  labels=75

ph_events_nonzero_records = 0
h2c_lines_bad_records = 0
c2h_frames_bad_records = 0
ok = true
```

Verification file in the package:

```text
metadata/file_count_check.json
metadata/batch_run.log
metadata/batch_run.rc
fpga_processing_manifest.json
```

Package copied to Windows:

```text
C:\Users\mechrevo\Desktop\yolo\isic2016_bm_lcoct_inverse_500_20260707_fpga_processed.tar.gz
C:\Users\mechrevo\Desktop\yolo\isic2016_bm_lcoct_inverse_500_20260707_fpga_processed.tar.gz.sha256
```

SHA256:

```text
cbd3d6812024f17b7e5860b34feae656d52fb16d601eb73bf895997e882cda79
```

The Windows-side hash matched the RK3588-generated `.sha256` file after copy.
Temporary smoke-test output and RK capture diagnostics were removed:

```text
/home/elf/Desktop/isic2016_bm_lcoct_inverse_500_20260707_fpga_processed_smoke removed
/home/elf/rk3568_capture/captures = 4.0K
```

## 34. 2026-07-07 ISIC2016 Two-Stage Cascade RKNN Deployment

User transferred the new two-stage model package to RK3588:

```text
/home/elf/Desktop/isic2016_bm_fpga_hybrid_tightbox_highconf_20260707
```

The visual prediction/effect reference folder is:

```text
/home/elf/Desktop/hybrid_conf960_box_plus_high_conf_classifier_large_label_3dec
```

This model corresponds to the dataset:

```text
/home/elf/Desktop/isic2016_bm_lcoct_inverse_500_20260707
```

Do not confuse this with older `lcoct_inverse_500_fpga_current_20260706`
or `lcoct_inverse_500_fpga_real_processed_20260706` folders.

Cascade logic:

```text
FPGA processed image, 2048 x 4096
  -> Stage 1 RKNN: stage1_yolov8n_conf960_box_model.rknn
     YOLOv8n, input 960 x 960, used for tight lesion box localization
  -> crop highest-confidence Stage 1 box with 18% margin
  -> square pad and resize crop to 320 x 320
  -> Stage 2 RKNN: stage2_bm_classifier_320.rknn
     classifier output: Benign / Malignant
  -> draw Stage 2 class and confidence on the Stage 1 box
```

Board-side RKNN conversion was done directly on RK3588 with toolkit 2.3.2:

```text
python3 /tmp/convert_isic2016_hybrid_to_rknn_20260707.py
```

The conversion script uses:

```text
from rknn.api import RKNN
rknn.config(mean_values=[[0, 0, 0]],
            std_values=[[255, 255, 255]],
            target_platform="rk3588")
rknn.load_onnx(...)
rknn.build(do_quantization=False)
rknn.export_rknn(...)
```

RKNN output files:

```text
/home/elf/Desktop/isic2016_bm_fpga_hybrid_tightbox_highconf_20260707/models/stage1_yolov8n_conf960_box_model.rknn
/home/elf/Desktop/isic2016_bm_fpga_hybrid_tightbox_highconf_20260707/models/stage2_bm_classifier_320.rknn
```

Active deployment directory:

```text
/home/elf/Desktop/yolo/03_rk3588_model
```

Active deployed files:

```text
predict_batch.py
stage1_yolov8n_conf960_box_model.rknn
stage2_bm_classifier_320.rknn
names.txt
data.yaml
convert_isic2016_hybrid_to_rknn_20260707.py
RKNN_CASCADE_CONVERSION_NOTE_20260707.md
```

Class names:

```text
Benign
Malignant
```

The old single-model files were not deleted. `predict_batch.py` was backed up
under:

```text
/home/elf/Desktop/yolo/03_rk3588_model/_codex_backups
```

Qt panel dataset-mode source was changed to the new dataset raw test split:

```text
/home/elf/Desktop/isic2016_bm_lcoct_inverse_500_20260707/raw/test
```

The panel's dataset mode now accepts `.raw` files directly. For a `.raw` source
frame, the same raw frame is used for both the left realtime preview and FPGA
H2C input, so the left preview and FPGA processing input stay aligned.

Qt files changed and rebuilt on RK3588:

```text
/home/elf/rk3568_capture/qt_panel/capture_worker.cpp
/home/elf/rk3568_capture/qt_panel/mainwindow.cpp

cd /home/elf/rk3568_capture/qt_panel
cmake --build build -j2

Result:
[100%] Built target oct_qt_panel
```

Important runtime note:

```text
If /usr/local/sbin/ensure_fpga_pcie_ready.sh reports permission denied for
/tmp/fpga_pcie_ready.log, remove the stale log once with:

echo elf | sudo -S rm -f /tmp/fpga_pcie_ready.log
```

Verification performed after deployment:

```text
python3 -m py_compile /home/elf/Desktop/yolo/03_rk3588_model/predict_batch.py
```

Offline five-frame RKNN smoke, using already FPGA-processed ISIC2016 images:

```text
input_dir = /tmp/isic2016_hybrid_smoke
records = 5
engine = cascade_rknn:stage1_yolov8n_conf960_box_model.rknn+stage2_bm_classifier_320.rknn
outputs:
  raw_realtime_stream.mp4
  fpga_processed_stream.mp4
  yolo_rknn_annotated_stream.mp4
  out.csv
  out.jsonl
```

One-frame full chain check:

```text
source raw:
/home/elf/Desktop/isic2016_bm_lcoct_inverse_500_20260707/raw/train/0001_train_benign_0001_ISIC_0002489_2048x4096_u16le_low12.raw

FPGA batch output:
/tmp/isic2016_fpga_one/train/images/0001_train_benign_0001_ISIC_0002489.png

XDMA result:
xdma = 133.0 ms

Cascade RKNN result:
class = Benign
score = 0.8247444033622742
records = 1
analysis video = /tmp/isic2016_fpga_one_analysis/yolo_rknn_annotated_stream.mp4
```

RKNN runtime may print `Query dynamic range failed` for these models because
they are static-shape RKNN models. The RKNNLite warning itself is expected and
was present during successful inference.

## 35. 2026-07-07 Reverted Failed ISIC2016 Cascade Model

The ISIC2016 two-stage cascade model was rejected after user testing because
the recognition effect was poor. The RK3588 deployment was reverted to the
previous single-model backup.

Active model after revert:

```text
/home/elf/Desktop/yolo/03_rk3588_model/yolov8n_fpga_real_benign_20260706_best.rknn
```

Active analysis entry:

```text
/home/elf/Desktop/yolo/03_rk3588_model/predict_batch.py
```

The active `predict_batch.py` was restored from:

```text
/home/elf/Desktop/yolo/03_rk3588_model/_codex_backups/predict_batch_20260707_150058.py
```

Active model metadata after revert:

```text
nc: 1
names: ['Benign']
```

Qt panel source was also reverted from the ISIC2016 raw/test folder back to
the previous dataset path:

```text
/home/elf/Desktop/lcoct_inverse_500_fpga_current_20260706/input_preview/test
```

Qt source files restored from backup and rebuilt:

```text
/home/elf/rk3568_capture/qt_panel/_codex_backups/capture_worker_20260707_150058.cpp
/home/elf/rk3568_capture/qt_panel/_codex_backups/mainwindow_20260707_150058.cpp

cd /home/elf/rk3568_capture/qt_panel
cmake --build build -j2

Result:
[100%] Built target oct_qt_panel
```

Removed RK3588 cascade deployment artifacts:

```text
/home/elf/Desktop/yolo/03_rk3588_model/stage1_yolov8n_conf960_box_model.rknn
/home/elf/Desktop/yolo/03_rk3588_model/stage2_bm_classifier_320.rknn
/home/elf/Desktop/yolo/03_rk3588_model/convert_isic2016_hybrid_to_rknn_20260707.py
/home/elf/Desktop/yolo/03_rk3588_model/RKNN_CASCADE_CONVERSION_NOTE_20260707.md
/home/elf/Desktop/isic2016_bm_fpga_hybrid_tightbox_highconf_20260707
/home/elf/Desktop/hybrid_conf960_box_plus_high_conf_classifier_large_label_3dec
/home/elf/Desktop/isic2016_bm_lcoct_inverse_500_20260707
/home/elf/Desktop/isic2016_bm_lcoct_inverse_500_20260707_fpga_processed
```

The Windows-side historical package under `C:\Users\mechrevo\Desktop\yolo`
was not deleted.

Post-revert smoke test:

```text
input_dir = /tmp/old_model_restore_smoke
records = 3
engine = rknn:yolov8n_fpga_real_benign_20260706_best.rknn
annotated video = /tmp/old_model_restore_smoke_out/yolo_rknn_annotated_stream.mp4
```

Temporary smoke-test directories were removed after verification. RK3588 free
space after deleting the failed cascade dataset/model files:

```text
/dev/root 56G total, 38G used, 17G free, 70% used
```

## 36. 2026-07-07 RK3588 Panel High-Resolution Realtime Preview

User noticed that FPGA-processed training images and YOLO prediction effect
images are clear on Windows, but the two realtime panes in the RK3588 panel
look blurry. This was traced to the RK3588 panel display path, not to FPGA
processing or the model.

Root cause:

```text
Full FPGA/source image: 2048 x 4096
Old realtime preview path:
  2048 x 4096
  -> makeFpgaVideoStreamPreview()
  -> average/downsample 4 vertical rows into 1 row
  -> 2048 x 1024 QImage
  -> QLabel scales again to the visible pane
```

This loses 75% of vertical display resolution before the Qt label does its
final scaling.

Local diagnostic used one Windows-side clear FPGA processed image:

```text
C:\Users\mechrevo\Desktop\yolo\lcoct_inverse_500_fpga_real_processed_20260706\test\images\competition_0426_ISIC_5718490.png
```

Measured result:

```text
source_shape = 4096 x 2048
old_current_preview_shape = 1024 x 2048
vertical_resolution_loss = 75%
sharpness_source_laplacian_var = 255194.326
sharpness_old_preview_upscaled = 51831.691
sharpness_simulated_pane_upscaled = 8218.248
```

Fix applied on RK3588:

```text
/home/elf/rk3568_capture/qt_panel/capture_worker.cpp
```

Changes:

```text
constexpr size_t kDatasetRealtimePreviewHeight = kDatasetRawHeight;

Left realtime raw pane:
  use the original input_preview QImage at 2048 x 4096;
  do not rebuild it as a 2048 x 1024 averaged preview.

Right realtime FPGA pane:
  still reads from the same cached fpga_processed_images/*.pgm source used
  by analysis, but now builds a 2048 x 4096 QImage for realtime display.

Qt QLabel is now the only scaling stage for dataset-mode realtime panes.
```

The analysis cache/model path was not changed. The FPGA bitstream/algorithm
was not changed.

Backup before edit:

```text
/home/elf/rk3568_capture/qt_panel/_codex_backups/capture_worker_before_hires_preview_20260707_155515.cpp
```

Build:

```text
cd /home/elf/rk3568_capture/qt_panel
cmake --build build -j2

Result:
[100%] Built target oct_qt_panel
```

The already-running panel process was restarted so the new binary is active:

```text
old oct_qt_panel killed
new oct_qt_panel pid = 33190
```

## 37. 2026-07-07 High-Resolution Analysis Video Stream

After the realtime-pane fix, user asked whether the analysis video stream had
the same downsampling problem. It did.

Root cause in active RK3588 model script:

```text
/home/elf/Desktop/yolo/03_rk3588_model/predict_batch.py

RAW_WIDTH = 2048
RAW_HEIGHT = 4096
VIDEO_WIDTH = 2048
old VIDEO_HEIGHT = 1024
```

The `resize_for_video()` path forced `raw_realtime_stream.mp4`,
`fpga_processed_stream.mp4`, and `yolo_rknn_annotated_stream.mp4` to
2048 x 1024, so the analysis videos also lost 75% of vertical resolution.

Fix applied:

```text
VIDEO_HEIGHT = RAW_HEIGHT
```

Also changed RAW loading so full 2048 x 4096 raw frames are not averaged down
to 1024 lines for video display, and scaled YOLO box/text thickness with image
size so annotations remain visible on full-resolution 4096-line frames.

Backup before edit:

```text
/home/elf/Desktop/yolo/03_rk3588_model/_codex_backups/predict_batch_before_hires_video_20260707_155906.py
```

Verification:

```text
python3 -m py_compile /home/elf/Desktop/yolo/03_rk3588_model/predict_batch.py
```

Two-frame smoke output:

```text
raw_realtime_stream.mp4:            2048 x 4096, 2 frames
fpga_processed_stream.mp4:          2048 x 4096, 2 frames
yolo_rknn_annotated_stream.mp4:     2048 x 4096, 2 frames
```

Panel auto capture plus analysis verification:

```text
diagnosis cached PGM count = 75
analysis max frames = 30
engine = rknn:yolov8n_fpga_real_benign_20260706_best.rknn

fpga_processed_stream.mp4:
  width = 2048
  height = 4096
  frames = 30

yolo_rknn_annotated_stream.mp4:
  width = 2048
  height = 4096
  frames = 30
```

Windows screenshots/frames copied for user inspection:

```text
C:\Users\mechrevo\Desktop\yolo\hires_realtime_video_verify_20260707\capture_panel_after_capture.png
C:\Users\mechrevo\Desktop\yolo\hires_realtime_video_verify_20260707\analysis_fpga_processed_stream_frame_001.png
C:\Users\mechrevo\Desktop\yolo\hires_realtime_video_verify_20260707\analysis_yolo_rknn_annotated_stream_frame_001.png
C:\Users\mechrevo\Desktop\yolo\hires_realtime_video_verify_20260707\summary.json
```

Temporary RK3588 test directories were removed after copying the screenshots.
The capture directory was cleaned:

```text
/home/elf/rk3568_capture/captures = 4.0K
```

The RK3588 panel was restarted after cleanup:

```text
new oct_qt_panel pid = 34522
```

## 38. 2026-07-08 Realtime Pane Matches Video Processing And Rotates For Display

User pointed out that the right realtime FPGA pane must come from the same
`fpga_processed_images/*.pgm` source as the analysis video stream, and must be
processed the same way before display.

Issue found:

```text
Right realtime pane:
  read fpga_processed_images/*.pgm
  C++ histogram normalization and display

Analysis video:
  predict_batch.py load_display_image()
  normalize_u8(): np.percentile([0.5, 99.5])
  clip to uint8
```

The source folder was already aligned after the earlier PGM-cache change, but
the display normalization was still implemented separately in C++ and could
look different from `fpga_processed_stream.mp4`.

Fix applied in:

```text
/home/elf/rk3568_capture/qt_panel/capture_worker.cpp
```

New realtime right-pane path:

```text
fpga_processed_images/*.pgm
  -> read PGM
  -> 0.5/99.5 percentile display normalization matching predict_batch.py
  -> 2048 x 4096 QImage
  -> emit to UI
```

User also noted that the full-resolution 2048 x 4096 image is too vertical for
the current two-column panel, leaving too much empty space. Display-only
rotation was added in:

```text
/home/elf/rk3568_capture/qt_panel/mainwindow.cpp
```

Realtime display behavior after this change:

```text
Left realtime raw pane:
  full-resolution QImage
  -> UI display rotates 90 degrees
  -> QLabel scales once with KeepAspectRatio

Right realtime FPGA pane:
  same PGM source and same normalization as video stream
  -> UI display rotates 90 degrees
  -> QLabel scales once with KeepAspectRatio
```

Important: this is display-only rotation. It does not rotate, crop, compress,
or alter:

```text
fpga_processed_images/*.pgm
fpga_processed_stream.mp4
yolo_rknn_annotated_stream.mp4
YOLO/RKNN model input
FPGA bitstream/algorithm
```

Backups before edit:

```text
/home/elf/rk3568_capture/qt_panel/_codex_backups/capture_worker_before_same_video_rotate_20260707_165148.cpp
/home/elf/rk3568_capture/qt_panel/_codex_backups/mainwindow_before_same_video_rotate_20260707_165148.cpp
```

Build:

```text
cd /home/elf/rk3568_capture/qt_panel
cmake --build build -j2

Result:
[100%] Built target oct_qt_panel
```

Verification:

```text
Auto capture cached PGM count = 75
Short analysis max frames = 2
fpga_processed_stream.mp4      = 2048 x 4096, 2 frames
yolo_rknn_annotated_stream.mp4 = 2048 x 4096, 2 frames
```

Windows verification artifacts:

```text
C:\Users\mechrevo\Desktop\yolo\rotated_realtime_same_video_verify_20260708\capture_panel_after_capture.png
C:\Users\mechrevo\Desktop\yolo\rotated_realtime_same_video_verify_20260708\analysis_fpga_processed_stream_frame_001.png
C:\Users\mechrevo\Desktop\yolo\rotated_realtime_same_video_verify_20260708\analysis_yolo_rknn_annotated_stream_frame_001.png
C:\Users\mechrevo\Desktop\yolo\rotated_realtime_same_video_verify_20260708\summary.json
```

Temporary RK3588 test directories were removed. Capture directory after cleanup:

```text
/home/elf/rk3568_capture/captures = 4.0K
```

The panel was restarted after cleanup:

```text
new oct_qt_panel pid = 35857
```

## 39. 2026-07-08 Realtime Overview And Detail Display Mode

User noted that even after using the correct PGM source and video-compatible
normalization, the right realtime FPGA pane still looked unclear when the full
4096 x 2048 rotated image was fitted into a small two-column pane.

Measured cause:

```text
Right live pane visible content in saved screenshot: about 490 x 300 pixels
Rotated full source size: 4096 x 2048
Compression on screen:
  x: about 8.36 source pixels per screen pixel
  y: about 6.83 source pixels per screen pixel
```

This is a display-size limitation: showing the entire full-resolution image in
a small QLabel necessarily removes fine detail. Directly opening the image
looks clear because the image viewer can zoom.

Fix applied in:

```text
/home/elf/rk3568_capture/qt_panel/mainwindow.cpp
/home/elf/rk3568_capture/qt_panel/mainwindow.h
```

Behavior after this change:

```text
Left realtime raw pane:
  rotated 90 degrees for the two-column layout
  overview mode only

Right realtime FPGA pane:
  default overview mode:
    full rotated image, SmoothTransformation scaling, shows structure
  click right pane:
    toggles high-detail mode
    displays a center ROI from the rotated image instead of the full image
    much less downsampling, suitable for checking local detail
  click again:
    returns to overview mode
```

The right pane has a tooltip:

```text
点击切换总览/高清细节
```

An environment variable was added only for automated verification:

```text
OCT_PANEL_PROCESSED_DETAIL_MODE=1
```

This starts the panel with right-pane detail mode enabled. Normal launches
default to overview mode.

Important: this is display-only. It does not alter:

```text
fpga_processed_images/*.pgm
fpga_processed_stream.mp4
yolo_rknn_annotated_stream.mp4
YOLO/RKNN model input
FPGA bitstream/algorithm
```

Backups before edit:

```text
/home/elf/rk3568_capture/qt_panel/_codex_backups/mainwindow_before_detail_mode_20260707_173602.cpp
/home/elf/rk3568_capture/qt_panel/_codex_backups/mainwindow_h_before_detail_mode_20260707_173602.h
```

Build:

```text
cd /home/elf/rk3568_capture/qt_panel
cmake --build build -j2

Result:
[100%] Built target oct_qt_panel
```

Verification screenshots copied to Windows:

```text
C:\Users\mechrevo\Desktop\yolo\realtime_detail_mode_verify_20260708\overview_capture_panel_after_capture.png
C:\Users\mechrevo\Desktop\yolo\realtime_detail_mode_verify_20260708\detail_capture_panel_after_capture.png
C:\Users\mechrevo\Desktop\yolo\realtime_detail_mode_verify_20260708\summary.json
```

After verification, RK3588 test captures were deleted:

```text
/home/elf/rk3568_capture/captures = 4.0K
```

The panel was restarted in normal overview mode:

```text
new oct_qt_panel pid = 37275
```

## 40. 2026-07-08 Video Stream Orientation/FPS Fix And Detail Mode Removal

This section supersedes the click-to-detail behavior recorded in section 39.
The user confirmed the two realtime panes were already acceptable, so the
right-pane click/ROI high-detail mode was removed. Current realtime behavior is:

```text
Left realtime raw pane:
  read from the existing raw preview path
  rotate 90 degrees at QLabel display layer
  keep full image, SmoothTransformation scaling

Right realtime FPGA pane:
  still read from diagnosis fpga_processed_images/*.pgm cache
  same normalization path as analysis/video preview
  rotate 90 degrees at QLabel display layer
  keep full image, SmoothTransformation scaling

No click-to-zoom/detail mode remains.
No OCT_PANEL_PROCESSED_DETAIL_MODE env switch remains.
```

The video blur/orientation issue was fixed in:

```text
/home/elf/Desktop/yolo/03_rk3588_model/predict_batch.py
/home/elf/rk3568_capture/qt_panel/mainwindow.cpp
/home/elf/rk3568_capture/qt_panel/mainwindow.h
```

Important implementation details:

```text
predict_batch.py:
  model inference input remains the original FPGA processed image
  display/video output is rotated 90 degrees clockwise to match realtime panes
  video dimensions changed from 2048 x 4096 to 4096 x 2048
  writer now prefers MJPG AVI, then XVID AVI, then mp4v MP4 fallback
  stale .avi/.mp4 with the same stem is unlinked before writing
  YOLO boxes are transformed and drawn after rotation, so text stays horizontal
  VIDEO_FPS remains 10.0

mainwindow.cpp:
  findAnalysisVideo() now prefers .avi before .mp4
  mpv launch includes --fps=10 and --speed=1.0
  eventFilter/mouse click detail mode and tooltip were removed
```

Reason for blur:

```text
The old analysis stream was being written as vertical 2048 x 4096 mp4v first.
For high-texture OCT/FPGA processed images, the default MP4 path visibly smeared
fine detail. The new path writes horizontal 4096 x 2048 MJPG AVI first, avoiding
the unnecessary display-orientation resize and reducing compression smearing.
```

Build and syntax check on RK3588:

```text
python3 -m py_compile /home/elf/Desktop/yolo/03_rk3588_model/predict_batch.py
cd /home/elf/rk3568_capture/qt_panel
cmake --build build -j2

Result:
[100%] Built target oct_qt_panel
```

Short verification on RK3588:

```text
script: /tmp/verify_video_no_detail_20260708.py
captured FPGA processed PGM frames: 75
analysis max frames: 30

raw_realtime_stream.avi:
  4096 x 2048, MJPG, 30 frames, 10.0 fps, 3.0 s

fpga_processed_stream.avi:
  4096 x 2048, MJPG, 30 frames, 10.0 fps, 3.0 s

yolo_rknn_annotated_stream.avi:
  4096 x 2048, MJPG, 30 frames, 10.0 fps, 3.0 s
```

Verification artifacts copied to Windows:

```text
C:\Users\mechrevo\Desktop\yolo\video_no_detail_verify_20260708\capture_panel_after_capture.png
C:\Users\mechrevo\Desktop\yolo\video_no_detail_verify_20260708\fpga_processed_stream_frame_001.png
C:\Users\mechrevo\Desktop\yolo\video_no_detail_verify_20260708\fpga_processed_stream_frame_016.png
C:\Users\mechrevo\Desktop\yolo\video_no_detail_verify_20260708\yolo_rknn_annotated_stream_frame_001.png
C:\Users\mechrevo\Desktop\yolo\video_no_detail_verify_20260708\yolo_rknn_annotated_stream_frame_016.png
C:\Users\mechrevo\Desktop\yolo\video_no_detail_verify_20260708\summary.json
```

After verification:

```text
/home/elf/rk3568_capture/captures was cleared
predict_batch.py analysis process was stopped
oct_qt_panel was restarted in normal non-capturing mode
new oct_qt_panel pid = 42530
```

Final source copies pulled back to the Windows workspace:

```text
D:\FPGA20251016\qianrushi\_codex_generated_temp\rk_final_mainwindow_video_no_detail_20260708.cpp
D:\FPGA20251016\qianrushi\_codex_generated_temp\rk_final_mainwindow_video_no_detail_20260708.h
D:\FPGA20251016\qianrushi\_codex_generated_temp\rk_final_predict_batch_video_no_detail_20260708.py
```

## 41. 2026-07-16 YOLO Video Playback Speed Stabilization

User reported that the analysis video playback still looked uneven, with visible
speed changes. The realtime raw/FPGA panes were left unchanged. The fix was
limited to the YOLO analysis video writer and the mpv launch parameters.

Root cause found on the active RK3588 deployment:

```text
/home/elf/Desktop/yolo/03_rk3588_model/predict_batch.py
  had reverted to OpenCV mp4v-first output:
    codecs = ["mp4v", "XVID", "MJPG"]

/home/elf/rk3568_capture/qt_panel/mainwindow.cpp
  opened mp4 first and launched mpv with --fps=10 plus cache/readahead options.
```

The existing generated video had correct nominal timestamps:

```text
yolo_rknn_annotated_stream.mp4
  codec: mpeg4
  size: 4096 x 2048
  avg_frame_rate: 10/1
  nb_frames: 55
```

But the high-bitrate MPEG-4 stream and forced playback override could still
make mpv appear to slow down and catch up on the RK3588 display.

Active fix deployed to RK3588:

```text
/home/elf/Desktop/yolo/03_rk3588_model/predict_batch.py
  open_video_writer() now prefers ffmpeg H.264 CFR MP4.
  The writer pipes BGR frames to ffmpeg rawvideo input.
  ffmpeg settings:
    -r 10
    -vf fps=10,setpts=N/(10*TB)
    -c:v libx264
    -preset veryfast
    -tune zerolatency
    -b:v 120M -maxrate 120M -bufsize 240M
    -g 10
    -bf 0
    -pix_fmt yuv420p
    -movflags +faststart

/home/elf/rk3568_capture/qt_panel/mainwindow.cpp
  mpv no longer forces --fps=10.
  mpv now uses:
    --speed=1.0
    --hwdec=rkmpp
    --video-sync=display-resample
    --framedrop=no
    --cache=no
```

Verification on RK3588:

```text
python3 -m py_compile /home/elf/Desktop/yolo/03_rk3588_model/predict_batch.py
cd /home/elf/rk3568_capture/qt_panel
cmake --build build -j2

Temporary 20-frame analysis video:
  codec_name=h264
  width=4096
  height=2048
  has_b_frames=0
  r_frame_rate=10/1
  avg_frame_rate=10/1
  duration=2.000000
  nb_frames=20

Packet timing after the fix:
  PTS/DTS/duration increments are 0.0, 0.1, 0.2, ... with 0.100000 duration.
```

The latest existing RK3588 diagnosis video was regenerated with the new writer
so the panel does not reopen the stale mp4v stream:

```text
/home/elf/rk3568_capture/captures/20260716_第001次诊断/analysis/yolo_rknn_annotated_stream.mp4
  codec_name=h264
  width=4096
  height=2048
  has_b_frames=0
  r_frame_rate=10/1
  avg_frame_rate=10/1
  duration=5.500000
  nb_frames=55
  size=96593651
```

The Qt panel was restarted after deployment:

```text
DISPLAY=:0 XAUTHORITY=/home/elf/.Xauthority \
  /home/elf/rk3568_capture/qt_panel/build/oct_qt_panel --capture-tab

new pid observed: 106189
```

Windows-side source package was also refreshed:

```text
C:\Users\mechrevo\Desktop\fpga-qt面板源码\LC-OCT-FPGA-QT-source-package_20260709_130750.zip
```
