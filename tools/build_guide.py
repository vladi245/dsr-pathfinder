"""Builds guide.txt for DSR Pathfinder from Dark Souls Remastered's own game files.

For every achievement item (all sorceries, pyromancies, miracles and the Knight's Honor weapons)
it finds where the item comes from: a pickup on the map, an enemy drop, a shop, or a boss-soul
recipe. Pickups and drops become steps with an exact destination (`goal`) and the pickup's event
flag, so the overlay points at the item and ticks the step off when you grab it. Non-item
achievements (covenants, bosses, endings) come from tools/milestones.txt.

Usage:
    python tools/build_guide.py [--game DIR] [--out guide.txt] [--report] [--local]

Waypoints you've recorded with F7 are kept when you regenerate: steps are matched by their `id`.
"""

import argparse
import math
import os
import re
import sys
from collections import defaultdict
from dataclasses import dataclass, field

sys.path.insert(0, os.path.dirname(__file__))
import dsr_formats as fmt  # noqa: E402
import navmesh  # noqa: E402
import walkthrough  # noqa: E402

ROUTE_SPACING = 8.0    # metres between route points on long straight stretches
ROUTE_MAX_GAP = 25.0   # a route that stops short by more than this isn't used (lift, warp...)

DEFAULT_GAME = r"C:\Program Files (x86)\Steam\steamapps\common\DARK SOULS REMASTERED"
HERE = os.path.dirname(os.path.abspath(__file__))

# Maps in the order a typical achievement run visits them.
MAPS = [
    ("m18_01_00_00", "Northern Undead Asylum"),
    ("m10_02_00_00", "Firelink Shrine"),
    ("m10_01_00_00", "Undead Burg & Undead Parish"),
    ("m12_00_00_01", "Darkroot Garden & Darkroot Basin"),
    ("m10_00_00_00", "The Depths"),
    ("m14_00_00_00", "Blighttown & Quelaag's Domain"),
    ("m15_00_00_00", "Sen's Fortress"),
    ("m15_01_00_00", "Anor Londo"),
    ("m11_00_00_00", "Painted World of Ariamis"),
    ("m16_00_00_00", "New Londo Ruins, Valley of Drakes & The Abyss"),
    ("m13_02_00_00", "The Great Hollow & Ash Lake"),
    ("m17_00_00_00", "Duke's Archives & Crystal Cave"),
    ("m14_01_00_00", "Demon Ruins & Lost Izalith"),
    ("m13_00_00_00", "The Catacombs"),
    ("m13_01_00_00", "Tomb of the Giants"),
    ("m12_01_00_00", "Oolacile (DLC)"),
    ("m18_00_00_00", "Kiln of the First Flame"),
]

CAT_WEAPON, CAT_ARMOR, CAT_RING, CAT_GOODS = 0x00000000, 0x10000000, 0x20000000, 0x40000000
SPELL_ACHIEVEMENT = [(3000, 3999, "Wisdom of a Sage"), (4000, 4999, "Bond of a Pyromancer"),
                     (5000, 5999, "Prayer of a Maiden")]
SHARED_MODELS = {2230, 2231, 2232}

# Where to go for steps outside the walkthrough, in the tools/step_spots.txt syntax.
SHOP_SPOTS = {
    1100: "Undead Merchant in m10_01_00_00", 1400: "Andre in m10_01_00_00",
    1600: "Patches in m10_02_00_00", 1700: "Shiva in m14_00_00_00",
    2000: "Griggs in m10_02_00_00", 2100: "Rickert in m16_00_00_00",
    2200: "Dusk of Oolacile in m12_00_00_01", 2400: "Ingward in m16_00_00_00",
    3000: "Laurentius in m10_02_00_00", 3200: "Fair Lady in m14_01_00_00",  # Eingyi sits by her
    3400: "Quelana in m14_00_00_00", 4000: "Petrus in m10_02_00_00", 4200: "Reah in m10_01_00_00",
    4400: "Oswald in m10_01_00_00", 5000: "Logan in m17_00_00_00",
    6200: "Giant Blacksmith in m15_01_00_00", 6500: "Elizabeth in m12_01_00_00",
}
RECIPE_SPOT = "Giant Blacksmith in m15_01_00_00"
# Covenant rewards and other event items, by a phrase in their how-to text: the covenant's leader
# or altar. Gravelord Servant is joined through the coffin (object o3060) in the Catacombs.
EVENT_SPOTS = {"Chaos Servant": "Fair Lady in m14_01_00_00",
               "Blade of the Darkmoon": "Dark Sun Gwyndolin in m15_01_00_00",
               "Gravelord Servant": "<o3060> in m13_00_00_00",
               "Warrior of Sunlight": "bonfire near 13.58, 32.47, -8.65 in m10_01_00_00",
               "Snuggly": "-11.2, 200.7, -69.0"}

