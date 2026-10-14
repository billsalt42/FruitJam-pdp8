#!/usr/bin/env python3
"""
Generate the PiDP-8/I <-> Raspberry Pi Pico adapter board (KiCad 7).

Board: Raspberry Pi HAT outline (65 x 56.5 mm) with the Pi's four M2.5
mounting holes and its 40-pin GPIO header in the Pi's position, so it sits
behind the PiDP-8/I exactly where the Raspberry Pi did.  The Pico (or Pico 2)
is soldered on the BACK of the board, USB connector at the right-hand edge.

Wiring as pidp8-panel/README.md (Pico GPn -> PiDP-8/I matrix line).  Columns
10 and 11 go through three-pad solder jumpers: bridge STD for an unmodified
PiDP-8/I (Pi pins 10/8), or SER for boards with the serial-port mod (pins 5/3).

    python3 make_board.py            -> pidp8-pico-adapter.kicad_pcb (unrouted)
    (route.sh does the routing, pours, DRC and fabrication outputs)
"""
import os
import sys
import pcbnew

MM = pcbnew.FromMM
OX, OY = 100.0, 100.0                  # board origin on the KiCad sheet
W, H = 65.0, 56.5                      # Raspberry Pi HAT size


def P(x, y):
    return pcbnew.VECTOR2I(MM(OX + x), MM(OY + y))


board = pcbnew.BOARD()
ds = board.GetDesignSettings()
ds.SetCopperLayerCount(2)

# ---- design rules: comfortably inside JLCPCB / OSH Park / PCBWay limits
ns = ds.m_NetSettings
default = ns.m_DefaultNetClass
default.SetClearance(MM(0.2))
default.SetTrackWidth(MM(0.25))
default.SetViaDiameter(MM(0.7))
default.SetViaDrill(MM(0.35))
power = pcbnew.NETCLASS("Power")
power.SetClearance(MM(0.2))
power.SetTrackWidth(MM(0.3))
power.SetViaDiameter(MM(0.9))
power.SetViaDrill(MM(0.45))
ns.m_NetClasses["Power"] = power
# (the router gets the Power class for +3V3/+5V/GND through the DSN file: see route.sh)
ds.m_MinClearance = MM(0.2)
ds.m_TrackMinWidth = MM(0.2)
ds.m_HoleToHoleMin = MM(0.25)
ds.m_CopperEdgeClearance = MM(0.5)
ds.m_MinResolvedSpokes = 1

nets = {}


def net(name):
    if name not in nets:
        n = pcbnew.NETINFO_ITEM(board, name)
        board.Add(n)
        nets[name] = n
    return nets[name]


# ---- board outline with 3 mm corner radius
def edge_line(x1, y1, x2, y2):
    s = pcbnew.PCB_SHAPE(board)
    s.SetShape(pcbnew.SHAPE_T_SEGMENT)
    s.SetStart(P(x1, y1)); s.SetEnd(P(x2, y2))
    s.SetLayer(pcbnew.Edge_Cuts); s.SetWidth(MM(0.1))
    board.Add(s)


def edge_arc(cx, cy, sx, sy, angle):
    s = pcbnew.PCB_SHAPE(board)
    s.SetShape(pcbnew.SHAPE_T_ARC)
    s.SetCenter(P(cx, cy)); s.SetStart(P(sx, sy))
    s.SetArcAngleAndEnd(pcbnew.EDA_ANGLE(angle, pcbnew.DEGREES_T), True)
    s.SetLayer(pcbnew.Edge_Cuts); s.SetWidth(MM(0.1))
    board.Add(s)


R = 3.0
edge_line(R, 0, W - R, 0); edge_line(W, R, W, H - R)
edge_line(W - R, H, R, H); edge_line(0, H - R, 0, R)
edge_arc(R, R, 0, R, 90); edge_arc(W - R, R, W - R, 0, 90)
edge_arc(W - R, H - R, W, H - R, 90); edge_arc(R, H - R, R, H, 90)


