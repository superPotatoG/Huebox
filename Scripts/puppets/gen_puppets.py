"""
Shadow puppets (皮影) for Huebox: the dancer (旦) and her partner (生).

Every puppet is a set of flat leather pieces joined at pivots, the way a real 皮影 figure is cut and tied:
skirt / robe, upper body, head, arms, water sleeves, legs and a few dangling ornaments. Each piece has
  - leather: translucent dyed hide (vertex colour = dye) and black ink lines (outline, eye, brow),
  - carving: the cut-out openings (glow). In the game the light coming through them is her current colour.

Coordinates are centimetres, the puppet faces +X, Z is up, the feet stand on Z = 0. Shapes are written in the
rest pose (arms and legs hanging straight down); the game poses and animates the pieces around their pivots.

Run with the engine's Python (no packages needed):
  "D:/Epic Games/UE_5.8/Engine/Binaries/ThirdParty/Python3/Win64/python.exe" Scripts/puppets/gen_puppets.py
      [--png <preview.png> [--zoom <closeup.png>]]
Writes Source/Huebox/Rig/HuePuppetShapes.inl (do not edit that file by hand).
"""
import math
import os
import sys

# ---------------------------------------------------------------------------------------------------------------
# Geometry helpers
# ---------------------------------------------------------------------------------------------------------------

def spline(ctrl, closed=True, n=6):
    """Catmull-Rom curve through the control points (n samples per span)."""
    pts = []
    m = len(ctrl)
    spans = range(m) if closed else range(m - 1)
    for i in spans:
        p0 = ctrl[(i - 1) % m] if closed else ctrl[max(i - 1, 0)]
        p1 = ctrl[i]
        p2 = ctrl[(i + 1) % m] if closed else ctrl[i + 1]
        p3 = ctrl[(i + 2) % m] if closed else ctrl[min(i + 2, m - 1)]
        for k in range(n):
            t = k / n
            t2, t3 = t * t, t * t * t
            pts.append(tuple(
                0.5 * (2 * p1[c] + (-p0[c] + p2[c]) * t + (2 * p0[c] - 5 * p1[c] + 4 * p2[c] - p3[c]) * t2
                       + (-p0[c] + 3 * p1[c] - 3 * p2[c] + p3[c]) * t3) for c in (0, 1)))
    if not closed:
        pts.append(tuple(ctrl[-1]))
    return pts


def ellipse(cx, cz, rx, rz, rot=0.0, n=14):
    c, s = math.cos(math.radians(rot)), math.sin(math.radians(rot))
    pts = []
    for i in range(n):
        a = 2 * math.pi * i / n
        x, z = rx * math.cos(a), rz * math.sin(a)
        pts.append((cx + x * c - z * s, cz + x * s + z * c))
    return pts


def circle(cx, cz, r, n=12):
    return ellipse(cx, cz, r, r, 0.0, n)


def spiral(cx, cz, r, turns=1.6, start=0.0, n=28, cw=False):
    """Cloud-scroll curl: radius shrinks towards the centre."""
    pts = []
    for i in range(n + 1):
        t = i / n
        a = math.radians(start) + (-1 if cw else 1) * t * turns * 2 * math.pi
        rr = r * (1.0 - 0.82 * t)
        pts.append((cx + rr * math.cos(a), cz + rr * math.sin(a)))
    return pts


def offset(pts, dx, dz):
    return [(x + dx, z + dz) for x, z in pts]


def signed_area(poly):
    a = 0.0
    for i in range(len(poly)):
        x0, z0 = poly[i]
        x1, z1 = poly[(i + 1) % len(poly)]
        a += x0 * z1 - x1 * z0
    return 0.5 * a


def point_in_poly(pt, poly):
    x, z = pt
    inside = False
    for i in range(len(poly)):
        x0, z0 = poly[i]
        x1, z1 = poly[(i + 1) % len(poly)]
        if (z0 > z) != (z1 > z):
            if x < x0 + (z - z0) * (x1 - x0) / (z1 - z0):
                inside = not inside
    return inside


def _cross(a, b, c):
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])


def _in_tri(p, a, b, c):
    d1, d2, d3 = _cross(a, b, p), _cross(b, c, p), _cross(c, a, p)
    return d1 >= -1e-9 and d2 >= -1e-9 and d3 >= -1e-9


def triangulate(poly):
    """Ear clipping of a simple polygon. Returns triangles as point triples (counter-clockwise)."""
    pts = []
    for p in poly:
        if not pts or abs(p[0] - pts[-1][0]) + abs(p[1] - pts[-1][1]) > 1e-6:
            pts.append(p)
    if len(pts) > 1 and abs(pts[0][0] - pts[-1][0]) + abs(pts[0][1] - pts[-1][1]) < 1e-6:
        pts.pop()
    if signed_area(pts) < 0:
        pts.reverse()
    idx = list(range(len(pts)))
    tris = []
    guard = 0
    while len(idx) > 3 and guard < 20000:
        guard += 1
        clipped = False
        m = len(idx)
        for i in range(m):
            a, b, c = idx[i - 1], idx[i], idx[(i + 1) % m]
            if _cross(pts[a], pts[b], pts[c]) <= 1e-9:
                continue
            if any(_in_tri(pts[j], pts[a], pts[b], pts[c]) for j in idx if j not in (a, b, c)):
                continue
            tris.append((pts[a], pts[b], pts[c]))
            del idx[i]
            clipped = True
            break
        if not clipped:
            # Degenerate leftovers (collinear runs): drop the flattest vertex and go on.
            flat = min(range(m), key=lambda i: abs(_cross(pts[idx[i - 1]], pts[idx[i]], pts[idx[(i + 1) % m]])))
            del idx[flat]
    if len(idx) == 3:
        a, b, c = idx
        if _cross(pts[a], pts[b], pts[c]) > 1e-9:
            tris.append((pts[a], pts[b], pts[c]))
    return tris


def stroke_tris(pts, w, closed=False):
    """A line of width w as one mitred strip (two vertices per point, shared by the neighbouring segments)."""
    h = 0.5 * w
    p = []
    for q in pts:
        if not p or math.hypot(q[0] - p[-1][0], q[1] - p[-1][1]) > 1e-4:
            p.append(q)
    if closed and len(p) > 2 and math.hypot(p[0][0] - p[-1][0], p[0][1] - p[-1][1]) < 1e-4:
        p.pop()
    n = len(p)
    if n < 2:
        return []

    def normal(a, b):
        dx, dz = b[0] - a[0], b[1] - a[1]
        length = math.hypot(dx, dz) or 1e-9
        return (-dz / length, dx / length)

    left, right = [], []
    for i in range(n):
        if closed:
            n0, n1 = normal(p[i - 1], p[i]), normal(p[i], p[(i + 1) % n])
        else:
            n0 = normal(p[i - 1], p[i]) if i > 0 else normal(p[0], p[1])
            n1 = normal(p[i], p[i + 1]) if i < n - 1 else n0
        mx, mz = n0[0] + n1[0], n0[1] + n1[1]
        length = math.hypot(mx, mz)
        if length < 1e-6:
            mx, mz = n0
        else:
            mx, mz = mx / length, mz / length
        # Mitre length h / cos(half angle), capped at 2h for sharp turns.
        scale = h / max(mx * n1[0] + mz * n1[1], 0.5)
        left.append((p[i][0] + mx * scale, p[i][1] + mz * scale))
        right.append((p[i][0] - mx * scale, p[i][1] - mz * scale))
    tris = []
    for i in (range(n) if closed else range(n - 1)):
        j = (i + 1) % n
        tris += [(right[i], right[j], left[j]), (right[i], left[j], left[i])]
    return tris