# Spells left in the game data but never obtainable, so the achievements don't count them.
CUT_SPELLS = {5200}  # Escape Death


@dataclass
class Target:
    key: tuple          # ("goods", id) or ("weapon", base id)
    name: str
    achievement: str
    ids: list = field(default_factory=list)       # weapon variants that all count
    pickups: list = field(default_factory=list)   # (map, pos, flag, container)
    drops: list = field(default_factory=list)     # (map, pos, flag, enemy name)
    shops: list = field(default_factory=list)     # (group, price)
    recipe: str = ""                              # boss-soul ascension
    ambiguous: bool = False                       # name shared with another kind of item


# ------------------------------------------------------------------------------ data files

PARAMDEX_NPC_NAMES = "https://raw.githubusercontent.com/soulsmods/Paramdex/master/DS1R/Names/NpcParam.txt"
# Too generic to match walkthrough text safely ("Hollow" is also in "Great Hollow").
PARAMDEX_SKIP = {"Bonfire", "Hollow", "Ghost", "Skeleton", "Darkwraith", "Forest Hunter", "Mass Of Souls",
                 "Humanity Phantom"}


def fetch_paramdex_names():
    """Downloads English NPC row names from the modding community's Paramdex into
    tools/npc_param_names.txt, once. The file isn't shipped: Paramdex states no license."""
    path = os.path.join(HERE, "npc_param_names.txt")
    if os.path.exists(path):
        return
    import urllib.request
    print("Downloading NPC names from Paramdex (github.com/soulsmods/Paramdex) ...")
    try:
        text = urllib.request.urlopen(PARAMDEX_NPC_NAMES, timeout=30).read().decode("utf-8")
    except OSError as e:
        print(f"  failed ({e}); continuing with tools/npc_names.txt only")
        return
    out = ["# English names for the game's NpcParam rows (id = name), downloaded from Paramdex",
           "# (github.com/soulsmods/Paramdex, DS1R/Names/NpcParam.txt) by build_guide.py. Variant notes",
           "# are cut, and names too generic to match walkthrough text safely are left out.",
           "# npc_names.txt overrides these where Paramdex is wrong or the walkthrough uses a shorter name."]
    for line in text.splitlines():
        row, _, name = line.strip().partition(" ")
        name = re.sub(r"\s*[\(\[\-].*", "", name).strip()
        if row.isdigit() and name and name.isascii() and name not in PARAMDEX_SKIP and \
                not re.search(r"test|dummy", name, re.I):
            out.append(f"{row} = {name}")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")


def read_lines(name):
    for line in open(os.path.join(HERE, name), encoding="utf-8-sig"):
        line = line.strip()
        if line and not line.startswith("#"):
            yield line


def load_weapon_list():
    out, section = [], None
    for line in read_lines("achievement_items.txt"):
        m = re.match(r"\[(.+)\]", line)
        if m:
            section = m.group(1)
            continue
        name, _, ids = line.partition("=")
        out.append((section, name.strip(), [int(i) for i in re.findall(r"\d+", ids)]))
    return out


def load_key_values(name, sep="="):
    out = {}
    for line in read_lines(name):
        k, _, v = line.partition(sep)
        out[k.strip()] = v.strip()
    return out


