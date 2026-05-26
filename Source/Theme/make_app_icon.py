#!/usr/bin/env python3
"""ZynForge Recording app icon. Dark macOS squircle, bold forge-orange Z,
subtle red recording waveform behind it. Rendered at 2x, downscaled."""
import math
from PIL import Image, ImageDraw, ImageFilter

S = 2048                      # work resolution (downscaled to 1024)
OUT = 1024
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

# ---- colours (brand) ----
BG_TOP    = (0x20, 0x20, 0x2b)   # lifted near-black (top)
BG_BOT    = (0x0a, 0x0a, 0x0e)   # deepest (bottom)
ORANGE    = (0xff, 0x8a, 0x3d)   # forge orange (top of Z)
ORANGE_LO = (0xe8, 0x5f, 0x1f)   # forge orange (bottom of Z)
RED       = (0xff, 0x3b, 0x3b)   # record red waveform

# ---- squircle background with vertical gradient ----
margin = int(S * 0.085)
box = (margin, margin, S - margin, S - margin)
radius = int((box[2] - box[0]) * 0.2237)   # Big Sur-ish corner

# gradient fill
grad = Image.new("RGBA", (S, S), (0, 0, 0, 0))
gd = ImageDraw.Draw(grad)
for y in range(S):
    t = y / S
    r = int(BG_TOP[0] + (BG_BOT[0] - BG_TOP[0]) * t)
    g = int(BG_TOP[1] + (BG_BOT[1] - BG_TOP[1]) * t)
    b = int(BG_TOP[2] + (BG_BOT[2] - BG_TOP[2]) * t)
    gd.line([(0, y), (S, y)], fill=(r, g, b, 255))

mask = Image.new("L", (S, S), 0)
md = ImageDraw.Draw(mask)
md.rounded_rectangle(box, radius=radius, fill=255)
img.paste(grad, (0, 0), mask)

# ---- subtle top gloss (specular sheen) ----
gloss = Image.new("RGBA", (S, S), (0, 0, 0, 0))
gld = ImageDraw.Draw(gloss)
gh = int((box[3] - box[1]) * 0.46)
gld.rounded_rectangle((box[0], box[1], box[2], box[1] + gh),
                      radius=radius, fill=(255, 255, 255, 26))
gloss = gloss.filter(ImageFilter.GaussianBlur(40))
img = Image.alpha_composite(img, Image.composite(gloss, Image.new("RGBA", (S, S), (0,0,0,0)), mask))

# ---- red recording waveform behind the Z ----
wave = Image.new("RGBA", (S, S), (0, 0, 0, 0))
wd = ImageDraw.Draw(wave)
cy = S // 2
wx0, wx1 = int(S * 0.20), int(S * 0.80)
amps = []
for i in range(wx1 - wx0):
    x = i / (wx1 - wx0)
    # audio-ish envelope: a few overlapping sines, tapered at the edges
    env = math.sin(math.pi * x) ** 0.6
    v = (math.sin(x * 38) * 0.5 + math.sin(x * 91 + 1) * 0.3
         + math.sin(x * 17 + 2) * 0.4)
    amps.append(v * env)
peak = max(abs(a) for a in amps) or 1.0
H = int(S * 0.14)
for i, a in enumerate(amps):
    x = wx0 + i
    h = int(a / peak * H)
    wd.line([(x, cy - h), (x, cy + h)], fill=(RED[0], RED[1], RED[2], 150), width=3)
wave = wave.filter(ImageFilter.GaussianBlur(1.5))
img = Image.alpha_composite(img, Image.composite(wave, Image.new("RGBA",(S,S),(0,0,0,0)), mask))

# ---- bold geometric "Z" with vertical gradient ----
# Build the Z as a filled polygon (top bar, diagonal, bottom bar).
cx = S // 2
zw = int(S * 0.46)          # Z width
zh = int(S * 0.50)          # Z height
bar = int(zh * 0.20)        # bar thickness
diag = int(zw * 0.30)       # diagonal thickness (horizontal run)
x0, x1 = cx - zw // 2, cx + zw // 2
y0, y1 = cy - zh // 2, cy + zh // 2

zmask = Image.new("L", (S, S), 0)
zmd = ImageDraw.Draw(zmask)
# top bar, bottom bar
zmd.polygon([(x0, y0), (x1, y0), (x1, y0 + bar), (x0, y0 + bar)], fill=255)
zmd.polygon([(x0, y1 - bar), (x1, y1 - bar), (x1, y1), (x0, y1)], fill=255)
# diagonal (top-right to bottom-left)
zmd.polygon([(x1, y0 + bar), (x1 - diag, y0 + bar),
             (x0 + diag, y1 - bar), (x0, y1 - bar)], fill=255)

# orange vertical gradient for the Z
zgrad = Image.new("RGBA", (S, S), (0, 0, 0, 0))
zgd = ImageDraw.Draw(zgrad)
for y in range(y0, y1 + 1):
    t = (y - y0) / max(1, (y1 - y0))
    r = int(ORANGE[0] + (ORANGE_LO[0] - ORANGE[0]) * t)
    g = int(ORANGE[1] + (ORANGE_LO[1] - ORANGE[1]) * t)
    b = int(ORANGE[2] + (ORANGE_LO[2] - ORANGE[2]) * t)
    zgd.line([(0, y), (S, y)], fill=(r, g, b, 255))

# soft drop shadow under the Z for depth
shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
shadow.paste((0, 0, 0, 130), (0, 0), zmask)
shadow = shadow.filter(ImageFilter.GaussianBlur(22))
img = Image.alpha_composite(img, Image.composite(shadow,
        Image.new("RGBA",(S,S),(0,0,0,0)),
        mask))  # keep shadow inside the squircle
img.paste(zgrad, (0, 0), zmask)

# ---- thin inner edge stroke for crisp definition ----
edge = Image.new("RGBA", (S, S), (0, 0, 0, 0))
ed = ImageDraw.Draw(edge)
ed.rounded_rectangle(box, radius=radius, outline=(255, 255, 255, 40), width=3)
img = Image.alpha_composite(img, edge)

# ---- downscale for anti-aliasing ----
img = img.resize((OUT, OUT), Image.LANCZOS)
img.save("/tmp/zynforge_logo.png")
print("wrote /tmp/zynforge_logo.png")
