"""Walking routes over the game's own navigation meshes.

Every map ships an .nvmbnd archive of NVM meshes (the walkable surface enemy AI uses), each placed
in the map by a navmesh part. MSB positions are already world coordinates: the maps' MapOffset
events must not be added (with them, neighbouring maps' meshes no longer meet). We load every
piece, join pieces where their open edges meet (plus small gaps and safe drops, which the meshes
leave unconnected; ladders are already built in as steep strips), then A* between points and pull
the result tight into a natural walking line.
"""

import heapq
import math
import os
import struct
from collections import defaultdict

import dsr_formats as fmt

PART_NAVMESH = 8

# Joining open edges between mesh pieces (metres).
SEAM_DIST, SEAM_RISE = 0.8, 0.8      # the same walkable seam split between two pieces
GAP_DIST, GAP_RISE = 1.5, 1.0        # a small gap or step you can walk or roll across
DROP_MIN, DROP_MAX, DROP_REACH = 1.0, 7.0, 2.0   # ledge you can safely drop off (one way)

MIN_REGION = 30  # triangles; smaller pieces connected to nothing aren't somewhere to route from

# Extra cost per link type, so routes prefer plain walking and only use these when needed.
PENALTY = {"walk": 0.0, "seam": 0.0, "gap": 4.0, "drop": 6.0}


def read_nvm(data):
    """Returns (vertices, triangles) where a triangle is (v1, v2, v3, n1, n2, n3); n = -1 if open."""
    _version, vcount, voff, tcount, toff = struct.unpack_from("<5i", data, 0)
    verts = [struct.unpack_from("<3f", data, voff + i * 12) for i in range(vcount)]
    tris = [struct.unpack_from("<6i", data, toff + i * 28) for i in range(tcount)]
    return verts, tris


def _rotate_y(v, degrees):
    if not degrees:
        return v
    r = math.radians(degrees)
    c, s = math.cos(r), math.sin(r)
    return (v[0] * c + v[2] * s, v[1], -v[0] * s + v[2] * c)


def _models(data):
    """Model names in MSB order (parts reference them by index)."""
    sections = fmt._sections(data)
    return [fmt._cstr(data, start + struct.unpack_from("<i", data, start)[0])
            for start in sections["MODEL_PARAM_ST"]]


