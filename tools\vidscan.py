#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Sample a video by decoding it sequentially instead of seeking.

cv2's POS_FRAMES seek lands on keyframes with long-GOP 4K phone recordings, so
several requested timestamps can return the very same picture.  This walks the
stream once and saves every frame whose timestamp crosses the next grid point.

Usage: vidscan.py VIDEO OUTDIR --step 0.25 --width 1000 [--start 5] [--end 40]
"""
import argparse
import os

import cv2
import numpy as np


def imwrite_u(path, img):
    ok, enc = cv2.imencode(os.path.splitext(path)[1] or '.png', img)
    if not ok:
        raise SystemExit('encode failed: %s' % path)
    enc.tofile(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('video')
    ap.add_argument('outdir')
    ap.add_argument('--step', type=float, default=0.25)
    ap.add_argument('--width', type=int, default=1000)
    ap.add_argument('--start', type=float, default=0.0)
    ap.add_argument('--end', type=float, default=1e9)
    a = ap.parse_args()

    os.makedirs(a.outdir, exist_ok=True)
    cap = cv2.VideoCapture(a.video)
    if not cap.isOpened():
        raise SystemExit('cannot open %s' % a.video)
    fps = cap.get(cv2.CAP_PROP_FPS) or 30.0
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT) or 0)
    print('fps %.2f  frames %d  dur %.1fs' % (fps, total, total / fps if fps else 0), flush=True)

    next_t = a.start
    idx = 0
    saved = 0
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        t = idx / fps if fps else idx / 30.0
        idx += 1
        if t + 1e-6 < next_t:
            continue
        if t > a.end:
            break
        if a.width and frame.shape[1] != a.width:
            h = int(round(frame.shape[0] * a.width / frame.shape[1]))
            frame = cv2.resize(frame, (a.width, h), interpolation=cv2.INTER_AREA)
        imwrite_u(os.path.join(a.outdir, 't%07.2f.png' % t), frame)
        saved += 1
        next_t += a.step
    cap.release()
    print('saved %d frames' % saved)


if __name__ == '__main__':
    main()
