# 新设备继续项目指南

## 1. 克隆仓库

```bash
git clone <this-repo-url>
cd LC-OCT-FPGA-Complete
```

## 2. FPGA 工程

当前工程目录：

```text
hardware/fpga_tuxiang_pcie_current
```

推荐用 Vivado 打开：

```text
hardware/fpga_tuxiang_pcie_current/prj/tuxiang_pcie.xpr
```

也可以用 TCL 重建/编译：

```text
hardware/fpga_tuxiang_pcie_current/prj/create_project.tcl
hardware/fpga_tuxiang_pcie_current/prj/build_bitstream.tcl
```

当前已验证 bitstream：

```text
hardware/bitstreams/current/xilinx_dma_pcie_ep_20260706_current.bit
```

当前 Flash 固化包：

```text
hardware/flash/tuxiang_pcie_flash_20260706_package/
```

## 3. RK3588 面板部署

当前面板源码：

```text
software/rk3588_qt_panel_current
```

在 RK3588 上编译：

```bash
cd ~/rk3568_capture/qt_panel
cmake --build build -j2
```

常用启动脚本已保存：

```text
software/rk3588_deploy_tools/run_qt_panel.sh
software/rk3588_deploy_tools/ensure_fpga_pcie_ready.sh
software/rk3588_deploy_tools/rk3588_tuxiang_pcie_test.py
```

## 4. YOLO/RKNN

当前分析目录：

```text
software/rk3588_yolo_analysis_current
```

关键文件：

```text
predict_batch.py
yolov8n_fpga1_full650_best.rknn
best.onnx
data.yaml
names.txt
RKNN_BOARD_CONVERSION_NOTE.md
```

RK3588 板端已经验证可用 `rknn-toolkit2 2.3.2` 转换 RKNN。为什么要转 RKNN：YOLO 训练通常输出 ONNX/PyTorch 模型，而 RK3588 NPU 需要 RKNN 格式才能用 RKNPU 加速推理。

## 5. 当前正确数据流

```text
OCT 相机采集
  -> RK3588 Qt 面板
  -> PCIe/XDMA 送入 FPGA
  -> FPGA 图像处理
  -> RK3588 缓存 FPGA processed images
  -> YOLO/RKNN 识别
  -> 10fps H.264 识别视频流
```

## 6. 不在仓库中的内容

以下内容通常很大，不适合直接进 GitHub：

```text
采集诊断目录
训练/测试数据集
批量 FPGA processed image 数据包
Vivado/Qt 临时编译缓存
```

如果新设备需要复现实验，需要另外从 RK3588 或移动硬盘拷贝相应数据集。
