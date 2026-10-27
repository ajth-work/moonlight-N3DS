from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


WIDTH = 320
HEIGHT = 240
OUT = Path(__file__).parent
WINDOWS_FONTS = Path(r"C:\Windows\Fonts")


def font(name, size):
    candidates = [
        WINDOWS_FONTS / name,
        Path("/usr/share/fonts/truetype/dejavu") / name.replace("segoeui", "DejaVuSans"),
        Path("/usr/share/fonts/truetype/dejavu") / name.replace("segoeuib", "DejaVuSans-Bold"),
    ]
    for path in candidates:
        try:
            return ImageFont.truetype(str(path), size=size)
        except OSError:
            pass
    return ImageFont.load_default()


FONT_SMALL = font("segoeui.ttf", 10)
FONT_SMALL_BOLD = font("segoeuib.ttf", 10)
FONT_TITLE = font("segoeuib.ttf", 17)
FONT_RESOLUTION = font("segoeuib.ttf", 20)
FONT_RATE = font("segoeuib.ttf", 15)
VERSION = "3.2.0-3d7"


def background():
    image = Image.new("RGB", (WIDTH, HEIGHT))
    pixels = image.load()
    for y in range(HEIGHT):
        t = y / (HEIGHT - 1)
        color = tuple(
            round(a * (1 - t) + b * t)
            for a, b in zip((8, 17, 30), (14, 27, 43))
        )
        for x in range(WIDTH):
            pixels[x, y] = color

    draw = ImageDraw.Draw(image, "RGBA")
    draw.ellipse((236, -78, 390, 76), fill=(47, 116, 113, 20))
    draw.ellipse((-105, 176, 94, 375), fill=(33, 75, 110, 18))
    draw.line((0, 60, WIDTH, 60), fill=(87, 123, 143, 65), width=1)
    draw.line((0, 222, WIDTH, 222), fill=(87, 123, 143, 45), width=1)
    return image


def header(image, subtitle):
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((14, 11, 18, 48), radius=2, fill="#48E5BE")
    draw.text((25, 10), "STREAM PROFILE", font=FONT_SMALL_BOLD, fill="#71F0CF")
    draw.text((25, 25), subtitle, font=FONT_TITLE, fill="#F2F7FA")


def card(draw, bounds, number):
    left, top, right, bottom = bounds
    draw.rounded_rectangle(
        bounds,
        radius=10,
        fill="#132238",
        outline="#29405A",
        width=1,
    )
    draw.text((left + 9, top + 7), number, font=FONT_SMALL_BOLD, fill="#7790A7")
    draw.rounded_rectangle(
        (right - 18, top + 11, right - 9, top + 20),
        radius=4,
        fill="#1F3C50",
    )


def footer(image):
    draw = ImageDraw.Draw(image)
    line = f"Moonlight {VERSION}  \u00b7  MENU TAP PICK"
    draw.text((14, 226), line, font=FONT_SMALL_BOLD, fill="#91A5B8")


def draw_resolution_page():
    image = background()
    header(image, "STEP 1 OF 2  /  RESOLUTION")
    draw = ImageDraw.Draw(image)
    boxes = [
        (14, 75, 154, 142),
        (166, 75, 306, 142),
        (14, 151, 154, 218),
        (166, 151, 306, 218),
    ]
    labels = ["400x240", "800x480", "1200x720", "Custom"]
    for index, (bounds, label) in enumerate(zip(boxes, labels), start=1):
        card(draw, bounds, f"{index:02d}")
        if label == "Custom":
            draw.text(
                (bounds[0] + 22, bounds[1] + 27),
                label,
                font=FONT_RESOLUTION,
                fill="#F2F7FA",
            )
            draw.text(
                (bounds[0] + 23, bounds[1] + 50),
                "MANUAL",
                font=FONT_SMALL_BOLD,
                fill="#71F0CF",
            )
        else:
            draw.text(
                (bounds[0] + 20, bounds[1] + 30),
                label,
                font=FONT_RESOLUTION,
                fill="#F2F7FA",
            )
    footer(image)
    image.save(OUT / "stream_profile_resolution.png")


def fit_rate_font(draw, label, max_width):
    for size in range(15, 11, -1):
        candidate = font("segoeuib.ttf", size)
        if draw.textlength(label, font=candidate) <= max_width:
            return candidate
    return FONT_SMALL_BOLD


def draw_rate_page(resolution, options, filename):
    image = background()
    header(image, f"STEP 2 OF 2  /  RATE  ·  {resolution}")
    draw = ImageDraw.Draw(image)
    boxes = [(14, 83, 306, 142), (14, 153, 306, 212)]
    for index, (bounds, label) in enumerate(zip(boxes, options), start=1):
        card(draw, bounds, f"0{index}")
        label_font = fit_rate_font(draw, label, bounds[2] - bounds[0] - 54)
        draw.text(
            (bounds[0] + 27, bounds[1] + 25),
            label,
            font=label_font,
            fill="#F2F7FA",
        )
    footer(image)
    image.save(OUT / filename)


draw_resolution_page()
draw_rate_page(
    "400x240",
    ["Away — 30 fps · 3000 kbps", "Home light — 30 fps · 5000 kbps"],
    "stream_profile_rate_400.png",
)
draw_rate_page(
    "800x480",
    ["Home sharp — 60 fps · 12000 kbps", "Balanced — 30 fps · 8000 kbps"],
    "stream_profile_rate_800.png",
)
draw_rate_page(
    "1200x720",
    ["Browse — 30 fps · 3000 kbps", "Play — 30 fps · 6000 kbps"],
    "stream_profile_rate_1200.png",
)
