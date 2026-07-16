# RK3588 Board-Side RKNN Conversion Note

Current active model:

```text
yolov8n_fpga1_full650_best.rknn
```

This model is for one-class lesion detection on FPGA-processed LC-OCT images.

Critical input rule:

```text
YOLO/RKNN input must come from diagnosis/fpga_processed_images/*.pgm
```

Do not use `input_preview`, `current/test/images`, raw, or line images as direct YOLO/RKNN input.

Conversion used on RK3588:

```text
rknn-toolkit2: 2.3.2
target_platform = rk3588
mean_values = [[0, 0, 0]]
std_values = [[255, 255, 255]]
do_quantization = False
```

Model settings:

```text
architecture: YOLOv8n
input: 1x3x640x640
output: 1x5x8400
class 0: lesion
confidence threshold: 0.40
NMS IoU threshold: 0.50
```

Board validation:

```text
3 positive FPGA-processed samples detected, confidence about 0.83-0.85.
2 negative FPGA-processed samples produced no boxes.
```
