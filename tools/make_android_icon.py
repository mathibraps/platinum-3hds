#!/usr/bin/env python3
"""Makes the Android builder's launcher icon (192x192, original art in the
style of the builder's HOME menu banner: tools/make_builder_art.py).

usage: make_android_icon.py <out.png>
"""
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from make_app_art import write_png  # noqa: E402
from make_builder_art import backdrop, text  # noqa: E402

img = backdrop(192, 192)
text(img, "P", 96, 34, 16, (255, 255, 255), (150, 160, 190))
text(img, "3HDS", 96, 156, 2, (255, 215, 120), (200, 70, 60))
write_png(sys.argv[1], 192, 192, img)