# ---------------------------------------------------------------------------------------------------------------
# Puppet description
# ---------------------------------------------------------------------------------------------------------------

LEATHER, GLOW = 0, 1
INK = (0.0, 0.0, 0.0)

# Depth inside a piece (cm towards the viewer): dyed hide, then the openings, then ink on top.
DEPTH_FILL, DEPTH_GLOW, DEPTH_INK = 0.0, 0.035, 0.07


class Piece:
    def __init__(self, name, parent, pivot, layer):
        self.name, self.parent, self.pivot, self.layer = name, parent, pivot, layer
        self.tris = []      # (section, depth, colour, (p0, p1, p2)) in rest-pose coordinates

    def fill(self, poly, dye, depth=DEPTH_FILL):
        for t in triangulate(poly):
            self.tris.append((LEATHER, depth, dye, t))

    def ink(self, pts, w=0.5, closed=False, depth=DEPTH_INK):
        for t in stroke_tris(pts, w, closed):
            self.tris.append((LEATHER, depth, INK, t))

    def outline(self, poly, w=0.7):
        self.ink(poly, w, closed=True)

    def glow(self, poly, weight=1.0, depth=DEPTH_GLOW):
        for t in triangulate(poly):
            self.tris.append((GLOW, depth, (weight, weight, weight), t))

    def glow_line(self, pts, w=0.5, weight=1.0, closed=False, depth=DEPTH_GLOW):
        for t in stroke_tris(pts, w, closed):
            self.tris.append((GLOW, depth, (weight, weight, weight), t))

    # Carved motifs --------------------------------------------------------------------------------------------
    def cloud(self, cx, cz, r, cw=False, w=0.45, start=0.0):
        self.glow_line(spiral(cx, cz, r, 1.5, start, 26, cw), w)

    def flower(self, cx, cz, r, petals=6, weight=1.0, rot=0.0):
        for i in range(petals):
            a = rot + 360.0 * i / petals
            ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
            self.glow(ellipse(cx + ca * r * 0.55, cz + sa * r * 0.55, r * 0.45, r * 0.2, a, 10), weight)
        self.ink(circle(cx, cz, r * 0.16, 8), 0.3, closed=True)

    def coins(self, x0, x1, z, r, count):
        for i in range(count):
            x = x0 + (x1 - x0) * (i + 0.5) / count
            self.glow(circle(x, z, r, 10))
            self.ink(circle(x, z, r * 0.35, 6), 0.25, closed=True)

    def zigzag(self, x0, x1, z0, z1, teeth, w=0.45):
        pts = []
        for i in range(teeth * 2 + 1):
            pts.append((x0 + (x1 - x0) * i / (teeth * 2), z0 if i % 2 == 0 else z1))
        self.glow_line(pts, w)

    def motif(self, kind, x, z, s, weight=1.0):
        if kind == "diamond":
            self.glow([(x, z + s), (x + s * 0.62, z), (x, z - s), (x - s * 0.62, z)], weight)
        elif kind == "dot":
            self.glow(circle(x, z, s * 0.55, 8), weight)
        elif kind == "coin":
            self.glow(circle(x, z, s, 10), weight)
            self.fill([(x, z + s * 0.42), (x + s * 0.42, z), (x, z - s * 0.42), (x - s * 0.42, z)], INK, DEPTH_INK)
        elif kind == "petal4":
            for a in (45, 135, 225, 315):
                ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
                self.glow(ellipse(x + ca * s * 0.5, z + sa * s * 0.5, s * 0.5, s * 0.24, a, 8), weight)
        elif kind == "scale":
            self.glow(ellipse(x, z - s * 0.2, s * 0.8, s * 0.55, 0, 10), weight)

    def lace(self, region, kind, step, size, margin=0.9, zmin=-1e9, zmax=1e9, weight=1.0, stagger=True):
        """Fills a region with a carved pattern: one motif per lattice point that keeps clear of the region's edge."""
        xs, zs = [p[0] for p in region], [p[1] for p in region]
        reach = size + margin
        row = 0
        z = min(zs) + step * 0.5
        while z < max(zs):
            x = min(xs) + step * 0.5 + (step * 0.5 if stagger and row % 2 else 0.0)
            while x < max(xs):
                if zmin <= z <= zmax and all(point_in_poly((x + dx, z + dz), region)
                                             for dx, dz in ((0, 0), (reach, 0), (-reach, 0), (0, reach), (0, -reach))):
                    self.motif(kind, x, z, size, weight)
                x += step
            z += step * (0.87 if stagger else 1.0)
            row += 1

    def knot(self, x, z, r=1.15):
        """The tied joint of a 皮影 figure: a leather washer with a hole."""
        self.fill(circle(x, z, r, 12), INK, DEPTH_INK + 0.01)
        self.glow(circle(x, z, r * 0.42, 8), 1.0, DEPTH_INK + 0.02)


class Puppet:
    def __init__(self, name):
        self.name = name
        self.pieces = []
        self.anchors = []   # (name, piece index, x, z) in rest-pose coordinates

    def piece(self, name, parent, pivot, layer):
        p = Piece(name, parent, pivot, layer)
        self.pieces.append(p)
        return p

    def index(self, name):
        return next(i for i, p in enumerate(self.pieces) if p.name == name)

    def anchor(self, name, piece, x, z):
        self.anchors.append((name, self.index(piece), x, z))


# Dyes (relative; the game multiplies them by the light coming through the screen).
# 1.0 = untouched thin hide (lets the most light through); dyed hide transmits less.
SKIRT = (0.62, 0.19, 0.10)       # madder red
ROBE_TOP = (0.12, 0.40, 0.28)    # malachite green
TRIM = (0.68, 0.46, 0.12)        # ochre / gold
SLEEVE = (1.0, 0.94, 0.82)       # thin pale hide (water sleeves)
SKIN = (0.95, 0.80, 0.60)
TROUSERS = (0.16, 0.22, 0.42)
SHOE = (0.60, 0.16, 0.09)
HAIR = (0.025, 0.02, 0.02)
SCHOLAR_ROBE = (0.16, 0.27, 0.56)  # indigo
SCHOLAR_CAP = (0.045, 0.045, 0.06)
BOOT = (0.06, 0.06, 0.06)
FAN = (0.95, 0.82, 0.55)


