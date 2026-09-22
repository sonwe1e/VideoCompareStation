"""Generate CompareStation branding icons (PNG + multi-size ICO)."""

from pathlib import Path

from PIL import Image, ImageDraw


def main() -> None:
    size = 1024
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    bg = (11, 18, 32, 255)
    cyan = (56, 189, 248, 255)
    amber = (245, 158, 11, 255)
    white = (255, 255, 255, 255)
    ink = (8, 12, 22, 255)

    draw.rounded_rectangle((48, 48, size - 48, size - 48), radius=180, fill=bg)

    pad_x = 150
    pad_y = 170
    gap = 28
    panel_w = (size - 2 * pad_x - gap) // 2
    panel_h = size - 2 * pad_y
    left = (pad_x, pad_y, pad_x + panel_w, pad_y + panel_h)
    right = (
        pad_x + panel_w + gap,
        pad_y,
        pad_x + 2 * panel_w + gap,
        pad_y + panel_h,
    )
    draw.rounded_rectangle(left, radius=36, fill=cyan)
    draw.rounded_rectangle(right, radius=36, fill=amber)

    lx0, ly0, lx1, ly1 = left
    rx0, ry0, rx1, ry1 = right
    base_l = ly1 - 48
    base_r = ry1 - 48

    draw.polygon(
        [(lx0 + 52, base_l), ((lx0 + lx1) // 2 - 20, ly0 + 110), (lx1 - 52, base_l)],
        fill=ink,
    )
    draw.polygon(
        [(lx0 + 52, base_l), (lx0 + 140, ly0 + 210), (lx0 + 250, base_l)],
        fill=ink,
    )
    draw.polygon(
        [(rx0 + 52, base_r), ((rx0 + rx1) // 2 + 28, ry0 + 88), (rx1 - 52, base_r)],
        fill=ink,
    )
    draw.polygon(
        [(rx0 + 52, base_r), (rx0 + 130, ry0 + 190), (rx0 + 240, base_r)],
        fill=ink,
    )

    cx = size // 2
    draw.line([(cx, 140), (cx, size - 140)], fill=white, width=14)
    radius = 58
    draw.ellipse((cx - radius, cx - radius, cx + radius, cx + radius), fill=white)
    draw.ellipse((cx - 26, cx - 26, cx + 26, cx + 26), fill=bg)

    out_dir = Path("assets/branding")
    out_dir.mkdir(parents=True, exist_ok=True)

    png_path = out_dir / "comparestation-icon.png"
    img.save(png_path, "PNG")
    print(f"PNG {png_path} {img.size}")

    ico_path = out_dir / "comparestation.ico"
    sizes = [16, 24, 32, 48, 64, 128, 256]
    img.save(ico_path, format="ICO", sizes=[(s, s) for s in sizes])
    print(f"ICO {ico_path} {sizes}")


if __name__ == "__main__":
    main()