def load_milestones():
    out = defaultdict(list)
    for line in read_lines("milestones.txt"):
        parts = [p.strip() for p in line.split("|")]
        if len(parts) == 3:
            out[parts[0]].append((parts[1], parts[2]))
    return out


# ------------------------------------------------------------------------------ extraction

def lot_chain(lots, lot_id):
    """A lot and the consecutive rows after it are awarded together."""
    rows = []
    while lot_id in lots and len(rows) < 10:
        rows.append(lots[lot_id])
        lot_id += 1
    return rows


def lot_items(row):
    for i in range(1, 9):
        item, cat = row[f"lotItemId0{i}"], row[f"lotItemCategory0{i}"]
        if item > 0:
            yield cat, item


def item_key(cat, item):
    if cat == CAT_WEAPON:
        return ("weapon", item - item % 1000)  # strip upgrade level and infusion
    if cat == CAT_GOODS:
        return ("goods", item)
    return None


def container(treasure_name):
    if "宝箱" in treasure_name:
        return "chest"
    if "死体" in treasure_name:
        return "corpse"
    return "item"


def with_article(name):
    return ("an " if name[0].upper() in "AEIOU" else "a ") + name


def build(game, world=False):
    P = fmt.load_params(game, ["ItemLotParam", "NpcParam", "ShopLineupParam", "EquipMtrlSetParam",
                               "EquipParamWeapon", "ObjActParam"])
    T = fmt.load_texts(game)
    lots, weapons = P["ItemLotParam"], P["EquipParamWeapon"]
    goods_names, weapon_names = T["Item_name_"], T["Weapon_name_"]

    # --- targets; `alias` maps every counting item key to its target
    targets, alias = {}, {}
    for spell_id, name in T["Magic_name_"].items():
        for lo, hi, ach in SPELL_ACHIEVEMENT:
            if lo <= spell_id <= hi and name and spell_id not in CUT_SPELLS:
                key = ("goods", spell_id)
                targets[key] = Target(key, name, ach)
                alias[key] = key
    ids_by_name = defaultdict(list)
    for wid, name in sorted(weapon_names.items()):
        if wid % 1000 == 0 and name and wid in weapons:
            ids_by_name[name].append(wid)
    missing = []
    for section, name, ids in load_weapon_list():
        ids = ids or ids_by_name.get(name, [])
        if not ids:
            missing.append(name)
            continue
        key = ("weapon", ids[0])
        targets[key] = Target(key, name, section, ids)
        for i in ids:
            alias[("weapon", i)] = key

    def target_for(cat, item):
        return targets.get(alias.get(item_key(cat, item)))

    lot_to_npcs = defaultdict(list)
    for npc_id, npc in P["NpcParam"].items():
        for i in range(1, 7):
            if npc[f"itemLotId_{i}"] > 0:
                lot_to_npcs[npc[f"itemLotId_{i}"]].append(npc_id)
    npc_names = T.get("NPC_name_", {})
    npc_english = {}
    fetch_paramdex_names()
    for name in ("npc_param_names.txt", "npc_names.txt"):  # the second overrides the first
        npc_english.update({int(k): v for k, v in load_key_values(name).items() if k.isdigit()})

    tables = {CAT_WEAPON: weapon_names, CAT_ARMOR: T["Armor_name_"], CAT_RING: T["Accessory_name_"],
              CAT_GOODS: goods_names}

    def item_name(cat, item):
        table = tables.get(cat & 0xF0000000, {})
        name = table.get(item) or table.get(item - item % 100) or table.get(item - item % 1000)
        # Spells are stored as "Pyromancy: Power Within"; guides just say "Power Within".
        return re.sub(r"^(Sorcery|Pyromancy|Miracle): ", "", name) if name else name

    # Every locatable thing by name, for matching walkthrough text: all pickups, one-time
    # drops, bosses (by character model, e.g. c2250 = Taurus Demon) and named NPCs.
    places = defaultdict(list)

    def remember(cat, item, stem, pos, flag):
        name = item_name(cat, item)
        if name:
            kind = "armor" if cat & 0xF0000000 == CAT_ARMOR else "item"
            places[name].append(walkthrough.Place(name, stem, pos, flag, kind))
            places[walkthrough.LOOT].append(walkthrough.Place(name, stem, pos, flag, kind))

    # --- pickups and enemy drops from every map
    for stem, _area in MAPS:
        msb = fmt.read_msb(os.path.join(game, "map", "MapStudio", stem + ".msb"))
        off = msb.offset if world else (0.0, 0.0, 0.0)

        def place(p):
            return tuple(a + b for a, b in zip(p, off))

        for t in msb.treasures:
            if not 0 <= t.part < len(msb.parts):
                continue
            pos = place(msb.parts[t.part].pos)
            for lot in t.item_lots:
                for row in lot_chain(lots, lot):
                    for cat, item in lot_items(row):
                        remember(cat, item, stem, pos, row["getItemFlagId"])
                        tgt = target_for(cat, item)
                        if tgt:
                            tgt.pickups.append((stem, pos, row["getItemFlagId"], container(t.name)))
        # Landmarks for steps that name nothing: bonfires (model o0200) and anything you can
        # open or pull, whose model number is its ObjActParam row (o8540 = an Asylum door).
        for part in msb.parts:
            m = re.match(r"o(\d{4})_", part.name)
            if m and m.group(1) == "0200":
                places[walkthrough.BONFIRE].append(walkthrough.Place("bonfire", stem, place(part.pos)))
            elif m and int(m.group(1)) >= 1000 and int(m.group(1)) in P["ObjActParam"]:
                places[walkthrough.MECHANISM].append(walkthrough.Place("door or lever", stem, place(part.pos)))
                places[f"<o{m.group(1)}>"].append(walkthrough.Place(part.name, stem, place(part.pos)))
        enemies = defaultdict(list)
        for part in msb.parts:
            if part.type not in (fmt.PART_ENEMY, fmt.PART_DUMMY_ENEMY):
                continue
            enemies[part.npc_param].append(part)
            # Every name the game data gives: the enemy's row, its name id and its model. The demon
            # models c2230-c2232 are shared by the Asylum Demon, Stray Demon and Demon Firesage,
            # so their model name can't be trusted.
            row = P["NpcParam"].get(part.npc_param, {})
            names = {npc_english.get(part.npc_param), npc_names.get(row.get("nameId"))}
            m = re.match(r"c(\d{4})_", part.name)
            if m and int(m.group(1)) not in SHARED_MODELS:
                names.add(npc_names.get(int(m.group(1))))
            names.discard(None)
            for name in names:
                places[name].append(walkthrough.Place(name, stem, place(part.pos), 0, "character"))
        for lot_id, npc_ids in lot_to_npcs.items():
            for row in lot_chain(lots, lot_id):
                for cat, item in lot_items(row):
                    tgt = target_for(cat, item)
                    for npc_id in npc_ids:
                        name = npc_names.get(P["NpcParam"][npc_id]["nameId"], "")
                        for part in enemies.get(npc_id, []):
                            if row["getItemFlagId"] > 0:
                                remember(cat, item, stem, place(part.pos), row["getItemFlagId"])
                            if tgt:
                                tgt.drops.append((stem, place(part.pos), row["getItemFlagId"], name))

    # One spot in Firelink holds every key, ember and Estus Flask: where the game parks items it
    # hands out by script. A spot with that many different items isn't somewhere to walk to.
    crowd = defaultdict(set)
    for name, ps in places.items():
        for p in ps:
            if p.kind in ("item", "armor"):
                crowd[(p.stem, p.pos)].add(name)
    parked = {spot for spot, names in crowd.items() if len(names) > 12}
    for name in list(places):
        places[name] = [p for p in places[name] if p.kind not in ("item", "armor") or (p.stem, p.pos) not in parked]

    # --- shops
    for row_id, row in P["ShopLineupParam"].items():
        if row["shopType"] != 0:  # 0 = buy; others are attunement / upgrade menus
            continue
        cat = {0: CAT_WEAPON, 3: CAT_GOODS}.get(row["equipType"])
        tgt = target_for(cat, row["equipId"]) if cat is not None else None
        if tgt:
            tgt.shops.append((row_id // 100 * 100, row["value"]))

    # --- boss-soul recipes: a weapon made by ascending another weapon with a soul
    for tgt in targets.values():
        sources, soul = [], None
        for wid in tgt.ids:
            w = weapons[wid]
            origin, mset = w["originEquipWep"], P["EquipMtrlSetParam"].get(w["materialSetId"])
            if origin > 0 and origin - origin % 1000 != wid and mset and mset["materialId01"] > 0:
                soul = goods_names.get(mset["materialId01"], f"item {mset['materialId01']}")
                src = weapon_names.get(origin) or weapon_names.get(origin - origin % 1000, str(origin))
                src = re.sub(r"\+(\d+)$", r" +\1", src)
                if src not in sources:
                    sources.append(src)
        if sources:
            options = " or ".join(with_article(s) for s in sources)
            tgt.recipe = f"Ascend {options} with the {soul} at the Giant Blacksmith in Anor Londo."

    # Items handed over by a script somewhere other than their owner's spawn.
    for item, owner in load_key_values("place_overrides.txt").items():
        old = places.get(item, [])
        spot = re.match(r"^(-?[\d.]+),\s*(-?[\d.]+),\s*(-?[\d.]+)$", owner)
        if owner == "none":  # no useful position at all
            places.pop(item, None)
        elif spot and old:  # a fixed position: where you actually go to use this
            places[item] = [walkthrough.Place(item, old[0].stem, tuple(float(c) for c in spot.groups()),
                                              old[0].flag, old[0].kind)]
        elif old and places.get(owner):
            places[item] = [walkthrough.Place(item, p.stem, p.pos, old[0].flag, "item") for p in places[owner][:1]]

    trophy = sorted(weapon_names.get(i, str(i)) for i, w in weapons.items()
                    if i % 1000 == 0 and w["trophySGradeId"] > 0 and ("weapon", i) not in alias)
    # Names used by more than one kind of item (the miracle Lightning Spear vs. a lightning
    # infused Spear) can't prove which one a walkthrough means.
    item_names, seen_in = set(), defaultdict(set)
    for cat, table in tables.items():
        for n in table.values():
            if n:
                n = re.sub(r"^(Sorcery|Pyromancy|Miracle): ", "", n)
                item_names.add(n)
                seen_in[n].add(cat)
    for t in targets.values():
        t.ambiguous = len(seen_in.get(t.name, ())) > 1
    return targets, missing, trophy, places, item_names, set(npc_names.values()) | set(npc_english.values())


# ------------------------------------------------------------------------------ output

def plan(tgt):
    """Picks how the guide should send you to an item. Returns (kind, entries)."""
    def first_per_flag(entries):
        order = {stem: i for i, (stem, _) in enumerate(MAPS)}
        seen, out = set(), []
        for e in sorted(entries, key=lambda e: order[e[0]]):
            if e[2] > 0 and e[2] in seen:
                continue
            seen.add(e[2])
            out.append(e)
        return out

    if tgt.pickups:
        return "pickup", first_per_flag(tgt.pickups)
    if tgt.shops:
        return "shop", tgt.shops
    unique = [d for d in tgt.drops if d[2] > 0]
    if unique:
        return "drop", first_per_flag(unique)
    if tgt.recipe:
        return "recipe", []
    if tgt.drops:
        # Random drop from common enemies: one farming step where they're most numerous.
        per_map = defaultdict(list)
        for d in tgt.drops:
            per_map[d[0]].append(d)
        best = max(per_map.values(), key=len)
        return "farm", [best[0] + (len(best),)]
    return "other", []


def nearest_order(steps):
    """Greedy nearest-neighbour ordering so a map's steps form a walkable sequence."""
    if not steps:
        return steps
    steps = sorted(steps, key=lambda s: s["pos"])
    left, out = steps[1:], [steps[0]]
    while left:
        nxt = min(left, key=lambda s: math.dist(out[-1]["pos"], s["pos"]))
        left.remove(nxt)
        out.append(nxt)
    return out


def recorded_waypoints(path):
    """step id -> recorded `wp =` lines, so regenerating doesn't lose F7 routes."""
    out, current = defaultdict(list), None
    if not os.path.exists(path):
        return out
    for line in open(path, encoding="utf-8-sig"):
        s = line.strip()
        if s == "[step]":
            current = None
        elif s.startswith("id ="):
            current = s.split("=", 1)[1].strip()
        elif s.startswith("wp =") and current:
            out[current].append(s)
    return out


def covered_by(tgt, walk_steps):
    """True if the walkthrough already sends you to this achievement item."""
    flags = {p.flag for s in walk_steps for p in s.goals if p.flag > 0}
    names = {n for s in walk_steps for n in s.names}
    if tgt.name in names and not tgt.ambiguous:
        return True
    return any(e[2] in flags for e in tgt.pickups + tgt.drops if e[2] > 0)


def walking_route(world, start, goal):
    """Route points strictly between start and goal, or [] when there's no sensible walk."""
    if world is None or start is None:
        return []
    pts, complete = world.route(start, goal)
    if not pts or len(pts) < 2:
        return []
    if not complete and math.dist(pts[-2], goal) > ROUTE_MAX_GAP:
        return []  # meshes don't connect: a lift, crow, painting or warp
    length = sum(math.dist(p, q) for p, q in zip(pts, pts[1:]))
    if length > 3 * math.dist(start, goal) + 200:
        return []  # a huge detour means the guide warps here rather than walks
    return navmesh.resample(pts, ROUTE_SPACING)[1:-1]


def spot(places, expr):
    """Where a tools/step_spots.txt style expression points, or None."""
    return next((p.pos for p in walkthrough.resolve_spot(expr, places)), None)


def write_guide(targets, out_path, walk_steps=None, world=None, places=None):
    shop_names = {int(k): v for k, v in load_key_values("shops.txt").items() if k.isdigit()}
    hints = load_key_values("item_hints.txt")
    milestones = load_milestones()
    keep = recorded_waypoints(out_path)
    notes = defaultdict(list)  # step id -> extra text lines (tools/step_notes.txt)
    for line in read_lines("step_notes.txt"):
        sid, _, text = line.partition("=")
        notes[sid.strip()].append(text.strip())

    L = ["# DSR Pathfinder achievement guide, generated by tools/build_guide.py from your game files.",
         "# Regenerating keeps waypoints you recorded with F7 (matched by each step's id).",
         "# goal = destination from the game data; path = walking route to it, computed from the",
         "# game's navigation meshes; wp = your own recorded route (replaces path for that step).",
         ""]
    last = {"pos": None}  # where the previous destination was, to route from

    def step(sid, title, text_lines, flags=(), goals=()):
        L.extend(["[step]", f"id = {sid}", f"title = {title}"])
        L.extend(f"text = {t}" for t in list(text_lines) + notes.get(sid, []))
        flags = [f for f in dict.fromkeys(flags) if f > 0]
        if flags:
            L.append("flag = " + ", ".join(str(f) for f in flags))
        L.extend(keep.get(sid, []))
        # Set pieces often share one chest: one goal per spot.
        for g in dict.fromkeys(tuple(round(c, 2) for c in g) for g in goals):
            for p in walking_route(world, last["pos"], g):
                L.append("path = {:.2f}, {:.2f}, {:.2f}".format(*p))
            L.append("goal = {:.2f}, {:.2f}, {:.2f}".format(*g))
            last["pos"] = g
        L.append("")

    def milestone_steps(key):
        for n, (title, text) in enumerate(milestones.get(key, []), 1):
            step(f"milestone-{key}-{n}", title, [t.strip() for t in text.split(" / ")])

    by_name = {t.name: t for t in targets.values()}
    if walk_steps:
        L[-1:-1] = ["# Step order and text: hundodex Dark Souls walkthrough",
                    "# (darksouls.hundodex.com/journey/walkthrough)."]
        section = None
        for s in walk_steps:
            if s.section != section:
                section = s.section
                L.extend([f"# ===== {section} =====", ""])
            text = [s.text]
            achievements = sorted({by_name[x].achievement for x in s.names if x in by_name})
            if achievements:
                text.append("Counts toward " + " and ".join(achievements) + ".")
            # Keyed by section and step number, so editing another section keeps your waypoints.
            sid = "wt-" + re.sub(r"\W+", "-", s.section.lower()).strip("-") + f"-{s.number}"
            step(sid, f"{s.section} ({s.number}/{s.total})", text,
                 [p.flag for p in s.goals], [p.pos for p in s.goals])
        targets = {k: t for k, t in targets.items() if not covered_by(t, walk_steps)}
        if targets:
            L.extend(["# ===== Achievement items the walkthrough doesn't send you to =====", ""])
        write_items(targets, L, step, shop_names, hints, None, places or {})
        if milestones.get("extra"):
            L.extend(["# ===== Wrapping up =====", ""])
            milestone_steps("extra")
    else:
        write_items(targets, L, step, shop_names, hints, milestone_steps, places or {})
        L.extend(["# ===== Wrapping up =====", ""])
        milestone_steps("end")
        milestone_steps("extra")

    with open(out_path, "w", encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(L))
    return sum(1 for line in L if line.startswith("goal")), len(targets)


def write_items(targets, L, step, shop_names, hints, milestone_steps, places):
    """Appends steps for achievement items, by area, then shops, recipes and event rewards."""
    per_map, by_shop, recipes, other = defaultdict(list), defaultdict(list), [], []
    for tgt in sorted(targets.values(), key=lambda t: (t.achievement, t.name)):
        kind, entries = plan(tgt)
        if kind == "farm" and tgt.name in hints:
            kind = "other"  # e.g. Dark Hand: joining the covenant beats farming a rare drop
        if kind in ("pickup", "drop", "farm"):
            for n, e in enumerate(entries, 1):
                per_map[e[0]].append({"tgt": tgt, "kind": kind, "pos": e[1], "flag": e[2],
                                      "extra": e[3], "count": e[4] if len(e) > 4 else 0,
                                      "id": f"{tgt.key[0]}-{tgt.key[1]}-{e[0]}-{n}"})
        elif kind == "shop":
            group, price = min(entries, key=lambda s: s[1])
            by_shop[group].append((tgt, price))
        elif kind == "recipe":
            recipes.append(tgt)
        else:
            other.append(tgt)

    for stem, area in MAPS:
        steps = nearest_order(per_map.get(stem, []))
        if not steps and not (milestone_steps and load_milestones().get(stem)):
            continue
        L.extend([f"# ===== {area} =====", ""])
        for s in steps:
            t = s["tgt"]
            if s["kind"] == "pickup":
                how = {"chest": "Open the chest", "corpse": "Loot the corpse"}.get(s["extra"], "Pick it up")
                text = [f"{how} here. Counts toward {t.achievement}."]
                title = f"{area}: {t.name}"
            elif s["kind"] == "drop":
                text = [f"Dropped by {s['extra'] or 'the enemy here'}. Counts toward {t.achievement}."]
                title = f"{area}: {t.name}"
            else:
                who = s["extra"] or "enemies of this type"
                text = [f"Random drop from {who} ({s['count']} in this area). Farm until it drops.",
                        f"Counts toward {t.achievement}."]
                title = f"Farm: {t.name}"
            if t.shops and s["kind"] != "farm":
                sellers = sorted({shop_names.get(g, f"shop {g}") for g, _ in t.shops})
                text.append("Also sold by " + ", ".join(sellers) + ".")
            step(s["id"], title, text, [s["flag"]], [s["pos"]])
        if milestone_steps:
            milestone_steps(stem)

    if by_shop:
        L.extend(["# ===== Shops =====", ""])
    for group, items in sorted(by_shop.items()):
        seller = shop_names.get(group, f"shop group {group}")
        at = spot(places, SHOP_SPOTS[group]) if group in SHOP_SPOTS else None
        step(f"shop-{group}", f"Buy from {seller}",
             [f"{t.name}: {price} souls ({t.achievement})" for t, price in items], goals=[at] if at else ())
    if recipes:
        L.extend(["# ===== Boss soul weapons =====", ""])
        for t in recipes:
            at = spot(places, RECIPE_SPOT)
            step(f"recipe-{t.key[1]}", f"Craft: {t.name}", [t.recipe, "Counts toward Knight's Honor."],
                 goals=[at] if at else ())
    if other:
        L.extend(["# ===== Covenant rewards, tail cuts and other events =====", ""])
        for t in other:
            how = hints.get(t.name, "Not a pickup, drop or shop item. Fill in from your guide.")
            at = next((spot(places, where) for phrase, where in EVENT_SPOTS.items() if phrase in how), None)
            step(f"other-{t.key[0]}-{t.key[1]}", f"Obtain: {t.name}", [how, f"Counts toward {t.achievement}."],
                 goals=[at] if at else ())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", default=DEFAULT_GAME, help="Dark Souls Remastered install folder")
    ap.add_argument("--out", default=os.path.join(HERE, "..", "guide.txt"))
    ap.add_argument("--report", action="store_true", help="print where every item was found")
    ap.add_argument("--map-offsets", action="store_true",
                    help="add the maps' MapOffset values to positions (off: MSB positions are already "
                         "world coordinates, which is what makes neighbouring maps' navmeshes meet)")
    ap.add_argument("--no-routes", action="store_true", help="skip computing walking routes")
    ap.add_argument("--walkthrough", default=None,
                    help="step list to follow (default tools/walkthrough.txt if present; '' to ignore)")
    args = ap.parse_args()

    targets, missing, trophy, places, item_names, character_names = build(args.game, world=args.map_offsets)
    if missing:
        print("Names in achievement_items.txt not found in the game:", ", ".join(missing))
    if args.report:
        names = dict(MAPS)
        for t in sorted(targets.values(), key=lambda t: (t.achievement, t.name)):
            kind, entries = plan(t)
            where = "; ".join(f"{names[e[0]]} flag {e[2]}" for e in entries) if entries and kind != "shop" \
                else ", ".join(f"shop {g} ({p})" for g, p in entries) if kind == "shop" else t.recipe
            print(f"{t.achievement:22} {t.name:34} {kind:7} {where}")
        print("\nWeapons the game flags as trophy items but the list doesn't include:",
              ", ".join(trophy) or "none")

    walk_steps = None
    walk_path = args.walkthrough if args.walkthrough is not None else os.path.join(HERE, "walkthrough.txt")
    if walk_path and os.path.exists(walk_path):
        walk_steps = walkthrough.build_steps(walk_path, places, item_names, character_names,
                                             load_key_values("step_spots.txt"))
        located = sum(1 for s in walk_steps if s.goals)
        print(f"Walkthrough: {len(walk_steps)} steps, {located} with a destination from the game data")
        if args.report:
            for s in walk_steps:
                if s.goals:
                    print(f"  {s.section[:20]:20} {s.number:>2}  " +
                          ", ".join(f"{p.name}" for p in s.goals))

    world = None
    if not args.no_routes:
        world = navmesh.World()
        world.load(args.game, [stem for stem, _ in MAPS], world_offsets=args.map_offsets)
        print(f"Navmesh: {len(world.tri)} walkable triangles")
        # The overlay routes from wherever you are with these (after dying, a Homeward Bone...).
        nav_path = os.path.join(os.path.dirname(os.path.abspath(args.out)), "navmesh.bin")
        world.export(nav_path)
        print(f"Wrote {nav_path} ({os.path.getsize(nav_path) >> 20} MB)")

    goals, extra = write_guide(targets, os.path.abspath(args.out), walk_steps, world, places)
    text = open(os.path.abspath(args.out), encoding="utf-8").read()
    routed = text.count("\npath = ")
    print(f"Wrote {os.path.abspath(args.out)}: {goals} destinations, {routed} route points; "
          f"{extra} achievement items {'added after the walkthrough' if walk_steps else 'in total'}")


if __name__ == "__main__":
    main()
