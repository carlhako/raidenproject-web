#!/usr/bin/env python3
"""Tile shots/frame_*.png into one labelled contact sheet."""
import sys, os, glob
from PIL import Image, ImageDraw
files = sorted(glob.glob(os.path.join(sys.argv[1] if len(sys.argv) > 1 else 'shots', 'frame_*.png')))
out = sys.argv[2] if len(sys.argv) > 2 else 'shots/contact.png'
cols = 5; w, h = 320, 256
rows = (len(files) + cols - 1) // cols
sheet = Image.new('RGB', (cols * w, rows * (h + 14)), (40, 40, 40))
d = ImageDraw.Draw(sheet)
for i, f in enumerate(files):
    im = Image.open(f).convert('RGB').resize((w, h))
    x, y = (i % cols) * w, (i // cols) * (h + 14)
    sheet.paste(im, (x, y + 14))
    d.text((x + 4, y + 1), os.path.basename(f), fill=(255, 255, 0))
sheet.save(out)
print(out, len(files))