# ---- footprint helpers
def new_fp(ref, value, x, y, rot=0):
    fp = pcbnew.FOOTPRINT(board)
    fp.SetReference(ref); fp.SetValue(value)
    fp.SetPosition(P(x, y))
    board.Add(fp)
    return fp


def th_pad(fp, num, dx, dy, netname, size=1.7, drill=1.0, square=False):
    p = pcbnew.PAD(fp)
    p.SetNumber(str(num))
    p.SetAttribute(pcbnew.PAD_ATTRIB_PTH)
    p.SetShape(pcbnew.PAD_SHAPE_RECT if square else pcbnew.PAD_SHAPE_CIRCLE)
    p.SetSize(pcbnew.VECTOR2I(MM(size), MM(size)))
    p.SetDrillSize(pcbnew.VECTOR2I(MM(drill), MM(drill)))
    p.SetLayerSet(pcbnew.PAD.PTHMask())
    pos = fp.GetPosition()
    p.SetPos0(pcbnew.VECTOR2I(MM(dx), MM(dy)))
    p.SetPosition(pcbnew.VECTOR2I(pos.x + MM(dx), pos.y + MM(dy)))
    if netname:
        p.SetNet(net(netname))
    fp.Add(p)
    return p


def smd_pad(fp, num, dx, dy, w, h, netname, layer=pcbnew.F_Cu):
    p = pcbnew.PAD(fp)
    p.SetNumber(str(num))
    p.SetAttribute(pcbnew.PAD_ATTRIB_SMD)
    p.SetShape(pcbnew.PAD_SHAPE_RECT)
    p.SetSize(pcbnew.VECTOR2I(MM(w), MM(h)))
    ls = pcbnew.LSET(); ls.AddLayer(layer); ls.AddLayer(pcbnew.F_Mask if layer == pcbnew.F_Cu else pcbnew.B_Mask)
    p.SetLayerSet(ls)
    pos = fp.GetPosition()
    p.SetPos0(pcbnew.VECTOR2I(MM(dx), MM(dy)))
    p.SetPosition(pcbnew.VECTOR2I(pos.x + MM(dx), pos.y + MM(dy)))
    if netname:
        p.SetNet(net(netname))
    fp.Add(p)
    return p


def fp_line(fp, x1, y1, x2, y2, layer, width=0.15):
    s = pcbnew.FP_SHAPE(fp)
    s.SetShape(pcbnew.SHAPE_T_SEGMENT)
    pos = fp.GetPosition()
    s.SetStart(pcbnew.VECTOR2I(pos.x + MM(x1), pos.y + MM(y1)))
    s.SetEnd(pcbnew.VECTOR2I(pos.x + MM(x2), pos.y + MM(y2)))
    s.SetStart0(pcbnew.VECTOR2I(MM(x1), MM(y1)))
    s.SetEnd0(pcbnew.VECTOR2I(MM(x2), MM(y2)))
    s.SetLayer(layer); s.SetWidth(MM(width))
    fp.Add(s)


def text(s, x, y, layer=pcbnew.F_SilkS, size=1.0, mirror=False, bold=False, angle=0):
    t = pcbnew.PCB_TEXT(board)
    t.SetText(s)
    t.SetPosition(P(x, y))
    t.SetLayer(layer)
    t.SetTextSize(pcbnew.VECTOR2I(MM(size), MM(size)))
    t.SetTextThickness(MM(size * (0.2 if bold else 0.15)))
    t.SetMirrored(mirror)
    if angle:
        t.SetTextAngle(pcbnew.EDA_ANGLE(angle, pcbnew.DEGREES_T))
    board.Add(t)


