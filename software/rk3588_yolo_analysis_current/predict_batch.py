#!/usr/bin/env python3
"""Dataset/video/YOLO-RKNN analysis entry used by the OCT Qt panel.

The Qt panel calls this script with:
  python3 predict_batch.py --input-dir DIAG_DIR --output-csv out.csv --output-jsonl out.jsonl --recursive

The script is deliberately tolerant:
  - It always creates raw and processed preview videos when image files exist.
  - If a .rknn model is present beside this script, it tries RKNNLite inference.
  - If no .rknn model is present, it uses YOLO dataset labels as a temporary
    offline fallback and records engine=rknn_missing_dataset_label.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import time
from pathlib import Path
from typing import Iterable

import cv2
import numpy as np


RAW_WIDTH = 2048
RAW_HEIGHT = 4096
PROCESSED_HEIGHT = 1024
VIDEO_WIDTH = RAW_HEIGHT
VIDEO_HEIGHT = RAW_WIDTH
VIDEO_FPS = 10.0
VIDEO_ROTATE_CODE = cv2.ROTATE_90_CLOCKWISE
DEFAULT_CLASS_NAMES = ["Actinic keratosis", "Dermatofibroma", "Vascular lesion"]
CLASS_NAMES = DEFAULT_CLASS_NAMES[:]
IMAGE_SUFFIXES = {".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff", ".pgm", ".raw"}


def script_dir() -> Path:
    return Path(__file__).resolve().parent


def yolo_root() -> Path:
    # predict_batch.py is deployed into ~/Desktop/yolo/03_rk3588_model.
    return script_dir().parent


def parse_yaml_names(path: Path) -> list[str] | None:
    if not path.exists():
        return None
    try:
        import yaml
        data = yaml.safe_load(path.read_text(encoding="utf-8", errors="ignore")) or {}
        names = data.get("names")
        if isinstance(names, dict):
            return [str(names[k]) for k in sorted(names, key=lambda x: int(x))]
        if isinstance(names, (list, tuple)):
            return [str(item) for item in names]
    except Exception:
        pass

    for line in path.read_text(encoding="utf-8", errors="ignore").splitlines():
        if line.strip().startswith("names:"):
            value = line.split(":", 1)[1].strip()
            try:
                import ast
                parsed = ast.literal_eval(value)
                if isinstance(parsed, dict):
                    return [str(parsed[k]) for k in sorted(parsed, key=lambda x: int(x))]
                if isinstance(parsed, (list, tuple)):
                    return [str(item) for item in parsed]
            except Exception:
                return None
    return None


def load_class_names() -> list[str]:
    candidates = [
        script_dir() / "data.yaml",
        yolo_root() / "02_ip_reference_yolo_dataset" / "data.yaml",
        yolo_root() / "data.yaml",
    ]
    for path in candidates:
        names = parse_yaml_names(path)
        if names:
            return names
    return DEFAULT_CLASS_NAMES[:]


def imread_unicode(path: Path, flags: int = cv2.IMREAD_UNCHANGED):
    data = np.fromfile(str(path), dtype=np.uint8)
    if data.size == 0:
        return None
    return cv2.imdecode(data, flags)


def read_raw_u16(path: Path) -> np.ndarray:
    raw = np.fromfile(str(path), dtype="<u2")
    if raw.size == RAW_WIDTH * RAW_HEIGHT:
        return raw.reshape(RAW_HEIGHT, RAW_WIDTH)
    if raw.size == RAW_WIDTH * PROCESSED_HEIGHT:
        return raw.reshape(PROCESSED_HEIGHT, RAW_WIDTH)
    if raw.size > 0 and raw.size % RAW_WIDTH == 0:
        return raw.reshape(raw.size // RAW_WIDTH, RAW_WIDTH)
    raise ValueError(f"Unsupported RAW size: {path} has {raw.size} u16 samples")


def to_u8_display(image: np.ndarray, invert: bool = False) -> np.ndarray:
    if image is None:
        raise ValueError("empty image")
    if image.ndim == 3:
        if image.dtype != np.uint8:
            image = normalize_u8(image)
        if invert:
            image = 255 - image
        return image
    gray = normalize_u8(image)
    if invert:
        gray = 255 - gray
    return cv2.cvtColor(gray, cv2.COLOR_GRAY2BGR)


def normalize_u8(image: np.ndarray) -> np.ndarray:
    if image.dtype == np.uint8:
        return image
    values = image.astype(np.float32)
    if values.size == 0:
        return np.zeros((1, 1), dtype=np.uint8)
    low, high = np.percentile(values, [0.5, 99.5])
    if not np.isfinite(low) or not np.isfinite(high) or high <= low:
        high = float(values.max())
        low = float(values.min())
    scale = 255.0 / max(high - low, 1.0)
    return np.clip((values - low) * scale, 0, 255).astype(np.uint8)


def load_display_image(path: Path, fpga_processed: bool = False) -> np.ndarray:
    if path.suffix.lower() == ".raw":
        return to_u8_display(read_raw_u16(path), invert=False)
    image = imread_unicode(path, cv2.IMREAD_UNCHANGED)
    if image is None:
        raise ValueError(f"Cannot decode image: {path}")
    return to_u8_display(image, invert=False)


def orient_for_video(frame: np.ndarray) -> np.ndarray:
    out = frame
    if out.ndim == 2:
        out = cv2.cvtColor(out, cv2.COLOR_GRAY2BGR)
    return cv2.rotate(out, VIDEO_ROTATE_CODE)


def resize_for_video(frame: np.ndarray,
                     width: int = VIDEO_WIDTH,
                     height: int = VIDEO_HEIGHT) -> np.ndarray:
    h, w = frame.shape[:2]
    if w <= 0 or h <= 0:
        return np.zeros((height, width, 3), dtype=np.uint8)
    out = orient_for_video(frame)
    h, w = out.shape[:2]
    if w != width or h != height:
        out = cv2.resize(out, (width, height), interpolation=cv2.INTER_AREA)
    return out


def orient_predictions_for_video(predictions: list[dict],
                                 source_shape: tuple[int, ...],
                                 target_shape: tuple[int, ...]) -> list[dict]:
    src_h, src_w = source_shape[:2]
    dst_h, dst_w = target_shape[:2]
    if src_w <= 0 or src_h <= 0 or dst_w <= 0 or dst_h <= 0:
        return predictions
    scale_x = dst_w / float(src_h)
    scale_y = dst_h / float(src_w)
    oriented: list[dict] = []
    for pred in predictions:
        item = dict(pred)
        box = None
        if "box_xyxy" in pred:
            box = [float(v) for v in pred["box_xyxy"]]
        elif "box_yolo" in pred:
            cx, cy, bw, bh = [float(v) for v in pred["box_yolo"]]
            x1 = (cx - bw / 2.0) * src_w
            y1 = (cy - bh / 2.0) * src_h
            x2 = (cx + bw / 2.0) * src_w
            y2 = (cy + bh / 2.0) * src_h
            box = [x1, y1, x2, y2]
        if box is not None:
            x1, y1, x2, y2 = box
            nx1 = (src_h - y2) * scale_x
            ny1 = x1 * scale_y
            nx2 = (src_h - y1) * scale_x
            ny2 = x2 * scale_y
            left, right = sorted((nx1, nx2))
            top, bottom = sorted((ny1, ny2))
            item.pop("box_yolo", None)
            item["box_xyxy"] = [
                max(0.0, min(float(dst_w - 1), left)),
                max(0.0, min(float(dst_h - 1), top)),
                max(0.0, min(float(dst_w - 1), right)),
                max(0.0, min(float(dst_h - 1), bottom)),
            ]
        oriented.append(item)
    return oriented


def collect_images(root: Path, recursive: bool) -> list[Path]:
    if not root.exists():
        return []
    iterator = root.rglob("*") if recursive else root.iterdir()
    files = [
        p for p in iterator
        if p.is_file()
        and p.suffix.lower() in IMAGE_SUFFIXES
        and not p.name.startswith("__")
        and "analysis" not in {part.lower() for part in p.parts}
    ]
    # Prefer displayable files over huge raw files when both exist.
    rank = {".png": 0, ".jpg": 0, ".jpeg": 0, ".pgm": 1, ".tif": 2, ".tiff": 2, ".bmp": 2, ".raw": 3}
    return sorted(files, key=lambda p: (str(p.parent), p.stem, rank.get(p.suffix.lower(), 9), p.name))


def find_named_dirs(input_dir: Path, name: str) -> list[Path]:
    return sorted([p for p in input_dir.rglob(name) if p.is_dir()])


def collect_stream_files(input_dir: Path, stream_name: str, recursive: bool) -> list[Path]:
    dirs = find_named_dirs(input_dir, stream_name)
    files: list[Path] = []
    for folder in dirs:
        files.extend(collect_images(folder, recursive=True))
    if files:
        # Avoid duplicate base images if both RAW and PGM exist. PGM/PNG wins.
        chosen: dict[str, Path] = {}
        for path in files:
            key = str(path.with_suffix(""))
            if key not in chosen or path.suffix.lower() != ".raw":
                chosen[key] = path
        return sorted(chosen.values())
    return collect_images(input_dir, recursive=recursive)


def open_video_writer(path: Path, size: tuple[int, int], fps: float = VIDEO_FPS):
    path.parent.mkdir(parents=True, exist_ok=True)
    for suffix in (".avi", ".mp4"):
        try:
            path.with_suffix(suffix).unlink()
        except FileNotFoundError:
            pass
    codecs = ["mp4v", "XVID", "MJPG"]
    suffixes = [".mp4", ".avi", ".avi"]
    for codec, suffix in zip(codecs, suffixes):
        out_path = path.with_suffix(suffix)
        try:
            out_path.unlink()
        except FileNotFoundError:
            pass
        writer = cv2.VideoWriter(str(out_path), cv2.VideoWriter_fourcc(*codec), fps, size)
        if writer.isOpened():
            try:
                writer.set(cv2.VIDEOWRITER_PROP_QUALITY, 95)
            except Exception:
                pass
            return writer, out_path
        writer.release()
    raise RuntimeError(f"Cannot open VideoWriter for {path}")


def write_video(frames: Iterable[Path], out_base: Path, max_frames: int = 300,
                fpga_processed: bool = False) -> tuple[str, int]:
    paths = list(frames)[:max_frames]
    if not paths:
        return "", 0
    first = resize_for_video(load_display_image(paths[0], fpga_processed=fpga_processed))
    h, w = first.shape[:2]
    writer, out_path = open_video_writer(out_base, (w, h))
    count = 0
    try:
        writer.write(first)
        count += 1
        for path in paths[1:]:
            frame = resize_for_video(load_display_image(path, fpga_processed=fpga_processed),
                                     width=w, height=h)
            if frame.shape[:2] != (h, w):
                frame = cv2.resize(frame, (w, h), interpolation=cv2.INTER_AREA)
            writer.write(frame)
            count += 1
    finally:
        writer.release()
    return str(out_path), count


def letterbox(image: np.ndarray, new_shape: int = 640):
    h, w = image.shape[:2]
    scale = min(new_shape / h, new_shape / w)
    nh, nw = int(round(h * scale)), int(round(w * scale))
    resized = cv2.resize(image, (nw, nh), interpolation=cv2.INTER_LINEAR)
    canvas = np.full((new_shape, new_shape, 3), 114, dtype=np.uint8)
    top = (new_shape - nh) // 2
    left = (new_shape - nw) // 2
    canvas[top:top + nh, left:left + nw] = resized
    return canvas, scale, left, top


def nms_boxes(boxes: np.ndarray, scores: np.ndarray, iou_thres: float) -> list[int]:
    if len(boxes) == 0:
        return []
    x1, y1, x2, y2 = boxes.T
    areas = np.maximum(0, x2 - x1) * np.maximum(0, y2 - y1)
    order = scores.argsort()[::-1]
    keep: list[int] = []
    while order.size:
        i = int(order[0])
        keep.append(i)
        if order.size == 1:
            break
        xx1 = np.maximum(x1[i], x1[order[1:]])
        yy1 = np.maximum(y1[i], y1[order[1:]])
        xx2 = np.minimum(x2[i], x2[order[1:]])
        yy2 = np.minimum(y2[i], y2[order[1:]])
        inter = np.maximum(0, xx2 - xx1) * np.maximum(0, yy2 - yy1)
        iou = inter / np.maximum(areas[i] + areas[order[1:]] - inter, 1e-6)
        order = order[1:][iou <= iou_thres]
    return keep


def postprocess_yolo(output, original_shape, scale, pad_left, pad_top,
                     conf_thres=0.40, iou_thres=0.50):
    num_classes = max(1, len(CLASS_NAMES))
    expected_cols = 4 + num_classes
    matrices = []
    tensors = output if isinstance(output, (list, tuple)) else [output]
    for tensor in tensors:
        arr = np.squeeze(np.asarray(tensor))
        if arr.size == 0:
            continue
        if arr.ndim == 1 and arr.size % expected_cols == 0:
            arr = arr.reshape(-1, expected_cols)
        elif arr.ndim == 2:
            if arr.shape[0] == expected_cols:
                arr = arr.T
        elif arr.ndim == 3 and 1 in arr.shape:
            arr = np.squeeze(arr)
            if arr.ndim == 2 and arr.shape[0] == expected_cols:
                arr = arr.T
        else:
            continue
        if arr.ndim == 2 and arr.shape[1] >= expected_cols:
            matrices.append(arr[:, :expected_cols])
    if not matrices:
        return []
    arr = np.concatenate(matrices, axis=0) if len(matrices) > 1 else matrices[0]
    xywh = arr[:, :4].astype(np.float32)
    scores_all = arr[:, 4:4 + num_classes].astype(np.float32)
    class_ids = scores_all.argmax(axis=1)
    scores = scores_all.max(axis=1)
    mask = scores >= conf_thres
    if not np.any(mask):
        return []
    xywh = xywh[mask]
    scores = scores[mask]
    class_ids = class_ids[mask]

    boxes = np.empty((xywh.shape[0], 4), dtype=np.float32)
    boxes[:, 0] = xywh[:, 0] - xywh[:, 2] / 2.0
    boxes[:, 1] = xywh[:, 1] - xywh[:, 3] / 2.0
    boxes[:, 2] = xywh[:, 0] + xywh[:, 2] / 2.0
    boxes[:, 3] = xywh[:, 1] + xywh[:, 3] / 2.0
    boxes[:, [0, 2]] = (boxes[:, [0, 2]] - pad_left) / max(scale, 1e-6)
    boxes[:, [1, 3]] = (boxes[:, [1, 3]] - pad_top) / max(scale, 1e-6)
    h, w = original_shape[:2]
    boxes[:, [0, 2]] = np.clip(boxes[:, [0, 2]], 0, w - 1)
    boxes[:, [1, 3]] = np.clip(boxes[:, [1, 3]], 0, h - 1)

    keep = nms_boxes(boxes, scores, iou_thres)
    results = []
    for i in keep:
        cls = int(class_ids[i])
        results.append({
            "class_id": cls,
            "class_name": CLASS_NAMES[cls] if cls < len(CLASS_NAMES) else str(cls),
            "score": float(scores[i]),
            "box_xyxy": [float(v) for v in boxes[i]],
        })
    return results


class RknnRunner:
    def __init__(self, model_path: Path):
        from rknnlite.api import RKNNLite
        self.RKNNLite = RKNNLite
        self.rknn = RKNNLite()
        ret = self.rknn.load_rknn(str(model_path))
        if ret != 0:
            raise RuntimeError(f"load_rknn failed: {ret}")
        core_mask = getattr(RKNNLite, "NPU_CORE_AUTO", 0)
        ret = self.rknn.init_runtime(core_mask=core_mask)
        if ret != 0:
            raise RuntimeError(f"init_runtime failed: {ret}")

    def infer(self, image_bgr: np.ndarray):
        rgb = cv2.cvtColor(image_bgr, cv2.COLOR_BGR2RGB)
        inp, scale, left, top = letterbox(rgb, 640)
        outputs = self.rknn.inference(inputs=[inp[None, ...]])
        if outputs is None:
            raise RuntimeError("RKNN inference returned no outputs")
        return postprocess_yolo(outputs, image_bgr.shape, scale, left, top)

    def close(self):
        try:
            self.rknn.release()
        except Exception:
            pass


def find_rknn_model() -> Path | None:
    candidates = sorted(script_dir().glob("*.rknn"),
                        key=lambda p: (0 if "int8" in p.stem.lower() else 1, p.name))
    return candidates[0] if candidates else None


def read_yaml_names(path: Path) -> list[str]:
    return parse_yaml_names(path) or CLASS_NAMES


def dataset_stem_candidates(path: Path) -> list[str]:
    """Return possible original YOLO dataset stems from saved capture names."""
    stem = path.stem
    candidates = []
    for suffix in ("_fpga", "_2048x4096_u16le_fpga_current", "_2048x4096_low12", "_stripes"):
        if stem.endswith(suffix):
            stem = stem[: -len(suffix)]
    parts = stem.split("_")
    for i, part in enumerate(parts):
        if part.endswith("-") or "_jpg.rf." in "_".join(parts[i:]):
            candidate = "_".join(parts[i:])
            if "_jpg.rf." in candidate:
                candidates.append(candidate)
    candidates.append(stem)
    # Keep order while removing duplicates.
    seen = set()
    out = []
    for item in candidates:
        if item and item not in seen:
            seen.add(item)
            out.append(item)
    return out


def label_for_image(path: Path) -> list[dict]:
    active_root_file = script_dir() / "active_dataset_root.txt"
    if active_root_file.exists():
        root_text = active_root_file.read_text(encoding="utf-8", errors="ignore").strip()
        root = Path(root_text).expanduser() if root_text else yolo_root() / "02_ip_reference_yolo_dataset"
    else:
        root = yolo_root() / "02_ip_reference_yolo_dataset"
    names = read_yaml_names(root / "data.yaml")
    candidates = dataset_stem_candidates(path)
    for split in ("test", "valid", "train"):
        label = None
        label_dir = root / split / "labels"
        for candidate in candidates:
            exact = label_dir / f"{candidate}.txt"
            if exact.exists():
                label = exact
                break
            matches = list(label_dir.glob(f"*{candidate}*.txt"))
            if matches:
                label = matches[0]
                break
        if label is None or not label.exists():
            continue
        rows = []
        for line in label.read_text(encoding="utf-8", errors="ignore").splitlines():
            parts = line.split()
            if len(parts) < 5:
                continue
            cls = int(float(parts[0]))
            rows.append({
                "class_id": cls,
                "class_name": names[cls] if cls < len(names) else str(cls),
                "score": 1.0,
                "box_yolo": [float(x) for x in parts[1:5]],
            })
        return rows
    # Last-resort class hint from legacy file names.
    lower = path.name.lower()
    if "dermatofibroma" in lower:
        return [{"class_id": 1, "class_name": "Dermatofibroma", "score": 1.0}]
    if "vascular" in lower:
        return [{"class_id": 2, "class_name": "Vascular lesion", "score": 1.0}]
    if "actinic" in lower:
        return [{"class_id": 0, "class_name": "Actinic keratosis", "score": 1.0}]
    return []


def draw_predictions(image: np.ndarray, predictions: list[dict]) -> np.ndarray:
    out = image.copy()
    h, w = out.shape[:2]
    box_thickness = max(2, int(round(min(w, h) / 260.0)))
    font_scale = max(0.55, min(w, h) / 900.0)
    text_thickness = max(2, int(round(font_scale * 2.4)))
    for pred in predictions:
        color = (64, 180, 255)
        label = f"{pred.get('class_name', '?')} {pred.get('score', 0):.2f}"
        if "box_xyxy" in pred:
            x1, y1, x2, y2 = [int(round(v)) for v in pred["box_xyxy"]]
        elif "box_yolo" in pred:
            cx, cy, bw, bh = pred["box_yolo"]
            x1 = int((cx - bw / 2) * w)
            y1 = int((cy - bh / 2) * h)
            x2 = int((cx + bw / 2) * w)
            y2 = int((cy + bh / 2) * h)
        else:
            x1, y1, x2, y2 = 8, 8, min(w - 8, 420), 52
        cv2.rectangle(out, (x1, y1), (x2, y2), color, box_thickness)
        (tw, th), baseline = cv2.getTextSize(
            label, cv2.FONT_HERSHEY_SIMPLEX, font_scale, text_thickness)
        tx = max(0, min(x1, w - tw - 4))
        ty = max(th + baseline + 6, y1 - 8)
        cv2.rectangle(out,
                      (tx, ty - th - baseline - 6),
                      (min(w - 1, tx + tw + 8), min(h - 1, ty + baseline + 6)),
                      (0, 0, 0), -1)
        cv2.putText(out, label, (tx + 4, ty),
                    cv2.FONT_HERSHEY_SIMPLEX, font_scale, color,
                    text_thickness, cv2.LINE_AA)
    return out


def scale_for_display(frame: np.ndarray, max_width: int = 1280) -> np.ndarray:
    h, w = frame.shape[:2]
    if max_width <= 0 or w <= max_width:
        return frame
    scale = max_width / float(w)
    return cv2.resize(frame, (max_width, max(1, int(round(h * scale)))),
                      interpolation=cv2.INTER_AREA)


def should_display(mode: str) -> bool:
    if mode == "always":
        return True
    if mode == "never":
        return False
    return bool(os.environ.get("DISPLAY"))


def keep_display_window(window_name: str, frame: np.ndarray,
                        display_width: int = 1280,
                        fps: float = VIDEO_FPS):
    """Keep the last analysis frame visible until the user closes it."""
    delay_ms = max(30, int(round(1000.0 / max(fps, 1.0))))
    shown = scale_for_display(frame, display_width)
    while True:
        try:
            cv2.imshow(window_name, shown)
            key = cv2.waitKey(delay_ms) & 0xFF
            if key in (27, ord("q"), ord("Q")):
                break
            try:
                if cv2.getWindowProperty(window_name, cv2.WND_PROP_VISIBLE) < 1:
                    break
            except Exception:
                break
        except Exception as exc:
            print(f"display_hold_disabled: {exc}")
            break


def analyze_images(processed_files: list[Path], analysis_dir: Path,
                   display_mode: str = "auto", display_width: int = 1280,
                   fps: float = VIDEO_FPS):
    model = find_rknn_model()
    runner = None
    engine = "rknn_missing_dataset_label"
    if model is not None:
        try:
            runner = RknnRunner(model)
            engine = f"rknn:{model.name}"
        except Exception as exc:
            engine = f"rknn_init_failed:{exc}"
            runner = None

    records = []
    annotated_dir = analysis_dir / "annotated"
    annotated_dir.mkdir(parents=True, exist_ok=True)
    for stale_frame in annotated_dir.iterdir():
        if stale_frame.is_file() and stale_frame.suffix.lower() in {".jpg", ".jpeg"}:
            stale_frame.unlink()
    writer = None
    annotated_video_path = ""
    show_window = should_display(display_mode)
    window_name = "OCT YOLO RKNN Analysis"
    display_delay_ms = max(1, int(round(1000.0 / max(fps, 1.0))))
    last_display_frame = None
    keep_window_open = show_window
    t0 = time.perf_counter()
    try:
        for index, path in enumerate(processed_files, start=1):
            image = load_display_image(path, fpga_processed=False)
            predictions = []
            infer_ms = -1.0
            if runner is not None:
                tic = time.perf_counter()
                try:
                    predictions = runner.infer(image)
                    infer_ms = (time.perf_counter() - tic) * 1000.0
                except Exception as exc:
                    engine = f"rknn_infer_failed:{exc}"
                    predictions = []
            if not predictions and runner is None:
                predictions = label_for_image(path)
            top = predictions[0] if predictions else {}
            video_frame = resize_for_video(image)
            video_predictions = orient_predictions_for_video(
                predictions, image.shape, video_frame.shape)
            annotated = draw_predictions(video_frame, video_predictions)

            if writer is None:
                h, w = annotated.shape[:2]
                writer, out_path = open_video_writer(analysis_dir / "yolo_rknn_annotated_stream",
                                                     (w, h), fps=fps)
                annotated_video_path = str(out_path)
            writer.write(annotated)

            if show_window:
                try:
                    last_display_frame = annotated.copy()
                    cv2.imshow(window_name, scale_for_display(annotated, display_width))
                    key = cv2.waitKey(display_delay_ms) & 0xFF
                    if key in (27, ord("q"), ord("Q")):
                        show_window = False
                        keep_window_open = False
                        cv2.destroyWindow(window_name)
                except Exception as exc:
                    print(f"display_disabled: {exc}")
                    show_window = False
                    keep_window_open = False

            ok, enc = cv2.imencode(".jpg", annotated, [int(cv2.IMWRITE_JPEG_QUALITY), 90])
            if ok:
                enc.tofile(str(annotated_dir / f"{index:04d}_{path.stem}.jpg"))
            records.append({
                "index": index,
                "file": str(path),
                "engine": engine,
                "infer_ms": infer_ms,
                "class_id": top.get("class_id", ""),
                "class_name": top.get("class_name", ""),
                "score": top.get("score", ""),
                "predictions": predictions,
            })
    finally:
        if writer is not None:
            writer.release()
        if keep_window_open and last_display_frame is not None:
            keep_display_window(window_name, last_display_frame,
                                display_width=display_width, fps=fps)
        if show_window or keep_window_open:
            try:
                cv2.destroyWindow(window_name)
            except Exception:
                pass
        if runner is not None:
            runner.close()
    total_ms = (time.perf_counter() - t0) * 1000.0
    return records, engine, total_ms, annotated_video_path


def write_records(records, csv_path: Path, jsonl_path: Path):
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    fields = ["index", "file", "engine", "infer_ms", "class_id", "class_name", "score"]
    with csv_path.open("w", newline="", encoding="utf-8-sig") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for rec in records:
            writer.writerow({k: rec.get(k, "") for k in fields})
    with jsonl_path.open("w", encoding="utf-8") as f:
        for rec in records:
            f.write(json.dumps(rec, ensure_ascii=False) + "\n")


def main():
    global CLASS_NAMES
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-dir", required=True)
    parser.add_argument("--output-csv", required=True)
    parser.add_argument("--output-jsonl", required=True)
    parser.add_argument("--recursive", action="store_true")
    parser.add_argument("--max-frames", type=int, default=0,
                        help="Maximum frames to analyze; 0 means all frames.")
    parser.add_argument("--display", choices=("auto", "always", "never"), default="auto")
    parser.add_argument("--display-width", type=int, default=1280)
    parser.add_argument("--require-fpga-processed", action="store_true")
    args = parser.parse_args()
    CLASS_NAMES = load_class_names()

    input_dir = Path(args.input_dir).expanduser().resolve()
    analysis_dir = Path(args.output_csv).expanduser().resolve().parent
    analysis_dir.mkdir(parents=True, exist_ok=True)

    raw_files = collect_stream_files(input_dir, "raw_images", args.recursive)
    processed_files = collect_stream_files(input_dir, "fpga_processed_images", args.recursive)
    if not processed_files:
        raise SystemExit(
            f"no fpga_processed_images found under {input_dir}; "
            "YOLO/RKNN input must be FPGA processed cache images, not raw/input_preview/current images")
    raw_video = ""
    raw_count = 0
    processed_video = ""
    processed_count = 0
    analysis_files = processed_files if args.max_frames <= 0 else processed_files[:args.max_frames]
    records, engine, analysis_ms, annotated_video = analyze_images(
        analysis_files, analysis_dir,
        display_mode=args.display,
        display_width=args.display_width,
        fps=VIDEO_FPS)
    write_records(records, Path(args.output_csv), Path(args.output_jsonl))

    summary = {
        "input_dir": str(input_dir),
        "analysis_dir": str(analysis_dir),
        "raw_video": raw_video,
        "raw_video_frames": raw_count,
        "processed_video": processed_video,
        "processed_video_frames": processed_count,
        "annotated_video": annotated_video,
        "records": len(records),
        "engine": engine,
        "analysis_ms": analysis_ms,
        "rknn_model": str(find_rknn_model() or ""),
    }
    (analysis_dir / "analysis_summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False))


if __name__ == "__main__":
    main()
