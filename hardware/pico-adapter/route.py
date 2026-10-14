#!/usr/bin/env python3
"""
Route the adapter board and make the fabrication files.

    python3 route.py [path/to/freerouting.jar]

1. make_board.py         -> unrouted board
2. Specctra DSN export (+3V3/+5V in the Power class)
3. Freerouting 1.9 (headless, via xvfb-run when there is no display) -> SES,
   imported back into the board
4. pour the ground planes, DRC report (drc.rpt), and export gerbers + drill
   files (zipped for JLCPCB / PCBWay / OSH Park), a 1:1 PDF for a fit check,
   preview.png, the schematic, and run check_board.py

Needs KiCad 7 (pcbnew Python module and kicad-cli), Java 17+, freerouting.jar
(FREEROUTING_JAR), scipy, cairosvg and Pillow.
"""
import os, re, subprocess, sys, shutil, zipfile
import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
os.chdir(HERE)
JAR = sys.argv[1] if len(sys.argv) > 1 else os.environ.get('FREEROUTING_JAR', 'freerouting.jar')
PCB = 'pidp8-pico-adapter.kicad_pcb'

def route_once(passes):
    subprocess.check_call([sys.executable, 'make_board.py', PCB])
    board = pcbnew.LoadBoard(PCB)
    pcbnew.ExportSpecctraDSN(board, 'build.dsn')

    # +3V3/+5V go into the (otherwise empty) Power class (wider tracks); GND is
    # routed like a signal so every ground pin is joined, then the pours add to it
    dsn = open('build.dsn').read()
    power = ['+3V3', '+5V']
    m = re.search(r'\(class kicad_default (.*?)\(circuit', dsn, re.S)
    names = [n for n in m.group(1).split() if n not in power]
    dsn = dsn[:m.start(1)] + ' '.join(names) + '\n      ' + dsn[m.end(1):]
    dsn = dsn.replace('(class Power', '(class Power "+3V3" "+5V"', 1)
    open('build.dsn', 'w').write(dsn)

    # Freerouting 1.9 wants a display even in batch mode: use xvfb-run when there is none
    cmd = ['java', '-jar', JAR, '-de', 'build.dsn', '-do', 'build.ses', '-mp', str(passes), '-mt', '1', '-da']
    if not os.environ.get('DISPLAY') and shutil.which('xvfb-run'):
        cmd = ['xvfb-run', '-a'] + cmd
    subprocess.check_call(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

    # --- import the routed session (pcbnew's own importer needs the GUI)
    def sexpr(text):
        tokens = re.findall(r'\(|\)|"[^"]*"|[^\s()]+', text)
        stack, cur = [], []
        for t in tokens:
            if t == '(':
                stack.append(cur); cur = []
            elif t == ')':
                done = cur; cur = stack.pop(); cur.append(done)
            else:
                cur.append(t.strip('"'))
        return cur[0]

    def find(node, key):
        for c in node:
            if isinstance(c, list) and c and c[0] == key:
                yield c

    board = pcbnew.LoadBoard(PCB)
    ses = sexpr(open('build.ses').read())
    routes = next(find(ses, 'routes'))
    res = next(find(routes, 'resolution'))
    scale = {'um': 1e-3, 'mm': 1.0, 'mil': 0.0254}[res[1]] / float(res[2])      # units -> mm
    def pt(x, y):
        return pcbnew.VECTOR2I(pcbnew.FromMM(float(x) * scale), pcbnew.FromMM(-float(y) * scale))
    layers = {'F.Cu': pcbnew.F_Cu, 'B.Cu': pcbnew.B_Cu}
    ntracks = nvias = 0
    for netnode in find(next(find(routes, 'network_out')), 'net'):
        netinfo = board.FindNet(netnode[1])
        for wire in find(netnode, 'wire'):
            path = next(find(wire, 'path'))
            layer, width, xy = layers[path[1]], float(path[2]) * scale, path[3:]
            for i in range(0, len(xy) - 2, 2):
                t = pcbnew.PCB_TRACK(board)
                t.SetStart(pt(xy[i], xy[i + 1])); t.SetEnd(pt(xy[i + 2], xy[i + 3]))
                t.SetWidth(pcbnew.FromMM(width)); t.SetLayer(layer); t.SetNet(netinfo)
                board.Add(t); ntracks += 1
        for via in find(netnode, 'via'):
            m = re.search(r'_(\d+):(\d+)_um', via[1])
            v = pcbnew.PCB_VIA(board)
            v.SetPosition(pt(via[2], via[3]))
            v.SetWidth(pcbnew.FromMM(int(m.group(1)) / 1000)); v.SetDrill(pcbnew.FromMM(int(m.group(2)) / 1000))
            v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu); v.SetNet(netinfo)
            board.Add(v); nvias += 1
    print('imported %d track segments, %d vias' % (ntracks, nvias))
    filler = pcbnew.ZONE_FILLER(board)
    filler.Fill(board.Zones())
    board.Save(PCB)

    # DRC (KiCad 7's kicad-cli has none; the Python API does)
    board = pcbnew.LoadBoard(PCB)
    pcbnew.WriteDRCReport(board, 'drc.rpt', pcbnew.EDA_UNITS_MILLIMETRES, True)
    rpt = open('drc.rpt').read()
    viol = re.search(r'Found (\d+) DRC violations', rpt)
    unconn = re.search(r'Found (\d+) unconnected', rpt)
    print('DRC:', viol.group(0) if viol else '?', '/', unconn.group(0) if unconn else '?')
    return int(unconn.group(1)) if unconn else 999

# Freerouting is not deterministic; try a few pass limits until nothing is left unrouted
for passes in (100, 60, 150, 200, 80, 120, 250, 90):
    if route_once(passes) == 0:
        break
else:
    sys.exit('could not route the board completely')


# fabrication outputs
shutil.rmtree('gerbers', ignore_errors=True)
os.makedirs('gerbers')
layers = 'F.Cu,B.Cu,F.SilkS,B.SilkS,F.Mask,B.Mask,Edge.Cuts'
subprocess.check_call(['kicad-cli', 'pcb', 'export', 'gerbers', '-l', layers, '--subtract-soldermask',
                       '-o', 'gerbers/', PCB], stdout=subprocess.DEVNULL)
subprocess.check_call(['kicad-cli', 'pcb', 'export', 'drill', '--format', 'excellon', '--excellon-separate-th',
                       '--generate-map', '--map-format', 'gerberx2', '-o', 'gerbers/', PCB], stdout=subprocess.DEVNULL)
with zipfile.ZipFile('pidp8-pico-adapter-gerbers.zip', 'w', zipfile.ZIP_DEFLATED) as z:
    for f in sorted(os.listdir('gerbers')):
        z.write(os.path.join('gerbers', f), f)

# previews, schematic, and the wiring cross-check
for script in ('render.py', 'make_schematic.py', 'check_board.py'):
    subprocess.check_call([sys.executable, script], stdout=subprocess.DEVNULL)
subprocess.check_call(['kicad-cli', 'pcb', 'export', 'pdf', '-l', 'F.Cu,F.SilkS,Edge.Cuts,B.SilkS',
                       '-o', 'pidp8-pico-adapter-1to1.pdf', PCB], stdout=subprocess.DEVNULL)
for f in ('build.dsn', 'build.ses', 'preview-top.svg', 'preview-bottom.svg'):
    if os.path.exists(f): os.remove(f)
shutil.rmtree('logs', ignore_errors=True)
print('done')
