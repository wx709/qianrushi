# Manifest

本清单记录本次打包的核心文件。

## Hardware

- `hardware/fpga_tuxiang_pcie_current/`: 当前 FPGA 工程源码和 Vivado 工程文件。
- `hardware/bitstreams/current/xilinx_dma_pcie_ep_20260706_current.bit`: 当前 bitstream。
- `hardware/flash/tuxiang_pcie_flash_20260706_package/xilinx_dma_pcie_ep_20260706_package.mcs`: 当前固化 MCS。
- `hardware/flash/tuxiang_pcie_flash_20260706_package/program_flash_prebridge.tcl`: 当前烧录 TCL。
- `hardware/flash/tuxiang_pcie_flash_20260706_package/program_flash_prebridge.log`: 当前烧录日志。

## RK3588 Software

- `software/rk3588_qt_panel_current/`: 当前 Qt 面板源码。
- `software/rk3588_yolo_analysis_current/predict_batch.py`: 当前 YOLO/RKNN 分析脚本。
- `software/rk3588_yolo_analysis_current/yolov8n_fpga1_full650_best.rknn`: 当前 RKNN 模型。
- `software/rk3588_yolo_analysis_current/best.onnx`: 当前 ONNX 模型。
- `software/rk3588_deploy_tools/`: 当前 RK3588 运行和诊断脚本。

## Docs

- `docs/handover/RK3588_OCT_PROJECT_HANDOVER.md`: 完整交接说明。
- `docs/guides/`: 其他部署和硬件触发说明。