# ---- the Raspberry Pi GPIO connector, in the Pi's position (HAT spec)
HDR_X0, HDR_Y = 8.37, 3.5               # pin 1 x, header centre line
PI_NET = {
    1: "+3V3", 17: "+3V3", 2: "+5V", 4: "+5V",
    6: "GND", 9: "GND", 14: "GND", 20: "GND", 25: "GND", 30: "GND", 34: "GND", 39: "GND",
    33: "COL0", 32: "COL1", 23: "COL2", 19: "COL3", 21: "COL4", 24: "COL5", 26: "COL6",
    31: "COL7", 29: "COL8", 7: "COL9",
    10: "COL10_STD", 5: "COL10_SER", 8: "COL11_STD", 3: "COL11_SER",
    38: "LROW0", 40: "LROW1", 15: "LROW2", 16: "LROW3", 18: "LROW4", 22: "LROW5", 37: "LROW6", 13: "LROW7",
    36: "SROW0", 11: "SROW1", 12: "SROW2",
}
j1 = new_fp("J1", "PiDP-8/I", HDR_X0, HDR_Y)
for k in range(20):
    x = k * 2.54
    th_pad(j1, 2 * k + 1, x, +1.27, PI_NET.get(2 * k + 1), square=(k == 0))   # odd: inner row
    th_pad(j1, 2 * k + 2, x, -1.27, PI_NET.get(2 * k + 2))                    # even: board edge
fp_line(j1, -1.27, -2.54, 49.53, -2.54, pcbnew.F_SilkS)
fp_line(j1, -1.27, 2.54, 49.53, 2.54, pcbnew.F_SilkS)
fp_line(j1, -1.27, -2.54, -1.27, 2.54, pcbnew.F_SilkS)
fp_line(j1, 49.53, -2.54, 49.53, 2.54, pcbnew.F_SilkS)
j1.Reference().SetVisible(False)
j1.Value().SetVisible(False)

# ---- Raspberry Pi Pico / Pico 2, on the back, USB at the right edge
PICO_GP_PIN = {0: 1, 1: 2, 2: 4, 3: 5, 4: 6, 5: 7, 6: 9, 7: 10, 8: 11, 9: 12, 10: 14, 11: 15,
               12: 16, 13: 17, 14: 19, 15: 20, 16: 21, 17: 22, 18: 24, 19: 25, 20: 26, 21: 27, 22: 29,
               26: 31, 27: 32, 28: 34}
STRAP_GP = 28                            # tied to GND on this board: tells the firmware which map
pico_net = {}
for gnd in (3, 8, 13, 18, 23, 28, 33, 38):
    pico_net[gnd] = "GND"
pico_net[PICO_GP_PIN[STRAP_GP]] = "GND"
pico_net[36] = "+3V3"
pico_net[40] = "+5V"                     # VBUS: USB 5 V from the Fruit Jam

PICO_CX, PICO_CY = 25.5 + 0.2, 32.5      # USB connector at the left-hand edge
u1 = new_fp("U1", "Raspberry Pi Pico", PICO_CX, PICO_CY)
# Pico's own frame (top view, USB up): pins 1-20 down the left, 21-40 up the right
for n in range(1, 41):
    if n <= 20:
        px, py = -8.89, -24.13 + (n - 1) * 2.54
    else:
        px, py = 8.89, 24.13 - (n - 21) * 2.54
    th_pad(u1, n, px, py, pico_net.get(n), size=1.5, drill=1.0, square=(n in (3, 8, 13, 18, 23, 28, 33, 38)))
# outline 21 x 51 and the USB connector
for (a, b, c, d) in ((-10.5, -25.5, 10.5, -25.5), (10.5, -25.5, 10.5, 25.5), (10.5, 25.5, -10.5, 25.5),
                     (-10.5, 25.5, -10.5, -25.5), (-4, -25.5, -4, -26.8), (4, -25.5, 4, -26.8), (-4, -26.8, 4, -26.8)):
    fp_line(u1, a, b, c, d, pcbnew.F_SilkS, 0.15)
fp_line(u1, -10.5, -21.5, -7.0, -25.5, pcbnew.F_SilkS, 0.15)       # pin-1 corner bevel
u1.Reference().SetVisible(False)
u1.Value().SetVisible(False)
u1.SetOrientation(pcbnew.EDA_ANGLE(90, pcbnew.DEGREES_T))            # USB end -> -x (left edge)
u1.Flip(u1.GetPosition(), False)                                     # to the back of the board