def band_between(c1, c2):
    """Closed polygon between two curves running the same way."""
    return list(c1) + list(reversed(c2))


def add_arm(pup, side, sx, shoulder_z, layer, dye, wide=False):
    """Upper arm and forearm (with hand), hanging straight down from the shoulder (sx, shoulder_z)."""
    sz = shoulder_z
    ez = sz - 16.5     # elbow
    wz = ez - 14.5     # wrist
    upper = pup.piece("arm_upper_" + side, pup.index("upper"), (sx, sz), layer)
    up = spline([(sx - 4.4, sz + 1.8), (sx + 4.0, sz + 1.6), (sx + 3.4, sz - 8), (sx + 2.8, ez - 0.8),
                 (sx - 2.8, ez - 1.0), (sx - 3.8, sz - 8)], True, 5)
    upper.fill(up, dye)
    upper.lace(up, "dot", 2.3, 0.75, 0.7, zmin=ez + 4.6, zmax=sz - 2.4)
    band = [(sx - 3.2, ez + 0.4), (sx + 3.0, ez + 0.6), (sx + 3.1, ez + 3.6), (sx - 3.5, ez + 3.4)]
    upper.fill(band, TRIM, DEPTH_FILL + 0.01)
    upper.zigzag(sx - 2.6, sx + 2.5, ez + 1.2, ez + 2.9, 3, 0.55)
    upper.outline(band, 0.45)
    upper.outline(up, 0.8)
    upper.knot(sx, sz - 0.4)

    fore = pup.piece("arm_fore_" + side, pup.index("arm_upper_" + side), (sx, ez), layer + 0.2)
    if wide:
        # Scholar: a wide hanging sleeve.
        fp = spline([(sx - 3.2, ez + 1.8), (sx + 3.2, ez + 1.8), (sx + 5.0, ez - 7), (sx + 7.0, wz - 1.4),
                     (sx + 3.4, wz - 3.8), (sx - 2.0, wz - 3.0), (sx - 6.2, wz - 0.8), (sx - 4.8, ez - 8)], True, 5)
        fore.fill(fp, dye)
        fore.lace(fp, "scale", 2.7, 1.0, 0.8, zmin=wz + 2.5)
        hem = spline([(sx - 5.6, wz + 0.2), (sx - 1.4, wz - 1.8), (sx + 3.6, wz - 2.0), (sx + 6.4, wz - 0.6)], False, 5)
        fore.fill(band_between(hem, offset(hem, 0.3, 2.4)), TRIM, DEPTH_FILL + 0.01)
        fore.glow_line(offset(hem, 0.15, 1.2), 0.6)
        fore.outline(fp, 0.8)
    else:
        fp = spline([(sx - 3.0, ez + 1.8), (sx + 3.0, ez + 1.8), (sx + 2.6, ez - 7.5), (sx + 4.0, wz + 0.2),
                     (sx - 4.0, wz + 0.2), (sx - 2.6, ez - 7.5)], True, 5)
        fore.fill(fp, dye)
        fore.lace(fp, "dot", 2.3, 0.75, 0.7, zmin=wz + 4.6)
        cuff = [(sx - 3.9, wz + 0.3), (sx + 3.9, wz + 0.3), (sx + 3.3, wz + 3.6), (sx - 3.3, wz + 3.6)]
        fore.fill(cuff, TRIM, DEPTH_FILL + 0.01)
        fore.coins(sx - 3.2, sx + 3.2, wz + 1.95, 0.8, 3)
        fore.outline(cuff, 0.45)
        fore.outline(fp, 0.8)
    fore.knot(sx, ez)
    # Orchid hand (兰花指): palm with the index finger pointing ahead.
    hz = wz - (2.4 if wide else 0.2)
    hp = spline([(sx - 1.5, hz + 0.6), (sx + 1.9, hz + 0.6), (sx + 2.8, hz - 1.0), (sx + 5.4, hz - 1.4),
                 (sx + 2.8, hz - 2.6), (sx + 2.2, hz - 4.6), (sx + 0.2, hz - 5.0), (sx - 1.5, hz - 3.0)], True, 3)
    fore.fill(hp, SKIN, DEPTH_FILL - 0.02)
    fore.ink(spline([(sx + 0.4, hz - 2.0), (sx + 1.6, hz - 3.2), (sx + 2.0, hz - 4.4)], False, 3), 0.3)
    fore.outline(hp, 0.45)
    pup.anchor("hand_" + side, "arm_fore_" + side, sx + 0.4, hz - 2.2)
    return wz


def add_water_sleeve(pup, side, sx, wz, layer):
    sleeve = pup.piece("sleeve_" + side, pup.index("arm_fore_" + side), (sx, wz + 0.8), layer)
    sp = spline([(sx - 3.6, wz + 1.0), (sx + 3.6, wz + 1.0), (sx + 5.2, wz - 9), (sx + 4.0, wz - 17),
                 (sx + 5.8, wz - 25), (sx + 3.0, wz - 29), (sx - 0.4, wz - 26.5), (sx - 3.4, wz - 29.6),
                 (sx - 5.4, wz - 24), (sx - 3.4, wz - 16), (sx - 5.0, wz - 8)], True, 5)
    sleeve.fill(sp, SLEEVE)
    # Thin silk: mostly cut away into waves, so the thrown sleeve flashes in her colour.
    for z in (wz - 4.5, wz - 10.0, wz - 15.5, wz - 21.0):
        wave = spline([(sx - 3.8, z), (sx - 1.3, z + 1.3), (sx + 1.3, z - 0.7), (sx + 4.2, z + 0.9)], False, 5)
        sleeve.glow(band_between(wave, offset(wave, 0.0, -2.4)))
        sleeve.ink(offset(wave, 0.0, -1.2), 0.35)
    sleeve.outline(sp, 0.7)


