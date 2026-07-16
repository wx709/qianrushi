# LC-OCT FPGA + RK3588 + YOLO/RKNN Complete Project

本仓库用于交接当前完整工程，目标是让另一台设备 clone 后，可以由新的 Codex 继续维护当前项目。

当前主流程：

```text
OCT 相机采集 -> RK3588 -> FPGA -> YOLO/RKNN
```

## 目录

```text
hardware/fpga_tuxiang_pcie_current/
  当前升腾 Pro FPGA 图像处理 + PCIe/XDMA 工程源码、约束、IP 配置和 Vivado TCL。

hardware/bitstreams/current/
  当前已验证 bitstream。

hardware/flash/tuxiang_pcie_flash_20260706_package/
  当前已固化版本对应的 MCS/PRM、生成脚本、烧录脚本和烧录日志。

software/rk3588_qt_panel_current/
  RK3588 当前正在使用的 Qt 采集面板源码。

software/rk3588_yolo_analysis_current/
  RK3588 当前 YOLO/RKNN 分析脚本、ONNX/RKNN 模型、类别文件和板端转换说明。

software/rk3588_deploy_tools/
  RK3588 启动面板、PCIe 重新枚举、XDMA smoke test 等部署/诊断脚本。

docs/handover/
  当前完整交接说明。

docs/guides/
  早期部署、触发和补丁说明。
```

## 当前状态摘要

- FPGA：当前 `tuxiang_pcie` 图像处理 bitstream 已生成、下载、固化到升腾 Pro Flash，并通过 RK3588 PCIe/XDMA smoke test。
- RK3588 面板：当前 Qt 面板保留实时原始图和 FPGA 处理图显示；点击“分析识别”时只打开 YOLO/RKNN 识别视频流。
- YOLO/RKNN：当前模型为 `yolov8n_fpga1_full650_best.rknn`，分析输入是 FPGA 处理后的缓存图像。
- 视频流：当前已回退到 H.264 优化前的旧版逻辑；YOLO/RKNN 视频由 `predict_batch.py` 生成，Qt 面板用 mpv 打开。下一版视频稳定化方案待确认后再更新。

## 继续工作建议

1. 先读 `docs/handover/RK3588_OCT_PROJECT_HANDOVER.md`。
2. 再读 `RESTORE_ON_NEW_MACHINE.md`。
3. 如果只改 FPGA，优先进入 `hardware/fpga_tuxiang_pcie_current`。
4. 如果只改面板或模型部署，优先进入 `software/rk3588_qt_panel_current` 和 `software/rk3588_yolo_analysis_current`。

大型采集数据、诊断目录、训练数据集没有放入仓库；它们属于运行/训练数据，不是工程源码。
