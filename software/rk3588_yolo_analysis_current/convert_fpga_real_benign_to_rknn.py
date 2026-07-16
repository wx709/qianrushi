#!/usr/bin/env python3
from pathlib import Path

from rknn.api import RKNN


ROOT = Path(__file__).resolve().parent
ONNX_PATH = ROOT / "best.onnx"
RKNN_PATH = ROOT / "yolov8n_fpga_real_benign_20260706_best.rknn"


def main() -> int:
    if not ONNX_PATH.exists():
        raise SystemExit(f"missing ONNX: {ONNX_PATH}")

    rknn = RKNN(verbose=True)
    try:
        ret = rknn.config(
            mean_values=[[0, 0, 0]],
            std_values=[[255, 255, 255]],
            target_platform="rk3588",
        )
        if ret != 0:
            raise SystemExit(f"rknn.config failed: {ret}")

        ret = rknn.load_onnx(model=str(ONNX_PATH))
        if ret != 0:
            raise SystemExit(f"rknn.load_onnx failed: {ret}")

        ret = rknn.build(do_quantization=False)
        if ret != 0:
            raise SystemExit(f"rknn.build failed: {ret}")

        ret = rknn.export_rknn(str(RKNN_PATH))
        if ret != 0:
            raise SystemExit(f"rknn.export_rknn failed: {ret}")
    finally:
        rknn.release()

    print(f"exported={RKNN_PATH}")
    print(f"bytes={RKNN_PATH.stat().st_size}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