def add_leg(pup, side, lx, hip_z, layer, trouser_dye, boot=False):
    leg = pup.piece("leg_" + side, pup.index("lower"), (lx, hip_z), layer)
    lp = [(lx - 4.4, hip_z + 2), (lx + 4.0, hip_z + 2), (lx + 3.4, 30), (lx + 2.6, 9), (lx + 2.9, 3.5),
          (lx - 3.4, 3.3), (lx - 2.6, 9), (lx - 3.6, 30)]
    leg.fill(lp, trouser_dye)
    leg.outline(lp, 0.7)
    if boot:
        bp = spline([(lx - 3.8, 22), (lx + 3.8, 22), (lx + 3.6, 6.5), (lx + 9.0, 4.8), (lx + 10.6, 2.6),
                     (lx + 9.8, 1.0), (lx - 3.8, 1.0)], True, 4)
        leg.fill(bp, BOOT, DEPTH_FILL + 0.01)
        leg.glow([(lx - 3.8, 0.0), (lx + 10.0, 0.0), (lx + 10.2, 1.5), (lx - 3.8, 1.5)])
        leg.glow_line([(lx - 3.2, 17), (lx + 3.2, 17)], 0.7)
        leg.glow(circle(lx + 0.2, 11.0, 1.0, 10))
        leg.outline(bp, 0.6)
    else:
        sp = spline([(lx - 3.4, 3.8), (lx + 2.9, 4.0), (lx + 6.2, 3.0), (lx + 9.8, 5.0), (lx + 9.2, 1.4),
                     (lx + 5.0, 0.0), (lx - 3.2, 0.0)], True, 4)
        leg.fill(sp, SHOE, DEPTH_FILL + 0.01)
        leg.glow(circle(lx + 8.8, 3.2, 0.8, 8))
        leg.glow_line([(lx - 2.6, 9.5), (lx + 2.6, 9.5)], 0.7)
        leg.outline(sp, 0.6)
    pup.anchor("foot_" + side, "leg_" + side, lx + 3.0, 0.0)


def build_dancer():
    pup = Puppet("Dancer")
    WAIST = (0.0, 62.0)
    NECK = (1.0, 96.5)

    # ---- Lower body: the skirt (root piece) ---------------------------------------------------------------
    lower = pup.piece("lower", -1, WAIST, 0.0)
    hem_ctrl = [(-23, 18.5), (-17, 17.2), (-10, 18.2), (-3, 16.8), (4, 18.2), (11, 16.8), (17, 17.8), (21, 19.6)]
    skirt = spline([(-8.0, 64), (-9.0, 56), (-11.5, 46), (-15, 34), (-19, 24)] + hem_ctrl +
                   [(16, 27), (12, 38), (9.5, 50), (8.0, 58), (7.5, 64)], True, 5)
    lower.fill(skirt, SKIRT)
    lower.lace(skirt, "petal4", 4.3, 1.55, 0.9, zmin=27.5, zmax=51.0)
    for x0, x1 in ((-5.0, -13.0), (-1.4, -4.6), (2.2, 3.8), (5.2, 11.0)):
        lower.ink(spline([(x0, 54), ((x0 + x1) * 0.5 + 0.5, 39), (x1, 25)], False, 6), 0.5)
    hem = spline(hem_ctrl, False, 5)
    hem_band = band_between(hem, [(x * 0.88, z + 5.6) for x, z in hem])
    lower.fill(hem_band, TRIM, DEPTH_FILL + 0.01)
    for i, x in enumerate((-16.0, -11.0, -6.0, -1.0, 4.0, 9.0, 14.0)):
        lower.cloud(x, 20.6, 1.85, cw=(i % 2 == 1), w=0.6, start=90.0)
    lower.ink([(x * 0.88, z + 5.6) for x, z in hem], 0.5)
    sash = [(-9.1, 54.2), (8.7, 54.2), (8.3, 59.8), (-8.7, 59.8)]
    lower.fill(sash, TRIM, DEPTH_FILL + 0.02)
    lower.coins(-8.0, 7.6, 57.0, 1.1, 6)
    lower.outline(sash, 0.5)
    lower.outline(skirt, 0.85)

    # ---- Legs (behind the skirt; only the shoes show below the hem) ------------------------------------------
    add_leg(pup, "far", -2.0, 54.0, -3.0, TROUSERS)
    add_leg(pup, "near", 1.5, 54.0, -1.8, TROUSERS)

    # ---- Sash ribbon hanging in front ------------------------------------------------------------------------
    ribbon = pup.piece("ribbon", pup.index("lower"), (7.6, 56.5), 0.3)
    rp = spline([(6.5, 57), (8.7, 57), (9.6, 45), (9.0, 35), (10.0, 30.5), (7.0, 29.6), (7.5, 35), (6.8, 45)], True, 5)
    ribbon.fill(rp, TRIM)
    for z in (51.0, 45.0, 39.0):
        ribbon.glow(ellipse(8.1, z, 0.85, 1.6, 0, 8))
    for x0, x1 in ((7.4, 6.6), (8.4, 8.6), (9.4, 10.6)):
        ribbon.ink([(x0, 29.8), (x1, 25.0)], 0.4)
    ribbon.outline(rp, 0.5)

    # ---- Upper body -------------------------------------------------------------------------------------------
    upper = pup.piece("upper", pup.index("lower"), WAIST, 0.6)
    torso = spline([(-8.6, 61), (-9.4, 70), (-9.0, 80), (-7.6, 88), (-5.2, 92.5), (-2.6, 95.0), (-1.8, 98.0),
                    (3.4, 98.0), (4.0, 95.5), (6.4, 92.4), (8.6, 86.5), (8.8, 79), (7.6, 70), (7.6, 61)], True, 5)
    upper.fill(torso, ROBE_TOP)
    upper.lace(torso, "diamond", 2.9, 1.2, 0.8, zmin=67.6, zmax=87.0)
    c1 = spline([(-2.8, 95.4), (0.4, 91.6), (3.4, 89.2), (7.9, 89.6)], False, 6)
    c2 = spline([(-1.4, 97.2), (1.6, 93.8), (4.4, 91.8), (8.2, 92.4)], False, 6)
    collar = band_between(c1, c2)
    upper.fill(collar, TRIM, DEPTH_FILL + 0.01)
    for t in (0.15, 0.4, 0.65, 0.9):
        p = c1[int(t * (len(c1) - 1))]
        q = c2[int(t * (len(c2) - 1))]
        upper.glow(circle((p[0] + q[0]) * 0.5, (p[1] + q[1]) * 0.5, 0.75, 8))
    upper.outline(collar, 0.45)
    belt = [(-8.7, 60.8), (7.7, 60.8), (7.7, 66.0), (-8.9, 66.0)]
    upper.fill(belt, TRIM, DEPTH_FILL + 0.01)
    upper.zigzag(-8.0, 7.0, 61.8, 65.0, 6, 0.6)
    upper.outline(belt, 0.45)
    upper.outline(torso, 0.85)
    upper.knot(-0.6, 63.4)
    pup.anchor("collar", "upper", 1.6, 92.0)

    # ---- Head: hollow face (空脸), hair, ornaments --------------------------------------------------------------
    head = pup.piece("head", pup.index("upper"), NECK, 1.2)
    # Forehead and nose in one straight cut (通天鼻), as in Shaanxi puppets.
    profile = [(3.4, 97.6), (5.0, 99.4), (6.5, 100.6), (7.3, 101.8), (7.0, 102.8), (7.7, 103.6), (7.3, 104.4),
               (7.9, 105.1), (7.7, 106.0), (9.2, 106.8), (6.6, 114.6)]
    face_back = [(4.6, 115.4), (3.0, 114.4), (1.9, 112.2), (1.2, 109.0), (0.6, 106.2), (0.9, 103.2), (1.8, 100.8),
                 (1.0, 97.2)]
    # Hair ends at the nape; the neck below it is part of the hollow face.
    hair = spline([(6.6, 114.6), (6.0, 116.6), (4.2, 118.4), (0.6, 119.6), (-3.4, 119.4), (-6.6, 117.8),
                   (-9.6, 118.8), (-12.8, 117.0), (-13.6, 113.4), (-11.8, 110.2), (-8.4, 108.4), (-5.6, 106.6),
                   (-3.6, 104.6), (-0.6, 104.4)], False, 3) + [(0.6, 106.2), (1.2, 109.0), (1.9, 112.2), (3.0, 114.4), (4.6, 115.4)]
    neck = [(3.4, 97.6), (1.8, 100.8), (0.9, 103.2), (0.4, 104.8), (-3.8, 105.0), (-3.4, 101.4), (-2.6, 96.6)]
    head.fill(hair, HAIR)
    head.glow(profile + face_back, 0.92)
    head.glow(neck, 0.92)
    head.ink([(-3.8, 105.0), (-3.4, 101.4), (-2.6, 96.6)], 0.5)
    head.ink(profile, 0.55)
    head.ink(face_back, 0.4)
    # eye (丹凤眼, the outer corner lifts towards the ear), brow, lips
    head.ink(spline([(7.0, 110.3), (5.6, 110.9), (3.6, 111.6)], False, 4), 0.5)
    head.ink(spline([(6.8, 110.0), (5.4, 110.3), (4.2, 110.8)], False, 4), 0.3)
    head.fill(circle(5.7, 110.55, 0.45, 8), INK, DEPTH_INK)
    head.ink(spline([(7.3, 112.0), (5.6, 112.9), (3.0, 113.3)], False, 4), 0.42)
    head.fill(circle(7.5, 104.4, 0.45, 8), INK, DEPTH_INK)
    # hair strands, crown band, flower comb, bun flowers, hairpins
    for k in range(3):
        head.glow_line(spline([(-4.0 - 1.6 * k, 117.0 - 0.6 * k), (-8.0 - 1.2 * k, 116.2 - 1.4 * k),
                               (-10.6 - 0.6 * k, 112.6 - 1.0 * k)], False, 5), 0.42, 0.65)
    head.glow_line(spline([(5.6, 116.6), (2.2, 119.4), (-2.6, 120.0), (-6.4, 118.6)], False, 5), 0.9)
    head.flower(-0.8, 121.0, 3.0, 8)
    head.flower(-11.0, 114.0, 2.3, 6, rot=15.0)
    head.flower(-7.4, 109.8, 1.7, 5)
    head.ink([(-6.0, 118.0), (-16.6, 121.4)], 0.8)
    head.glow(circle(-16.6, 121.4, 1.1, 10))
    head.ink([(-3.0, 119.0), (-5.2, 126.0)], 0.7)
    head.glow(circle(-5.2, 126.0, 1.0, 10))
    head.glow(circle(0.9, 103.0, 0.7, 8))          # ear drop
    head.outline(hair, 0.5)

    # ---- 步摇: beads dangling from the hairpin ------------------------------------------------------------------
    tassel = pup.piece("tassel", pup.index("head"), (-16.4, 121.0), 1.0)
    for dx, length in ((-1.4, 9.0), (0.0, 12.0), (1.4, 8.0)):
        x = -16.4 + dx
        tassel.ink([(-16.4, 121.0), (x, 121.0 - length)], 0.35)
        for k in range(3):
            tassel.glow(circle(x, 121.0 - length * (k + 1) / 3.0, 0.65 if k < 2 else 0.95, 8))

    # ---- Arms and water sleeves (far side behind the body, near side in front) ---------------------------------
    wz = add_arm(pup, "far", -1.6, 89.0, -2.4, ROBE_TOP)
    add_water_sleeve(pup, "far", -1.6, wz, -2.6)
    wz = add_arm(pup, "near", -0.4, 89.0, 1.6, ROBE_TOP)
    add_water_sleeve(pup, "near", -0.4, wz, 1.7)

    pup.height = 126.0
    return pup