# ---- which Pico GPIO drives which PiDP-8/I line: chosen to suit the layout.
# Each matrix line goes to the free GPIO pad nearest (left-right) to its header
# pin, preferring the Pico row next to the header, so the tracks hardly cross.
# The result is written to pidp8-panel/board_map.h for the firmware.
JUMPER_X = {"COL10": 6.0, "COL11": 14.0}
targets = {}
for pin, name in PI_NET.items():
    if name.startswith(("COL", "LROW", "SROW")) and not name.endswith("_SER"):
        targets[name.replace("_STD", "")] = HDR_X0 + ((pin - 1) // 2) * 2.54
for k, x in JUMPER_X.items():
    targets[k] = x
pads = {int(p.GetNumber()): (pcbnew.ToMM(p.GetPosition().x) - OX, pcbnew.ToMM(p.GetPosition().y) - OY) for p in u1.Pads()}
free_gp = [g for g in PICO_GP_PIN if g != STRAP_GP]
names = sorted(targets)
try:
    from scipy.optimize import linear_sum_assignment
    import numpy as np
    cost = np.array([[abs(pads[PICO_GP_PIN[g]][0] - targets[n]) + (6.0 if pads[PICO_GP_PIN[g]][1] > PICO_CY else 0.0)
                      for g in free_gp] for n in names])
    rows, cols = linear_sum_assignment(cost)
    assign = {names[r]: free_gp[c] for r, c in zip(rows, cols)}
except ImportError:
    sys.exit("needs scipy (pip install scipy)")
for n, g in assign.items():
    net_name = n + "_PICO" if n in ("COL10", "COL11") else n
    for p in u1.Pads():
        if int(p.GetNumber()) == PICO_GP_PIN[g]:
            p.SetNet(net(net_name))

with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "pidp8-panel", "board_map.h"), "w") as f:
    f.write("/* Generated by hardware/pico-adapter/make_board.py - do not edit.\n"
            " * Pico GPIO for each PiDP-8/I matrix line on the adapter board, which ties\n"
            " * GP%d to ground so the firmware can tell it from hand wiring. */\n" % STRAP_GP)
    f.write("#define BOARD_STRAP_GP %d\n" % STRAP_GP)
    f.write("static const uint8_t board_cols[12]  = { %s };\n" % ", ".join(str(assign["COL%d" % i]) for i in range(12)))
    f.write("static const uint8_t board_lrows[8]  = { %s };\n" % ", ".join(str(assign["LROW%d" % i]) for i in range(8)))
    f.write("static const uint8_t board_srows[3]  = { %s };\n" % ", ".join(str(assign["SROW%d" % i]) for i in range(3)))
print("GPIO map:", ", ".join("%s=GP%d" % (n, assign[n]) for n in sorted(assign, key=lambda n: targets[n])))

# ---- column 10/11 jumpers: centre pad = Pico, STD side = Pi pin 10/8, SER side = pin 5/3
def jumper(ref, x, y, common, std, ser, label):
    jp = new_fp(ref, "STD-SER", x, y)
    smd_pad(jp, 1, -1.1, 0, 0.9, 1.4, std)
    smd_pad(jp, 2, 0, 0, 0.9, 1.4, common)
    smd_pad(jp, 3, 1.1, 0, 0.9, 1.4, ser)
    jp.Reference().SetVisible(False); jp.Value().SetVisible(False)
    text(label, x, y - 2.0, size=0.8)
    text("STD", x - 2.4, y + 2.0, size=0.8)
    text("SER", x + 2.4, y + 2.0, size=0.8)
    fp_line(jp, -1.75, -1.0, 1.75, -1.0, pcbnew.F_SilkS, 0.12)
    fp_line(jp, -1.75, 1.0, 1.75, 1.0, pcbnew.F_SilkS, 0.12)


jumper("JP1", JUMPER_X["COL10"], 13.5, "COL10_PICO", "COL10_STD", "COL10_SER", "COL10")
jumper("JP2", JUMPER_X["COL11"], 13.5, "COL11_PICO", "COL11_STD", "COL11_SER", "COL11")


