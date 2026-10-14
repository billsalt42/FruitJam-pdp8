#!/usr/bin/env python3
"""
Render realistic previews of the board (green solder mask, gold pads, white
silkscreen) from KiCad's per-layer SVG exports:  preview.png (both sides).
Needs kicad-cli, cairosvg and Pillow.
"""
import io, os, re, subprocess, tempfile
import pcbnew, cairosvg
from PIL import Image, ImageChops, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
PCB = os.path.join(HERE, 'pidp8-pico-adapter.kicad_pcb')
PX_PER_MM = 16

board = pcbnew.LoadBoard(PCB)
bb = board.GetBoundingBox()                 # what kicad-cli uses for "board area"
x0, y0 = pcbnew.ToMM(bb.GetX()), pcbnew.ToMM(bb.GetY())
wmm, hmm = pcbnew.ToMM(bb.GetWidth()), pcbnew.ToMM(bb.GetHeight())
W, H = int(round(wmm * PX_PER_MM)), int(round(hmm * PX_PER_MM))


def layer_mask(layer, mirror):
    """white where the layer has something"""
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, 'l.svg')
        subprocess.check_call(['kicad-cli', 'pcb', 'export', 'svg', '-l', layer, '--black-and-white',
                               '--page-size-mode', '2', '--exclude-drawing-sheet', '--drill-shape-opt', '0',
                               '-o', out, PCB] + (['--mirror'] if mirror else []), stdout=subprocess.DEVNULL)
        svg = open(out).read()
    png = cairosvg.svg2png(bytestring=svg.encode(), output_width=W, output_height=H, background_color='white')
    im = Image.open(io.BytesIO(png)).convert('L').resize((W, H))
    return ImageChops.invert(im).point(lambda v: 255 if v > 80 else 0)


def holes(mirror):
    im = Image.new('L', (W, H), 0)
    d = ImageDraw.Draw(im)
    for fp in board.GetFootprints():
        for p in fp.Pads():
            dr = p.GetDrillSize().x
            if dr <= 0:
                continue
            x = (pcbnew.ToMM(p.GetPosition().x) - x0) * PX_PER_MM
            y = (pcbnew.ToMM(p.GetPosition().y) - y0) * PX_PER_MM
            if mirror:
                x = W - x
            r = pcbnew.ToMM(dr) / 2 * PX_PER_MM
            d.ellipse((x - r, y - r, x + r, y + r), fill=255)
    for t in board.GetTracks():
        if isinstance(t, pcbnew.PCB_VIA):
            x = (pcbnew.ToMM(t.GetPosition().x) - x0) * PX_PER_MM
            y = (pcbnew.ToMM(t.GetPosition().y) - y0) * PX_PER_MM
            if mirror:
                x = W - x
            r = pcbnew.ToMM(t.GetDrillValue()) / 2 * PX_PER_MM
            d.ellipse((x - r, y - r, x + r, y + r), fill=255)
    return im


def side(cu, mask, silk, mirror):
    edge = layer_mask('Edge.Cuts', mirror)
    shape = edge.copy()
    ImageDraw.floodfill(shape, (1, 1), 128)             # outside
    inside = shape.point(lambda v: 0 if v == 128 else 255)
    copper = layer_mask(cu, mirror)
    opening = layer_mask(mask, mirror)
    silkm = layer_mask(silk, mirror)
    hole = holes(mirror)
    img = Image.new('RGB', (W, H), (255, 255, 255))
    img.paste((18, 92, 44), mask=inside)                # FR4 under green mask
    img.paste((40, 125, 60), mask=ImageChops.multiply(copper, inside))
    img.paste((214, 170, 70), mask=ImageChops.multiply(ImageChops.multiply(copper, opening), inside))
    img.paste((240, 240, 235), mask=ImageChops.multiply(silkm, ImageChops.invert(opening)))
    img.paste((40, 40, 40), mask=hole)
    img.paste((150, 150, 150), mask=edge)
    return img


top = side('F.Cu', 'F.Mask', 'F.SilkS', False)
bot = side('B.Cu', 'B.Mask', 'B.SilkS', True)
pad = 30
out = Image.new('RGB', (2 * W + 3 * pad, H + 2 * pad + 50), 'white')
dr = ImageDraw.Draw(out)
try:
    font = ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', 26)
except OSError:
    font = None
dr.text((pad, 15), 'Front: J1 side, faces the PiDP-8/I', fill='black', font=font)
dr.text((2 * pad + W, 15), 'Back: Pico side (seen from behind)', fill='black', font=font)
out.paste(top, (pad, pad + 40))
out.paste(bot, (2 * pad + W, pad + 40))
out.save(os.path.join(HERE, 'preview.png'))
print('preview.png', out.size)