def build_partner():
    pup = Puppet("Partner")
    WAIST = (0.0, 64.0)
    NECK = (1.0, 100.0)

    lower = pup.piece("lower", -1, WAIST, 0.0)
    hem_ctrl = [(-15.5, 12.5), (-6, 11.2), (4, 11.8), (13.5, 12.2)]
    robe = spline([(-9.2, 66), (-10.0, 56), (-11.5, 40), (-13.5, 25)] + hem_ctrl + [(11.8, 25), (10.2, 40), (8.9, 56), (8.4, 66)], True, 5)
    lower.fill(robe, SCHOLAR_ROBE)
    lower.lace(robe, "coin", 3.5, 1.15, 0.9, zmin=21.0, zmax=60.0)
    opening = spline([(5.6, 64), (6.4, 45), (8.2, 24), (10.0, 13)], False, 6)
    lower.fill(band_between(opening, offset(opening, 2.6, 0.0)), TRIM, DEPTH_FILL + 0.01)
    lower.glow_line(offset(opening, 1.3, 0.0), 0.6)
    hem = spline(hem_ctrl, False, 5)
    lower.fill(band_between(hem, [(x * 0.92, z + 6.0) for x, z in hem]), TRIM, DEPTH_FILL + 0.02)
    for i, x in enumerate((-11.0, -6.0, -1.0, 4.0, 9.0)):
        lower.cloud(x, 15.0, 2.0, cw=(i % 2 == 0), w=0.6, start=90.0)
    lower.outline(robe, 0.85)

    add_leg(pup, "far", -2.4, 56.0, -3.0, SCHOLAR_ROBE, boot=True)
    add_leg(pup, "near", 1.2, 56.0, -1.8, SCHOLAR_ROBE, boot=True)

    upper = pup.piece("upper", pup.index("lower"), WAIST, 0.6)
    torso = spline([(-9.6, 63), (-10.2, 72), (-9.8, 83), (-8.2, 90.5), (-5.4, 95.2), (-2.6, 98.0), (-1.8, 101.0),
                    (3.6, 101.0), (4.2, 98.4), (6.8, 95.0), (9.0, 89), (9.2, 80), (8.2, 70), (8.4, 63)], True, 5)
    upper.fill(torso, SCHOLAR_ROBE)
    upper.lace(torso, "diamond", 3.0, 1.25, 0.8, zmin=69.5, zmax=90.0)
    c1 = spline([(-2.8, 98.4), (0.6, 94.2), (4.0, 91.2), (8.6, 91.8)], False, 6)
    c2 = spline([(-1.4, 100.2), (1.8, 96.4), (5.0, 93.8), (8.9, 94.6)], False, 6)
    upper.fill(band_between(c1, c2), TRIM, DEPTH_FILL + 0.01)
    upper.glow_line(spline([(-2.1, 99.3), (1.2, 95.3), (4.5, 92.5), (8.75, 93.2)], False, 6), 0.55)
    upper.outline(band_between(c1, c2), 0.45)
    belt = [(-10.0, 63.2), (8.6, 63.2), (8.6, 67.6), (-10.0, 67.6)]
    upper.fill(belt, BOOT, DEPTH_FILL + 0.01)
    upper.coins(-9.0, 2.4, 65.4, 1.0, 4)
    upper.glow(circle(5.4, 65.4, 1.7, 12))       # jade disc
    upper.fill(circle(5.4, 65.4, 0.6, 8), INK, DEPTH_INK)
    upper.outline(belt, 0.45)
    upper.outline(torso, 0.85)
    upper.knot(-0.8, 69.0)
    pup.anchor("collar", "upper", 1.8, 95.0)

    head = pup.piece("head", pup.index("upper"), NECK, 1.2)
    profile = [(3.6, 101.0), (5.6, 102.8), (7.4, 104.0), (7.4, 105.4), (7.9, 106.2), (7.5, 107.0), (8.1, 107.8),
               (7.9, 108.8), (9.8, 110.0), (7.2, 118.4)]
    face_back = [(5.6, 118.6), (3.8, 117.0), (2.4, 114.2), (1.6, 110.8), (1.2, 107.4), (1.6, 104.4), (2.6, 102.2),
                 (1.6, 100.2)]
    cap = spline([(7.2, 118.4), (7.8, 120.0), (7.0, 124.8), (2.0, 126.6), (-6.0, 125.8), (-9.4, 122.4),
                  (-9.8, 116.8), (-8.0, 112.6), (-5.6, 109.4), (-2.4, 108.0), (0.6, 108.2)], False, 3) + \
        [(1.6, 110.8), (2.4, 114.2), (3.8, 117.0), (5.6, 118.6)]
    neck = [(3.6, 101.0), (2.6, 102.2), (1.6, 104.4), (1.2, 107.4), (0.8, 108.6), (-5.0, 109.6), (-4.0, 104.6), (-2.8, 99.8)]
    head.fill(cap, SCHOLAR_CAP)
    head.glow(profile + face_back, 0.92)
    head.glow(neck, 0.92)
    head.ink([(-5.0, 109.6), (-4.0, 104.6), (-2.8, 99.8)], 0.5)
    head.ink(profile, 0.55)
    head.ink(face_back, 0.4)
    head.ink(spline([(7.4, 113.6), (6.0, 114.0), (4.2, 114.5)], False, 4), 0.5)
    head.fill(circle(6.0, 113.95, 0.48, 8), INK, DEPTH_INK)
    head.ink(spline([(7.8, 115.5), (5.8, 116.5), (3.6, 116.5)], False, 4), 0.75)
    head.fill(circle(7.8, 107.4, 0.4, 8), INK, DEPTH_INK)
    head.glow_line(spline([(7.6, 120.0), (2.0, 120.8), (-4.0, 120.4), (-9.2, 119.4)], False, 5), 1.0)
    head.glow(circle(4.4, 123.2, 1.5, 12))       # jade on the cap
    head.fill(circle(4.4, 123.2, 0.55, 8), INK, DEPTH_INK)
    head.cloud(-3.2, 123.2, 1.9, cw=True, w=0.6)
    head.outline(cap, 0.5)

    streamers = pup.piece("streamers", pup.index("head"), (-8.6, 121.0), 1.0)
    for dx in (-0.4, 2.4):
        x = -8.6 + dx
        sp = spline([(x - 1.1, 121.5), (x + 1.3, 121.5), (x + 0.7, 112), (x + 1.8, 103), (x - 0.7, 101.4),
                     (x - 0.9, 112)], True, 4)
        streamers.fill(sp, SCHOLAR_CAP)
        streamers.glow_line([(x + 0.1, 118.5), (x + 0.5, 106)], 0.55)
        streamers.outline(sp, 0.4)

    add_arm(pup, "far", -1.8, 92.0, -2.4, SCHOLAR_ROBE, wide=True)
    add_arm(pup, "near", -0.4, 92.0, 1.6, SCHOLAR_ROBE, wide=True)

    # Folding fan in the near hand (opens upwards).
    hand = next(a for a in pup.anchors if a[0] == "hand_near")
    hx, hz = hand[2], hand[3]
    fan = pup.piece("fan", pup.index("arm_fore_near"), (hx, hz), 1.9)
    leaf_outer = [(hx + 14.0 * math.cos(math.radians(a)), hz + 14.0 * math.sin(math.radians(a))) for a in range(40, 141, 5)]
    leaf_inner = [(hx + 5.0 * math.cos(math.radians(a)), hz + 5.0 * math.sin(math.radians(a))) for a in range(140, 39, -5)]
    fan.fill(leaf_outer + leaf_inner, FAN)
    for k in range(8):
        a0, a1 = 40 + 100 * k / 8.0 + 2.2, 40 + 100 * (k + 1) / 8.0 - 2.2
        cell = [(hx + 12.4 * math.cos(math.radians(a)), hz + 12.4 * math.sin(math.radians(a))) for a in (a0, a1)]
        cell += [(hx + 6.6 * math.cos(math.radians(a)), hz + 6.6 * math.sin(math.radians(a))) for a in (a1, a0)]
        fan.glow(cell)
    for i in range(9):
        a = math.radians(40 + 100 * i / 8.0)
        fan.ink([(hx, hz), (hx + 14.0 * math.cos(a), hz + 14.0 * math.sin(a))], 0.45)
    fan.outline(leaf_outer + leaf_inner, 0.5)

    pup.height = 127.0
    return pup


