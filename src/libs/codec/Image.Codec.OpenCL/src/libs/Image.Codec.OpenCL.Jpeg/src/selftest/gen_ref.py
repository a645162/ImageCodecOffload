#!/usr/bin/env python3
"""src/jpeg/selftest/gen_ref.py - 生成 JPEG 模块自测参考数据。

输出到当前目录（或 --dir <dir>）：
  ref.bmp / tiny8.bmp          参考图像（24-bit BMP）
  ref_444_q95.jpg ...          PIL(libjpeg) 编码的 baseline JPEG（4:4:4 / 4:2:0 / 4:2:2 / 灰度 / 8x8）
  *_pil.bmp                    PIL 解码的参考像素（与我们的解码对比）
依赖: pip install pillow numpy

File: gen_ref.py
Brief: Gen ref.
Author: Haomin Kong
Email: a645162@163.com
Created: 2026-08-12
"""
import argparse
import os

import numpy as np
from PIL import Image

def make_reference(w, h, seed=7):
    rng = np.random.default_rng(seed)
    # 大尺度梯度 + 色块 + 平滑变化，避免纯平块，覆盖 DC/AC 全频段
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    r = (128 + 96 * np.sin(xx / 23.0) * np.cos(yy / 17.0) + 40 * np.sin(xx / 7.0)).clip(0, 255)
    g = (128 + 96 * np.sin((xx + yy) / 29.0) + 30 * np.cos(yy / 11.0)).clip(0, 255)
    b = (128 + 90 * np.cos((xx - yy) / 31.0) + rng.normal(0, 6, (h, w))).clip(0, 255)
    # 彩色块
    r[40:80, 30:110] = 255
    g[40:80, 30:110] = 0
    b[40:80, 30:110] = 128
    r[100:160, 150:230] = 0
    g[100:160, 150:230] = 200
    b[100:160, 150:230] = 255
    img = np.stack([r, g, b], axis=-1).astype(np.uint8)
    return Image.fromarray(img, "RGB")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=".")
    args = ap.parse_args()
    os.makedirs(args.dir, exist_ok=True)
    d = args.dir

    ref = make_reference(256, 192)
    ref.save(os.path.join(d, "ref.bmp"))

    tiny = Image.fromarray(
        (np.arange(64).reshape(8, 8) * 3 + 40).astype(np.uint8), "L"
    ).convert("RGB")
    tiny.save(os.path.join(d, "tiny8.bmp"))

    jobs = [
        ("ref_444_q95.jpg", ref, 95, 0),
        ("ref_420_q90.jpg", ref, 90, 2),
        ("ref_422_q90.jpg", ref, 90, 1),
        ("gray_q95.jpg", ref.convert("L"), 95, 2),
        ("tiny8_q95.jpg", tiny, 95, 0),
    ]
    for name, img, q, sub in jobs:
        jpg = os.path.join(d, name)
        img.save(jpg, "JPEG", quality=q, subsampling=sub)
        dec = Image.open(jpg).convert("RGB")
        pil = os.path.join(d, name.replace(".jpg", "_pil.bmp"))
        dec.save(pil)
        print(f"{name}: {img.size} q={q} sub={sub}")

    print("reference data ready in", d)

if __name__ == "__main__":
    main()
