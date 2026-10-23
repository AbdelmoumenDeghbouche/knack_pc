"""Draws the port's icon (a K, for Knack, on a dark blue field) as .ico and .png. Needs Pillow.
usage: make_icon.py launcher/gow3.ico launcher/gow3.png"""
import sys
from PIL import Image, ImageDraw, ImageFilter

S = 1024  # drawn large, scaled down for every icon size


def draw():
    image = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    mask = Image.new('L', (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle((24, 24, S - 24, S - 24), radius=190, fill=255)

    field = Image.new('RGBA', (S, S))
    fd = ImageDraw.Draw(field)
    for y in range(S):
        t = y / S
        fd.line((0, y, S, y), fill=(int(14 - 8 * t), int(40 - 26 * t), int(96 - 60 * t), 255))

    glow = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((200, 160, 824, 784), fill=(30, 150, 230, 120))
    field.alpha_composite(glow.filter(ImageFilter.GaussianBlur(90)))

    # A bold K: a stem and two arms.
    glyph = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    od = ImageDraw.Draw(glyph)
    ash = (236, 226, 208, 255)
    od.rectangle((250, 200, 400, 824), fill=ash)
    od.polygon([(400, 470), (640, 200), (820, 200), (520, 540), (400, 640)], fill=ash)
    od.polygon([(470, 560), (600, 450), (830, 824), (650, 824)], fill=ash)
    shadow = glyph.copy()
    shadow.putalpha(glyph.getchannel('A').point(lambda a: a * 0.6))
    shadow = Image.composite(Image.new('RGBA', (S, S), (0, 0, 0, 255)), Image.new('RGBA', (S, S), (0, 0, 0, 0)),
                             shadow.getchannel('A')).filter(ImageFilter.GaussianBlur(18))
    field.alpha_composite(shadow, (0, 14))
    field.alpha_composite(glyph)

    image.paste(field, (0, 0), mask)
    border = ImageDraw.Draw(image)
    border.rounded_rectangle((24, 24, S - 24, S - 24), radius=190, outline=(150, 200, 235, 255), width=22)
    return image


big = draw()
big.resize((256, 256), Image.LANCZOS).save(
    sys.argv[1], sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
big.resize((128, 128), Image.LANCZOS).save(sys.argv[2])