WOOD = (0.34, 0.18, 0.08)


def build_pane():
    """The lock on the stage: a carved lattice window standing on the floor. Its openings glow in the key colour."""
    pup = Puppet("Pane")
    pane = pup.piece("pane", -1, (0.0, 0.0), 0.0)
    W, H = 17.0, 124.0          # half width, height of the window
    outer = [(-W - 2.5, 0.0), (W + 2.5, 0.0), (W + 1.0, 4.0), (W, 4.0), (W, H), (W + 3.0, H + 3.0), (W + 1.0, H + 6.0)] + \
        [(9.0 * math.cos(math.radians(a)), H + 6.0 + 7.0 * math.sin(math.radians(a))) for a in range(0, 181, 15)] + \
        [(-W - 1.0, H + 6.0), (-W - 3.0, H + 3.0), (-W, H), (-W, 4.0), (-W - 1.0, 4.0)]
    pane.fill(outer, WOOD)
    x_lo, x_hi, z_lo, z_hi = -W + 3.0, W - 3.0, 8.0, H - 3.0
    inner = [(x_lo, z_lo), (x_hi, z_lo), (x_hi, z_hi), (x_lo, z_hi)]
    pane.glow(inner)
    # Lattice (窗棂): a diamond grid of bars over the opening, a round medallion in the middle.
    slope, step = 1.6, 7.0
    for sign in (1.0, -1.0):
        for i in range(-8, 30):
            c = z_lo + i * step
            pts = []
            x = x_lo
            while x <= x_hi + 1e-6:
                z = c + sign * slope * (x - x_lo if sign > 0 else x - x_hi)
                if z_lo <= z <= z_hi:
                    pts.append((x, z))
                x += 0.25
            if len(pts) >= 2:
                pane.ink([pts[0], pts[-1]], 1.1)
    cx, cz = 0.0, H * 0.5 + 4.0
    pane.fill(circle(cx, cz, 9.0, 24), WOOD, DEPTH_INK + 0.01)
    pane.glow(circle(cx, cz, 7.0, 24), 1.0, DEPTH_INK + 0.02)
    for i in range(8):
        a = 22.5 + 45.0 * i
        ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
        pane.fill(ellipse(cx + ca * 3.6, cz + sa * 3.6, 2.6, 0.9, a, 10), INK, DEPTH_INK + 0.03)
    pane.fill(circle(cx, cz, 1.4, 10), INK, DEPTH_INK + 0.03)
    pane.ink(circle(cx, cz, 7.0, 24), 0.9, closed=True)
    pane.ink(inner, 1.6, closed=True)
    pane.outline(outer, 1.0)
    pup.height = H + 13.0
    return pup