# ---- mounting holes (Raspberry Pi positions, M2.5)
def hole(ref, x, y):
    fp = new_fp(ref, "M2.5", x, y)
    p = pcbnew.PAD(fp)
    p.SetAttribute(pcbnew.PAD_ATTRIB_NPTH)
    p.SetShape(pcbnew.PAD_SHAPE_CIRCLE)
    p.SetSize(pcbnew.VECTOR2I(MM(2.75), MM(2.75)))
    p.SetDrillSize(pcbnew.VECTOR2I(MM(2.75), MM(2.75)))
    p.SetLayerSet(pcbnew.PAD.UnplatedHoleMask())
    p.SetPos0(pcbnew.VECTOR2I(0, 0))
    p.SetPosition(P(x, y))
    fp.Add(p)
    fp.Reference().SetVisible(False); fp.Value().SetVisible(False)
    # keep copper away from the standoff
    zk = pcbnew.ZONE(board)
    zk.SetIsRuleArea(True)
    zk.SetDoNotAllowCopperPour(True); zk.SetDoNotAllowTracks(True); zk.SetDoNotAllowVias(True)
    zk.SetDoNotAllowPads(False); zk.SetDoNotAllowFootprints(False)
    ls = pcbnew.LSET(); ls.AddLayer(pcbnew.F_Cu); ls.AddLayer(pcbnew.B_Cu)
    zk.SetLayerSet(ls)
    ol = zk.Outline(); ol.NewOutline()
    r = 3.0
    for i in range(16):
        import math
        a = 2 * math.pi * i / 16
        ol.Append(MM(OX + x + r * math.cos(a)), MM(OY + y + r * math.sin(a)))
    board.Add(zk)


for i, (hx, hy) in enumerate(((3.5, 3.5), (61.5, 3.5), (3.5, 52.5), (61.5, 52.5))):
    hole("H%d" % (i + 1), hx, hy)

# ---- silkscreen
text("PiDP-8/I  <->  Raspberry Pi Pico", 32.5, 46.0, size=1.3, bold=True)
text("panel interface adapter  v1.0", 32.5, 48.5, size=0.9)
text("pin 1", HDR_X0, 7.0, size=0.8)
text("J1 plugs into the PiDP-8/I where the Raspberry Pi was", 32.5, 50.9, size=0.8)
text("COL10/11: bridge STD, or SER for a serial-modded PiDP-8/I", 32.5, 53.2, size=0.8)
# back side
text("Raspberry Pi Pico / Pico 2 on this side", 32.5, 18.0, layer=pcbnew.B_SilkS, size=1.0, mirror=True)
text("Fruit Jam <- USB", 8.0, 46.0, layer=pcbnew.B_SilkS, size=0.9, mirror=True)
text("firmware: pidp8_panel_pico*.uf2", 32.5, 49.0, layer=pcbnew.B_SilkS, size=0.8, mirror=True)
text("PDP-8 Fruit Jam project", 32.5, 51.5, layer=pcbnew.B_SilkS, size=0.8, mirror=True)

# ---- ground pours on both layers
for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
    z = pcbnew.ZONE(board)
    z.SetLayer(layer)
    z.SetNet(net("GND"))
    z.SetLocalClearance(MM(0.3))
    z.SetMinThickness(MM(0.25))
    z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
    z.SetThermalReliefGap(MM(0.4))
    z.SetThermalReliefSpokeWidth(MM(0.5))
    ol = z.Outline(); ol.NewOutline()
    for (x, y) in ((0.3, 0.3), (W - 0.3, 0.3), (W - 0.3, H - 0.3), (0.3, H - 0.3)):
        ol.Append(MM(OX + x), MM(OY + y))
    board.Add(z)

out = sys.argv[1] if len(sys.argv) > 1 else "pidp8-pico-adapter.kicad_pcb"
board.Save(out)
print("wrote", out, "nets:", len(nets))
