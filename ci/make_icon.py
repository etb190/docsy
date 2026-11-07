# Generates src/docsy.ico and logo.png (run locally: python ci/make_icon.py)
from PIL import Image, ImageDraw, ImageFilter
S = 1024

def gradient(c1, c2, size=S):
    g = Image.new("RGB", (size, size))
    px = g.load()
    for y in range(size):
        for x in range(size):
            t = (x + y) / (2 * (size - 1))
            px[x, y] = tuple(int(c1[i] + (c2[i] - c1[i]) * t) for i in range(3))
    return g

img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
bg = gradient((67, 56, 202), (192, 38, 211))
m = Image.new("L", (S, S), 0)
ImageDraw.Draw(m).rounded_rectangle([40, 40, S - 40, S - 40], radius=230, fill=255)
img.paste(bg, (0, 0), m)

# page shadow
sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
ImageDraw.Draw(sh).polygon([(270, 200), (604, 200), (754, 350), (754, 844), (270, 844)], fill=(20, 10, 60, 110))
sh = sh.filter(ImageFilter.GaussianBlur(22))
img = Image.alpha_composite(img, sh)

d = ImageDraw.Draw(img)
d.polygon([(270, 190), (604, 190), (754, 340), (754, 834), (270, 834)], fill=(255, 255, 255, 255))
d.polygon([(604, 190), (604, 340), (754, 340)], fill=(214, 207, 252, 255))
d.rounded_rectangle([320, 250, 520, 272], radius=11, fill=(205, 200, 235, 255))
d.rounded_rectangle([320, 300, 470, 322], radius=11, fill=(205, 200, 235, 255))

# geometric "D"
dm = Image.new("L", (S, S), 0)
dd = ImageDraw.Draw(dm)
dd.rounded_rectangle([360, 430, 660, 760], radius=165, fill=255)
dd.rectangle([360, 430, 510, 760], fill=255)
dd.rounded_rectangle([430, 500, 590, 690], radius=95, fill=0)
dd.rectangle([430, 500, 520, 690], fill=0)
dg = gradient((67, 56, 202), (192, 38, 211))
img.paste(dg, (0, 0), dm)

img.resize((512, 512), Image.LANCZOS).save("logo.png")
img.save("src/docsy.ico", sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
print("ok")
