"""Рисует значки Дана: dan.ico (программа, «Пуск») и dan-fail.ico (файлы .dan),
а также SVG-исходники и PNG 512 px. Буквы «Da» переведены в контуры из шрифта Inter
(лицензия SIL OFL), поэтому SVG не зависят от шрифтов на компьютере.

Нужны пакеты: cairosvg, pillow, fonttools; шрифт Inter ExtraBold (путь в FONT).
Запуск из папки поставка/значки: python narisovat.py
"""
import cairosvg, io, struct
from PIL import Image, ImageDraw
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.boundsPen import BoundsPen

FONT = TTFont("/usr/share/fonts/opentype/inter/Inter-ExtraBold.otf")
gs, cmap = FONT.getGlyphSet(), FONT.getBestCmap()

def glyph(ch):
    n = cmap[ord(ch)]; p = SVGPathPen(gs); gs[n].draw(p); b = BoundsPen(gs); gs[n].draw(b)
    return p.getCommands(), gs[n].width, b.bounds

def word(chars, cx, base, cap, gap=-0.02, colors=None):
    """Буквы контурами: высота прописной = cap, центр по x = cx, базовая линия = base."""
    capH = FONT["OS/2"].sCapHeight; k = cap / capH
    gl = [glyph(c) for c in chars]
    total = sum(w for _, w, _ in gl) * k + gap * cap * (len(gl) - 1)
    x = cx - total / 2; out = ""
    for (d, w, _), col in zip(gl, colors):
        out += f'<path transform="translate({x:.2f} {base}) scale({k:.5f} {-k:.5f})" d="{d}" fill="{col}"/>'
        x += w * k + gap * cap
    return out

RUBY = '''<linearGradient id="bg" x1="0" y1="0" x2="1" y2="1">
  <stop offset="0" stop-color="#D3164A"/><stop offset=".55" stop-color="#9B1035"/><stop offset="1" stop-color="#5A0A22"/></linearGradient>'''
GOLD = "#F6C453"

def app_svg(small=False):
    r = 56
    gloss = '' if small else f'''<rect x="8" y="8" width="240" height="240" rx="{r}" fill="url(#gl)"/>'''
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">
<defs>{RUBY}
 <linearGradient id="gl" x1="0" y1="0" x2="0" y2="1">
  <stop offset="0" stop-color="#FFFFFF" stop-opacity=".20"/><stop offset=".5" stop-color="#FFFFFF" stop-opacity="0"/></linearGradient>
</defs>
<rect x="8" y="8" width="240" height="240" rx="{r}" fill="url(#bg)"/>{gloss}
<rect x="9" y="9" width="238" height="238" rx="{r}" fill="none" stroke="#3A0515" stroke-opacity=".45" stroke-width="{4 if small else 2}"/>
{word("Da", 128, 172, 112 if small else 100, colors=("#FFFFFF", GOLD))}
{'' if small else f'<rect x="74" y="190" width="108" height="10" rx="5" fill="{GOLD}" opacity=".9"/>'}
</svg>'''

PAGE = "M52 16 H164 L212 64 V228 a12 12 0 0 1 -12 12 H52 a12 12 0 0 1 -12 -12 V28 a12 12 0 0 1 12 -12 Z"
FOLD = "M164 16 V52 a12 12 0 0 0 12 12 H212 Z"

def file_svg(small=False):
    if small:
        return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256"><defs>{RUBY}</defs>
<path d="{PAGE}" fill="#FFFFFF" stroke="#6E5560" stroke-width="12"/>
<path d="{FOLD}" fill="#F3C9D4" stroke="#6E5560" stroke-width="10" stroke-linejoin="round"/>
<rect x="34" y="104" width="184" height="140" rx="16" fill="url(#bg)"/>
{word("Da", 126, 214, 96, colors=("#FFFFFF", GOLD))}
</svg>'''
    lines = "".join(f'<rect x="64" y="{y}" width="{w}" height="10" rx="5" fill="{c}"/>'
                    for y, w, c in [(48, 70, "#B3163F"), (70, 100, "#D9C3CA"), (92, 56, "#E0A93A")])
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256"><defs>{RUBY}</defs>
<path d="{PAGE}" fill="#FFFFFF" stroke="#9A8790" stroke-width="5"/>
<path d="{FOLD}" fill="#F7DDE4" stroke="#9A8790" stroke-width="5" stroke-linejoin="round"/>
{lines}
<path d="M40 122 H212 V228 a12 12 0 0 1 -12 12 H52 a12 12 0 0 1 -12 -12 Z" fill="url(#bg)"/>
{word("Da", 126, 196, 58, colors=("#FFFFFF", GOLD))}
{word(".dan", 126, 228, 17, gap=0.02, colors=("#F7DDE4",)*4)}
</svg>'''

def png(svg, s):
    return Image.open(io.BytesIO(cairosvg.svg2png(bytestring=svg.encode(), output_width=s, output_height=s))).convert("RGBA")

def ico(path, maker):
    blobs = []
    for s in [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]:
        b = io.BytesIO(); png(maker(small=s <= 32), s).save(b, "PNG"); blobs.append((s, b.getvalue()))
    off = 6 + 16 * len(blobs); dirs = b""; data = b""
    for s, d in blobs:
        dirs += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(d), off + len(data)); data += d
    open(path, "wb").write(struct.pack("<HHH", 0, 1, len(blobs)) + dirs + data)

for name, mk in (("dan", app_svg), ("dan-fail", file_svg)):
    open(f"{name}.svg", "w").write(mk()); open(f"{name}-maly.svg", "w").write(mk(True))
    ico(f"{name}.ico", mk); png(mk(), 512).save(f"{name}-512.png")

W, H = 1400, 560
sheet = Image.new("RGBA", (W, H), "#FFFFFF"); ImageDraw.Draw(sheet).rectangle([0, H//2, W, H], fill="#1F1F1F")
for bgy in (0, H//2):
    x = 30
    for mk in (app_svg, file_svg):
        for s in (256, 96, 48, 32, 24, 16):
            sheet.alpha_composite(png(mk(small=s <= 32), s), (x, bgy + (H//2 - s)//2)); x += s + 18
        x += 30
sheet.save("predprosmotr.png")
