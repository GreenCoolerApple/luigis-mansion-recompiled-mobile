#!/usr/bin/env python3
import os
import sys

from PIL import Image, ImageDraw

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SOURCE = os.path.join(ROOT, "packaging", "icon-source.png")
BACKGROUND = "#26358A"

ANDROID_DENSITIES = {"mdpi": 1.0, "hdpi": 1.5, "xhdpi": 2.0, "xxhdpi": 3.0, "xxxhdpi": 4.0}

IOS_SIZES = {
    "AppIcon60x60@2x.png": 120,
    "AppIcon60x60@3x.png": 180,
    "AppIcon76x76@2x~ipad.png": 152,
    "AppIcon83.5x83.5@2x~ipad.png": 167,
    "AppIcon1024.png": 1024,
}


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def remove(path):
    if os.path.exists(path):
        os.remove(path)


def main():
    if len(sys.argv) > 1:
        Image.open(sys.argv[1]).convert("RGB").save(SOURCE)
    src = Image.open(SOURCE).convert("RGB")

    res = os.path.join(ROOT, "packaging", "android", "app", "src", "main", "res")
    remove(os.path.join(res, "drawable", "ic_launcher_foreground.xml"))
    for density, scale in ANDROID_DENSITIES.items():
        folder = os.path.join(res, f"mipmap-{density}")
        os.makedirs(folder, exist_ok=True)
        fg = round(108 * scale)
        inner = round(76 * scale)
        canvas = Image.new("RGB", (fg, fg), BACKGROUND)
        canvas.paste(src.resize((inner, inner), Image.LANCZOS), ((fg - inner) // 2, (fg - inner) // 2))
        canvas.save(os.path.join(folder, "ic_launcher_foreground.png"))
        legacy = round(48 * scale)
        icon = src.resize((legacy, legacy), Image.LANCZOS)
        icon.save(os.path.join(folder, "ic_launcher.png"))
        mask = Image.new("L", (legacy * 4, legacy * 4), 0)
        ImageDraw.Draw(mask).ellipse((0, 0, legacy * 4 - 1, legacy * 4 - 1), fill=255)
        mask = mask.resize((legacy, legacy), Image.LANCZOS)
        round_icon = icon.convert("RGBA")
        round_icon.putalpha(mask)
        round_icon.save(os.path.join(folder, "ic_launcher_round.png"))

    write(os.path.join(res, "values", "ic_launcher_background.xml"),
          '<?xml version="1.0" encoding="utf-8"?>\n<resources>\n'
          f'    <color name="ic_launcher_background">{BACKGROUND}</color>\n</resources>\n')
    adaptive = ('<?xml version="1.0" encoding="utf-8"?>\n'
                '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
                '    <background android:drawable="@color/ic_launcher_background" />\n'
                '    <foreground android:drawable="@mipmap/ic_launcher_foreground" />\n'
                '</adaptive-icon>\n')
    write(os.path.join(res, "mipmap-anydpi-v26", "ic_launcher.xml"), adaptive)
    write(os.path.join(res, "mipmap-anydpi-v26", "ic_launcher_round.xml"), adaptive)

    ios = os.path.join(ROOT, "packaging", "ios", "icons")
    os.makedirs(ios, exist_ok=True)
    for name, px in IOS_SIZES.items():
        src.resize((px, px), Image.LANCZOS).save(os.path.join(ios, name))
    remove(os.path.join(ROOT, "packaging", "icon-preview.png"))
    print("icons written")


if __name__ == "__main__":
    main()
