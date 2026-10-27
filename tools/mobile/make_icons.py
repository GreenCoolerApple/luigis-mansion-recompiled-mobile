#!/usr/bin/env python3
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

BACKGROUND = "#1B1033"


def circle(cx, cy, r):
    return f"M{cx - r},{cy} a{r},{r} 0 1,0 {2 * r},0 a{r},{r} 0 1,0 {-2 * r},0 Z"


def rect(x, y, w, h):
    return f"M{x},{y} h{w} v{h} h{-w} Z"


SHAPES = [
    ("#2A1A4F", 1.0, circle(54, 54, 34)),
    ("#F3E9B5", 1.0, circle(66, 33, 8)),
    ("#2A1A4F", 1.0, circle(69, 30.5, 6)),
    ("#0B0716", 1.0, "M28,88 V54 L37,40 L46,54 V52 L55,38 L64,52 V54 L71,44 L80,54 V88 Z"),
    ("#0B0716", 1.0, rect(58, 38, 4, 10)),
    ("#7CFF6B", 1.0, rect(34, 58, 5, 6)),
    ("#7CFF6B", 1.0, rect(52, 48, 6, 5)),
    ("#7CFF6B", 0.55, rect(70, 58, 5, 6)),
    ("#7CFF6B", 1.0, rect(70, 70, 5, 6)),
    ("#7CFF6B", 0.4, rect(34, 70, 5, 6)),
    ("#3B2B66", 1.0, "M49,88 V76 a5,5 0 0,1 10,0 V88 Z"),
    ("#FFF3A0", 0.32, "M18,96 L38,60 L60,74 Z"),
    ("#FFFFFF", 0.92, "M33,82 V72 a7,7 0 0,1 14,0 V82 l-2.3,-2 l-2.4,2 l-2.3,-2 l-2.4,2 l-2.3,-2 l-2.3,2 Z"),
    ("#1B1033", 1.0, circle(37.5, 72, 1.2)),
    ("#1B1033", 1.0, circle(42.5, 72, 1.2)),
]


def svg(size, with_background=True):
    body = []
    if with_background:
        body.append(f'<rect width="108" height="108" fill="{BACKGROUND}"/>')
    for color, alpha, d in SHAPES:
        body.append(f'<path d="{d}" fill="{color}" fill-opacity="{alpha}"/>')
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" viewBox="18 18 72 72">'
            + "".join(body) + "</svg>")


def vector_drawable():
    paths = []
    for color, alpha, d in SHAPES:
        paths.append(f'    <path android:fillColor="{color}" android:fillAlpha="{alpha}" android:pathData="{d}" />')
    return ('<?xml version="1.0" encoding="utf-8"?>\n'
            '<vector xmlns:android="http://schemas.android.com/apk/res/android"\n'
            '    android:width="108dp"\n'
            '    android:height="108dp"\n'
            '    android:viewportWidth="108"\n'
            '    android:viewportHeight="108">\n'
            + "\n".join(paths) + "\n</vector>\n")


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def main():
    res = os.path.join(ROOT, "packaging", "android", "app", "src", "main", "res")
    write(os.path.join(res, "drawable", "ic_launcher_foreground.xml"), vector_drawable())
    write(os.path.join(res, "values", "ic_launcher_background.xml"),
          '<?xml version="1.0" encoding="utf-8"?>\n<resources>\n'
          f'    <color name="ic_launcher_background">{BACKGROUND}</color>\n</resources>\n')
    adaptive = ('<?xml version="1.0" encoding="utf-8"?>\n'
                '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
                '    <background android:drawable="@color/ic_launcher_background" />\n'
                '    <foreground android:drawable="@drawable/ic_launcher_foreground" />\n'
                '    <monochrome android:drawable="@drawable/ic_launcher_foreground" />\n'
                '</adaptive-icon>\n')
    write(os.path.join(res, "mipmap-anydpi-v26", "ic_launcher.xml"), adaptive)
    write(os.path.join(res, "mipmap-anydpi-v26", "ic_launcher_round.xml"), adaptive)

    try:
        import cairosvg
    except ImportError:
        sys.exit("pip install cairosvg to render the iOS icons")
    ios = os.path.join(ROOT, "packaging", "ios", "icons")
    os.makedirs(ios, exist_ok=True)
    sizes = {
        "AppIcon60x60@2x.png": 120,
        "AppIcon60x60@3x.png": 180,
        "AppIcon76x76@2x~ipad.png": 152,
        "AppIcon83.5x83.5@2x~ipad.png": 167,
        "AppIcon1024.png": 1024,
    }
    for name, px in sizes.items():
        cairosvg.svg2png(bytestring=svg(px).encode(), write_to=os.path.join(ios, name), output_width=px,
                         output_height=px)
    cairosvg.svg2png(bytestring=svg(512).encode(), write_to=os.path.join(ROOT, "packaging", "icon-preview.png"),
                     output_width=512, output_height=512)
    print("icons written")


if __name__ == "__main__":
    main()