# ---------------------------------------------------------------------------------------------------------------
# Output: C++ data
# ---------------------------------------------------------------------------------------------------------------

def mesh_sections(pup):
    """Per piece and section: vertices relative to the piece pivot (x, depth, z, r, g, b) and triangle indices."""
    out = []
    for piece in pup.pieces:
        px, pz = piece.pivot
        sections = []
        for sec in (LEATHER, GLOW):
            verts, index_of, indices = [], {}, []
            for s, depth, col, tri in piece.tris:
                if s != sec:
                    continue
                for (x, z) in tri:
                    key = (round(x - px, 3), round(depth, 3), round(z - pz, 3), col)
                    if key not in index_of:
                        index_of[key] = len(verts)
                        verts.append(key)
                    indices.append(index_of[key])
            sections.append((verts, indices))
        out.append(sections)
    return out


def fmt(v):
    s = ("%.3f" % v).rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def ff(v):
    """C++ float literal."""
    s = fmt(v)
    return (s if "." in s else s + ".0") + "f"


def write_inl(path, puppets):
    lines = ["// Generated by Scripts/puppets/gen_puppets.py - do not edit by hand.",
             "// Shadow puppet pieces: per piece a leather section (dyed hide + ink) and a glow section (the carved openings).",
             "// Vertices: x, depth (towards the viewer), z, r, g, b - centimetres relative to the piece pivot.",
             "// Included once, inside namespace HuePuppetShapes (HueShadowPuppet.cpp).", ""]
    for pup in puppets:
        sections = mesh_sections(pup)
        all_verts, all_idx, piece_rows = [], [], []
        for piece, secs in zip(pup.pieces, sections):
            ranges = []
            for verts, idx in secs:
                ranges.append((len(all_verts), len(verts), len(all_idx), len(idx)))
                all_verts += verts
                all_idx += idx
            parent_pivot = pup.pieces[piece.parent].pivot if piece.parent >= 0 else (0.0, 0.0)
            piece_rows.append((piece, (piece.pivot[0] - parent_pivot[0], piece.pivot[1] - parent_pivot[1]), ranges))
        n = pup.name
        lines.append("static const float %sVertices[] = {" % n)
        for v in all_verts:
            lines.append("\t%s,%s,%s,%s,%s,%s," % (ff(v[0]), ff(v[1]), ff(v[2]), ff(v[3][0]), ff(v[3][1]), ff(v[3][2])))
        lines.append("};")
        lines.append("static const uint16 %sIndices[] = {" % n)
        for i in range(0, len(all_idx), 24):
            lines.append("\t" + ",".join(str(k) for k in all_idx[i:i + 24]) + ",")
        lines.append("};")
        lines.append("static const FHuePuppetPieceDef %sPieces[] = {" % n)
        for piece, (lx, lz), ranges in piece_rows:
            r = ", ".join("{ %d, %d, %d, %d }" % rr for rr in ranges)
            lines.append('\t{ TEXT("%s"), %d, %s, %s, %s, { %s } },' % (piece.name, piece.parent, ff(lx), ff(lz), ff(piece.layer), r))
        lines.append("};")
        lines.append("static const FHuePuppetAnchorDef %sAnchors[] = {" % n)
        for name, idx, x, z in pup.anchors:
            px, pz = pup.pieces[idx].pivot
            lines.append('\t{ TEXT("%s"), %d, %s, %s },' % (name, idx, ff(x - px), ff(z - pz)))
        if not pup.anchors:
            lines.append('\t{ TEXT(""), -1, 0.0f, 0.0f }, // placeholder: no rods')
        lines.append("};")
        lines.append("static const FHuePuppetDef %sPuppet = { TEXT(\"%s\"), %sPieces, UE_ARRAY_COUNT(%sPieces), %sVertices, UE_ARRAY_COUNT(%sVertices) / 6, "
                     "%sIndices, UE_ARRAY_COUNT(%sIndices), %sAnchors, %d, %s };"
                     % (n, n, n, n, n, n, n, n, n, len(pup.anchors), ff(pup.height)))
        lines.append("")
        print("%s: %d pieces, %d vertices, %d triangles" % (n, len(pup.pieces), len(all_verts), len(all_idx) // 3))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))

# ---------------------------------------------------------------------------------------------------------------
# Output: SVG preview (poses are a rough copy of the game's; only for checking shapes and pivots)
# ---------------------------------------------------------------------------------------------------------------

def world_transforms(pup, pose):
    """Per piece: (angle, pivot in world) given local angles (degrees, counter-clockwise as the viewer sees it)."""
    result = []
    for i, piece in enumerate(pup.pieces):
        local = pose.get(piece.name, 0.0)
        if piece.parent < 0:
            result.append((local, piece.pivot))
            continue
        pa, (ppx, ppz) = result[piece.parent]
        parent_rest = pup.pieces[piece.parent].pivot
        dx, dz = piece.pivot[0] - parent_rest[0], piece.pivot[1] - parent_rest[1]
        c, s = math.cos(math.radians(pa)), math.sin(math.radians(pa))
        result.append((pa + local, (ppx + dx * c - dz * s, ppz + dx * s + dz * c)))
    return result


def puppet_draws(pup, pose, ox, glow_rgb, light=(1.0, 0.92, 0.78), transmission=0.6, flip=False):
    """(depth, linear rgb, world triangle) for every triangle of the posed puppet, standing at x = ox."""
    xf = world_transforms(pup, pose)
    items = []
    for i, piece in enumerate(pup.pieces):
        ang, (wx, wz) = xf[i]
        c, s = math.cos(math.radians(ang)), math.sin(math.radians(ang))
        for sec, depth, col, tri in piece.tris:
            pts = []
            for (x, z) in tri:
                lx, lz = x - piece.pivot[0], z - piece.pivot[1]
                gx, gz = wx + lx * c - lz * s, wz + lx * s + lz * c
                pts.append((ox + (-gx if flip else gx), gz))
            if sec == GLOW:
                rgb = tuple(min(1.0, glow_rgb[k] * col[k]) for k in range(3))
            else:
                rgb = tuple(col[k] * light[k] * transmission for k in range(3))
            items.append((piece.layer + depth, rgb, pts))
    return items


def srgb(v):
    v = max(0.0, min(1.0, v))
    return int(255 * (v * 12.92 if v < 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055) + 0.5)


def render_png(path, draws, x0, x1, z0, z1, scale, labels=()):
    """Scanline-fills the triangles (painter's order by depth) at 2x and box-filters down. Pure Python + zlib."""
    import struct
    import zlib
    ss = 2
    W, H = int((x1 - x0) * scale), int((z1 - z0) * scale)
    SW, SH = W * ss, H * ss
    k = scale * ss
    buf = bytearray(SW * SH * 3)
    floor_z = 0.0
    for row in range(SH):
        z = z1 - (row + 0.5) / k
        if z < floor_z:
            col = (0.10, 0.06, 0.035)
        else:
            t = min(1.0, max(0.0, (z - floor_z) / (z1 - floor_z)))
            glow = 1.0 - 0.55 * abs(t - 0.45) ** 1.6
            col = (1.0 * glow, 0.90 * glow, 0.72 * glow)
        buf[row * SW * 3:(row + 1) * SW * 3] = bytes((srgb(col[0]), srgb(col[1]), srgb(col[2]))) * SW
    for depth, rgb, tri in sorted(draws, key=lambda d: d[0]):
        c = bytes((srgb(rgb[0]), srgb(rgb[1]), srgb(rgb[2])))
        px = [((x - x0) * k, (z1 - z) * k) for x, z in tri]
        ymin = max(0, int(math.floor(min(p[1] for p in px))))
        ymax = min(SH - 1, int(math.ceil(max(p[1] for p in px))))
        for y in range(ymin, ymax + 1):
            yc = y + 0.5
            xs = []
            for i in range(3):
                (ax, ay), (bx, by) = px[i], px[(i + 1) % 3]
                if (ay <= yc < by) or (by <= yc < ay):
                    xs.append(ax + (yc - ay) * (bx - ax) / (by - ay))
            if len(xs) < 2:
                continue
            a = max(0, int(math.floor(min(xs) + 0.5)))
            b = min(SW, int(math.floor(max(xs) + 0.5)) + 1)
            if b > a:
                buf[(y * SW + a) * 3:(y * SW + b) * 3] = c * (b - a)
    # 2x2 box filter down to the output size.
    raw = bytearray()
    for y in range(H):
        raw.append(0)
        r0 = (2 * y) * SW * 3
        r1 = (2 * y + 1) * SW * 3
        row = bytearray(W * 3)
        for x in range(W):
            i = 6 * x
            for ch in range(3):
                row[3 * x + ch] = (buf[r0 + i + ch] + buf[r0 + i + 3 + ch] + buf[r1 + i + ch] + buf[r1 + i + 3 + ch]) >> 2
        raw += row
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def write_preview(path, dancer, partner):
    rest = {"arm_upper_near": 52, "arm_fore_near": 78, "sleeve_near": -128, "arm_upper_far": -22, "arm_fore_far": 26,
            "sleeve_far": -4, "head": 4, "upper": -2, "tassel": -4, "ribbon": 2}
    walk = {"leg_near": 22, "leg_far": -18, "upper": 7, "head": -3, "arm_upper_near": 25, "arm_fore_near": 35,
            "sleeve_near": -45, "arm_upper_far": -40, "arm_fore_far": 15, "sleeve_far": 35, "tassel": 18, "ribbon": 14}
    strike = {"arm_upper_near": 150, "arm_fore_near": 25, "sleeve_near": 40, "arm_upper_far": -70, "arm_fore_far": 20,
              "sleeve_far": 60, "upper": 9, "head": 6, "leg_near": 12, "leg_far": -8, "tassel": 30, "ribbon": 20}
    fall = {"lower": -70, "upper": -15, "head": -20, "arm_upper_near": 120, "arm_fore_near": 30, "sleeve_near": 50,
            "arm_upper_far": 90, "arm_fore_far": 20, "sleeve_far": 70, "leg_near": 30, "leg_far": 15, "tassel": 60, "ribbon": 60}
    partner_rest = {"arm_upper_near": 40, "arm_fore_near": 55, "fan": -25, "arm_upper_far": -10, "arm_fore_far": 10,
                    "head": 2, "streamers": -3}
    red, cyan, white, yellow = (1.0, 0.16, 0.10), (0.15, 0.95, 1.0), (1.0, 1.0, 1.0), (1.0, 0.85, 0.12)
    draws = []
    draws += puppet_draws(partner, partner_rest, 0, red)
    draws += puppet_draws(dancer, rest, 55, red)
    draws += puppet_draws(dancer, walk, 105, cyan)
    draws += puppet_draws(dancer, strike, 160, yellow)
    draws += puppet_draws(dancer, {}, 215, white)
    draws += puppet_draws(dancer, fall, 290, white)
    render_png(path, draws, -25.0, 345.0, -12.0, 140.0, 5.0)
    if "--zoom" in sys.argv:
        render_png(sys.argv[sys.argv.index("--zoom") + 1], puppet_draws(dancer, rest, 0, red) + puppet_draws(partner, partner_rest, -60, red)
                   + puppet_draws(build_pane(), {}, 50, yellow), -80.0, 75.0, -6.0, 140.0, 7.0)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.normpath(os.path.join(here, "..", ".."))
    dancer, partner = build_dancer(), build_partner()
    write_inl(os.path.join(root, "Source", "Huebox", "Rig", "HuePuppetShapes.inl"), [dancer, partner, build_pane()])
    if "--png" in sys.argv:
        write_preview(sys.argv[sys.argv.index("--png") + 1], dancer, partner)


if __name__ == "__main__":
    main()
