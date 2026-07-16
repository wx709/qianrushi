给 RK3588 同门：

1. 首选使用 yolov8n_fpga_real_benign_20260706_best.onnx 转 RKNN。
2. 类别只有 1 类：0 Benign，见 names.txt。
3. 模型输入：1x3x640x640，BCHW；输出：1x5x8400。
4. 如果做 int8 量化，rknn_calibration.txt 指向本包内 calibration_images/ 的 128 张 FPGA 处理后图片。
5. 推理前处理要和 YOLOv8 一致：按 640 letterbox/resize、归一化到 0-1、灰度图转 3 通道。
6. example_predictions/ 里有一张带框示例图。