class World:
    def __init__(self):
        self.pos = []        # triangle centroid
        self.tri = []        # (a, b, c) world vertices
        self.stem = []       # map of each triangle
        self.mesh = []       # mesh piece of each triangle
        self.links = defaultdict(list)   # tri -> [(tri, cost, kind, portal(a, b))]
        self.grid = defaultdict(list)    # (x // 4, z // 4) -> tris, for point lookups

    # ------------------------------------------------------------------ loading

    def load(self, game, stems, world_offsets=False):
        for stem in stems:
            msb_path = os.path.join(game, "map", "MapStudio", stem + ".msb")
            bnd_path = os.path.join(game, "map", stem, stem + ".nvmbnd.dcx")
            if not os.path.exists(bnd_path):
                continue
            data = open(msb_path, "rb").read()
            models = _models(data)
            offset = fmt.read_msb(msb_path).offset if world_offsets else (0.0, 0.0, 0.0)
            meshes = fmt.read_bnd3(fmt.read_dcx(bnd_path))
            for start in fmt._sections(data)["PARTS_PARAM_ST"]:
                _name_off, ptype, _idx, model_idx = struct.unpack_from("<iIii", data, start)
                if ptype != PART_NAVMESH:
                    continue
                pos = struct.unpack_from("<3f", data, start + 0x14)
                rot_y = struct.unpack_from("<f", data, start + 0x24)[0]
                model = models[model_idx]
                nvm = next((d for n, d in meshes.items() if n.startswith(model)), None)
                if nvm is None:
                    continue
                verts, tris = read_nvm(nvm)
                world = [tuple(p + q + o for p, q, o in zip(_rotate_y(v, rot_y), pos, offset))
                         for v in verts]
                self._add_mesh(stem, world, tris)
        self._join_open_edges()

    def _add_mesh(self, stem, verts, tris):
        base = len(self.tri)
        mesh_id = (self.mesh[-1] + 1) if self.mesh else 0
        for a, b, c, *_ in tris:
            t = (verts[a], verts[b], verts[c])
            idx = len(self.tri)
            self.tri.append(t)
            self.pos.append(tuple(sum(p[k] for p in t) / 3 for k in range(3)))
            self.stem.append(stem)
            self.mesh.append(mesh_id)
            xs, zs = [p[0] for p in t], [p[2] for p in t]
            for gx in range(int(min(xs) // 4), int(max(xs) // 4) + 1):
                for gz in range(int(min(zs) // 4), int(max(zs) // 4) + 1):
                    self.grid[(gx, gz)].append(idx)
        self.open_edges = getattr(self, "open_edges", [])
        for i, (a, b, c, n1, n2, n3) in enumerate(tris):
            corners = (verts[a], verts[b], verts[c])
            for k, n in enumerate((n1, n2, n3)):
                edge = (corners[k], corners[(k + 1) % 3])
                if n >= 0:
                    self._link(base + i, base + n, "walk", edge)
                else:
                    self.open_edges.append((base + i, edge))

    def _link(self, a, b, kind, portal):
        cost = math.dist(self.pos[a], self.pos[b]) + PENALTY[kind]
        self.links[a].append((b, cost, kind, portal))

    def _join_open_edges(self):
        """Connects mesh pieces (and maps) where their open edges meet, plus gaps, drops, climbs."""
        buckets = defaultdict(list)
        for i, (tri, (p, q)) in enumerate(self.open_edges):
            mid = tuple((a + b) / 2 for a, b in zip(p, q))
            buckets[(int(mid[0] // 3), int(mid[2] // 3))].append((tri, mid, (p, q)))
        seen = set()
        for (gx, gz), items in buckets.items():
            near = [it for dx in (-1, 0, 1) for dz in (-1, 0, 1) for it in buckets.get((gx + dx, gz + dz), [])]
            for ta, ma, ea in items:
                for tb, mb, eb in near:
                    # Only between different mesh pieces: open edges within one piece face
                    # each other across walls, pillars and holes, never across a walkable gap.
                    if self.mesh[ta] == self.mesh[tb] or (ta, tb) in seen:
                        continue
                    flat = math.hypot(ma[0] - mb[0], ma[2] - mb[2])
                    rise = mb[1] - ma[1]
                    kind = None
                    if flat <= SEAM_DIST and abs(rise) <= SEAM_RISE:
                        kind = "seam"
                    elif flat <= GAP_DIST and abs(rise) <= GAP_RISE:
                        kind = "gap"
                    elif flat <= DROP_REACH and DROP_MIN <= -rise <= DROP_MAX:
                        kind = "drop"  # one way: only from the higher edge down
                    if kind:
                        seen.add((ta, tb))
                        self._link(ta, tb, kind, ea if kind == "seam" else (ma, ma))
        # Ledges that overhang the middle of a lower surface (not just its edge).
        for ta, (p, q) in self.open_edges:
            mid = tuple((a + b) / 2 for a, b in zip(p, q))
            out = self._outward(ta, p, q)
            probe = (mid[0] + out[0] * 1.0, mid[1], mid[2] + out[1] * 1.0)
            below = self.surface_below(probe, DROP_MIN, DROP_MAX)
            if below is not None and (ta, below) not in seen:
                seen.add((ta, below))
                self._link(ta, below, "drop", (mid, mid))
        del self.open_edges
        self._mark_scraps()

    def export(self, path):
        """Writes the joined meshes for the overlay's own routing (src/route.cpp reads this):
            "DSRN", version 1, triangle count, link count           (4 x u32)
            per triangle: 3 corners x (x, y, z)                      (9 x f32)
            per triangle: 1 if it's a scrap piece                    (u8)
            first link of each triangle, plus the end               (count + 1 x u32)
            per link: to (u32), cost (f32), kind (u8: walk, seam, gap, drop; 3 pad),
                      portal corners (6 x f32)
        """
        kinds = {"walk": 0, "seam": 1, "gap": 2, "drop": 3}
        n = len(self.tri)
        links = [self.links.get(t, []) for t in range(n)]
        total = sum(len(ls) for ls in links)
        with open(path, "wb") as f:
            f.write(b"DSRN" + struct.pack("<3I", 1, n, total))
            f.write(b"".join(struct.pack("<9f", *a, *b, *c) for a, b, c in self.tri))
            f.write(bytes(1 if t in self.scrap else 0 for t in range(n)))
            first, k = [], 0
            for ls in links:
                first.append(k)
                k += len(ls)
            first.append(k)
            f.write(struct.pack(f"<{n + 1}I", *first))
            f.write(b"".join(struct.pack("<IfB3x6f", to, cost, kinds[kind], *portal[0], *portal[1])
                             for ls in links for to, cost, kind, portal in ls))

    def _mark_scraps(self):
        """Tiny pieces connected to nothing (one under the Firelink bonfire, for one) would
        swallow a route that starts on them, so point lookups skip them."""
        undirected = defaultdict(set)
        for a, links in self.links.items():
            for b, *_ in links:
                undirected[a].add(b)
                undirected[b].add(a)
        self.scrap = set()
        seen = set()
        for start in range(len(self.tri)):
            if start in seen:
                continue
            piece, todo = [start], [start]
            seen.add(start)
            while todo:
                for n in undirected[todo.pop()]:
                    if n not in seen:
                        seen.add(n)
                        piece.append(n)
                        todo.append(n)
            if len(piece) < MIN_REGION:
                self.scrap.update(piece)

    def _outward(self, tri, p, q):
        """Horizontal unit vector pointing away from the triangle across edge p-q."""
        ex, ez = q[0] - p[0], q[2] - p[2]
        nx, nz = ez, -ex
        length = math.hypot(nx, nz) or 1.0
        nx, nz = nx / length, nz / length
        c = self.pos[tri]
        if (c[0] - p[0]) * nx + (c[2] - p[2]) * nz > 0:
            nx, nz = -nx, -nz
        return nx, nz

    # ------------------------------------------------------------------ queries

    def _contains(self, t, x, z):
        (ax, _, az), (bx, _, bz), (cx, _, cz) = self.tri[t]
        d1 = (x - bx) * (az - bz) - (ax - bx) * (z - bz)
        d2 = (x - cx) * (bz - cz) - (bx - cx) * (z - cz)
        d3 = (x - ax) * (cz - az) - (cx - ax) * (z - az)
        neg = d1 < 0 or d2 < 0 or d3 < 0
        pos = d1 > 0 or d2 > 0 or d3 > 0
        return not (neg and pos)

    def height(self, t, x, z):
        (ax, ay, az), (bx, by, bz), (cx, cy, cz) = self.tri[t]
        det = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz)
        if abs(det) < 1e-9:
            return self.pos[t][1]
        w1 = ((bz - cz) * (x - cx) + (cx - bx) * (z - cz)) / det
        w2 = ((cz - az) * (x - cx) + (ax - cx) * (z - cz)) / det
        return w1 * ay + w2 * by + (1 - w1 - w2) * cy

    def surface_at(self, p, max_rise=2.0):
        """Triangle under (or just above) point p, closest in height."""
        best, best_dy = None, None
        scrap = getattr(self, "scrap", ())
        for t in self.grid.get((int(p[0] // 4), int(p[2] // 4)), []):
            if t not in scrap and self._contains(t, p[0], p[2]):
                dy = abs(self.height(t, p[0], p[2]) - p[1])
                if dy <= max_rise and (best_dy is None or dy < best_dy):
                    best, best_dy = t, dy
        return best

    def surface_below(self, p, lo, hi):
        best, best_drop = None, None
        for t in self.grid.get((int(p[0] // 4), int(p[2] // 4)), []):
            if self._contains(t, p[0], p[2]):
                drop = p[1] - self.height(t, p[0], p[2])
                if lo <= drop <= hi and (best_drop is None or drop < best_drop):
                    best, best_drop = t, drop
        return best

    def _distance(self, t, p):
        """Distance from p to triangle t: horizontal gap to its outline, and height above it."""
        x, z = p[0], p[2]
        if self._contains(t, x, z):
            flat = 0.0
        else:
            flat = math.inf
            corners = self.tri[t]
            for k in range(3):
                (ax, _, az), (bx, _, bz) = corners[k], corners[(k + 1) % 3]
                ex, ez = bx - ax, bz - az
                f = max(0.0, min(1.0, ((x - ax) * ex + (z - az) * ez) / ((ex * ex + ez * ez) or 1.0)))
                flat = min(flat, math.hypot(x - (ax + ex * f), z - (az + ez * f)))
        ys = [c[1] for c in self.tri[t]]
        rise = max(0.0, min(ys) - p[1], p[1] - max(ys))
        return math.hypot(flat, rise)

    def nearest(self, p, radius=8.0):
        """Closest triangle to p: the one under it, else the nearest within radius."""
        t = self.surface_at(p, 3.0)
        if t is not None:
            return t
        best, best_d = None, radius
        r = int(radius // 4) + 1
        gx, gz = int(p[0] // 4), int(p[2] // 4)
        for dx in range(-r, r + 1):
            for dz in range(-r, r + 1):
                for t in self.grid.get((gx + dx, gz + dz), []):
                    if t in self.scrap:
                        continue
                    d = self._distance(t, p)
                    if d < best_d:
                        best, best_d = t, d
        return best

    # ------------------------------------------------------------------ routing

    def route(self, start, goal, max_nodes=400000):
        """Walking route from start to goal as a list of points, and whether it reaches the goal.

        When the meshes don't connect all the way (a lift, a long drop, a door the AI mesh
        skips), the route goes as far as it can get toward the goal and then straight to it.
        Returns (None, False) if start or goal isn't near any walkable surface.
        """
        # A boss on a ledge or an item over water can be well off the walkable floor: route to
        # the nearest floor and finish with a short straight piece.
        a = self.nearest(start)
        a = a if a is not None else self.nearest(start, 25.0)
        b = self.nearest(goal)
        b = b if b is not None else self.nearest(goal, 25.0)
        if a is None or b is None:
            return None, False
        if a == b:
            return [start, goal], True
        target = self.pos[b]
        open_heap = [(math.dist(self.pos[a], target), 0.0, a)]
        came = {a: None}
        cost = {a: 0.0}
        closest, closest_d = a, math.dist(self.pos[a], target)
        expanded = 0
        while open_heap and expanded < max_nodes:
            _, g, t = heapq.heappop(open_heap)
            if t == b:
                break
            if g > cost[t]:
                continue
            expanded += 1
            d = math.dist(self.pos[t], target)
            if d < closest_d:
                closest, closest_d = t, d
            for n, c, kind, portal in self.links[t]:
                ng = g + c
                if ng < cost.get(n, math.inf):
                    cost[n] = ng
                    came[n] = (t, kind, portal)
                    heapq.heappush(open_heap, (ng + math.dist(self.pos[n], target), ng, n))
        complete = b in came
        if not complete:
            b = closest
        if b == a:
            return [start, goal], complete

        # Corridor of triangles and the portals between them, start to goal.
        corridor, steps = [b], []
        t = b
        while came[t] is not None:
            prev, kind, portal = came[t]
            steps.append((kind, portal))
            corridor.append(prev)
            t = prev
        corridor.reverse()
        steps.reverse()
        end = goal if complete else self.pos[b]
        pts = self._pull_tight(start, end, corridor, steps)
        if not complete:
            pts.append(goal)
        return pts, complete

    def _pull_tight(self, start, goal, corridor, steps):
        """Points through portal midpoints, then drop every point that a straight walk can skip."""
        pts = [start]
        keep = [True]  # points that must stay (drop-offs and gaps)
        for kind, (p, q) in steps:
            mid = tuple((u + v) / 2 for u, v in zip(p, q))
            pts.append(mid)
            keep.append(kind in ("drop", "gap"))
        pts.append(goal)
        keep.append(True)

        allowed = set(corridor)
        out = [pts[0]]
        i = 0
        while i < len(pts) - 1:
            j = i + 1
            # Extend as far as the straight segment stays on the corridor.
            while j + 1 < len(pts) and not keep[j] and self._walkable(pts[i], pts[j + 1], allowed):
                j += 1
            out.append(pts[j])
            i = j
        return out

    def _neighbours(self, t):
        cache = self.__dict__.setdefault("_nbr", {})
        if t not in cache:
            cache[t] = {n for n, _c, kind, _p in self.links[t] if kind in ("walk", "seam")}
        return cache[t]

    def _walkable(self, p, q, allowed, step=0.5, max_dy=1.0):
        """True if walking straight from p to q stays on one connected stretch of the corridor's
        floor: every sample close to the line's height, on the same triangle as the last one or
        a neighbour of it. Without the neighbour rule a line through a stairwell or between
        stacked floors passes, because some floor is always near it."""
        n = max(2, int(math.dist(p, q) / step))
        cur = None
        for k in range(n + 1):
            f = k / n
            x = p[0] + (q[0] - p[0]) * f
            y = p[1] + (q[1] - p[1]) * f
            z = p[2] + (q[2] - p[2]) * f
            here = [t for t in self.grid.get((int(x // 4), int(z // 4)), [])
                    if t in allowed and self._contains(t, x, z) and abs(self.height(t, x, z) - y) <= max_dy]
            if not here:
                if k in (0, n):
                    continue  # the ends sit on portal edges, between triangles
                return False
            if cur in here:
                continue
            if cur is None:
                cur = min(here, key=lambda t: abs(self.height(t, x, z) - y))
                continue
            nb = self._neighbours(cur)
            nxt = [t for t in here if t in nb]
            if not nxt:
                return False
            cur = nxt[0]
        return True


def resample(points, spacing=8.0):
    """Adds points along long straight runs so the overlay always has a nearby target."""
    out = [points[0]]
    for p, q in zip(points, points[1:]):
        n = int(math.dist(p, q) // spacing)
        for k in range(1, n + 1):
            f = k / (n + 1)
            out.append(tuple(a + (b - a) * f for a, b in zip(p, q)))
        out.append(q)
    return out
