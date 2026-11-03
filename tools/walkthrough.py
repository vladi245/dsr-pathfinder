"""Turns a step-by-step walkthrough (tools/walkthrough.txt) into guide steps, attaching
destinations and pickup flags wherever a step names something the game data can locate:
items, armor sets, bosses and named NPCs.

Walkthrough format: "N. Section name" headers (followed by "1. ..." steps), numbered steps,
and "Part ..." lines, which are ignored.
"""

import re
from collections import defaultdict
from dataclasses import dataclass, field

# Which maps each walkthrough section moves through, to tell apart items that exist in
# several places. Matched against the section name; first match wins.
SECTION_MAPS = [
    ("Asylum", ["m18_01_00_00"]),
    ("Firelink", ["m10_02_00_00", "m16_00_00_00", "m12_00_00_01", "m10_01_00_00", "m18_01_00_00"]),
    ("Lower Undead Burg", ["m10_01_00_00", "m10_02_00_00"]),
    ("Undead Burg", ["m10_01_00_00"]),
    ("Undead Parish", ["m10_01_00_00", "m10_02_00_00"]),
    ("Depths", ["m10_00_00_00"]),
    ("Blighttown", ["m14_00_00_00", "m16_00_00_00", "m10_02_00_00"]),
    ("Darkroot", ["m12_00_00_01", "m10_01_00_00", "m16_00_00_00", "m14_00_00_00"]),
    ("Sen's", ["m15_00_00_00"]),
    ("Anor Londo", ["m15_01_00_00", "m10_02_00_00"]),
    ("New Londo", ["m16_00_00_00", "m14_00_00_00"]),
    ("Great Hollow", ["m13_02_00_00"]),
    ("Ash Lake", ["m13_02_00_00"]),
    ("Painted World", ["m11_00_00_00", "m15_01_00_00"]),
    ("Archives", ["m17_00_00_00"]),
    ("Crystal Cave", ["m17_00_00_00", "m10_02_00_00"]),
    ("Demon Ruins", ["m14_01_00_00"]),
    ("Izalith", ["m14_01_00_00", "m10_02_00_00", "m13_02_00_00"]),
    ("Catacombs", ["m13_00_00_00"]),
    ("Tomb", ["m13_01_00_00"]),
    ("Miscellaneous", ["m10_02_00_00", "m18_01_00_00", "m10_01_00_00", "m13_00_00_00", "m15_01_00_00"]),
    ("Sanctuary", ["m12_01_00_00", "m12_00_00_01"]),
    ("Oolacile", ["m12_01_00_00"]),
    ("Chasm", ["m12_01_00_00"]),
    ("Kiln", ["m18_00_00_00"]),
]

# Short names the walkthrough uses for bosses and NPCs, mapped to their in-game names.
ALIASES = {
    "Ornstein": "Dragon Slayer Ornstein", "Smough": "Executioner Smough",
    "Priscilla": "Crossbreed Priscilla", "Sif": "Great Grey Wolf Sif", "Quelaag": "Chaos Witch Quelaag",
    "Seath": "Seath the Scaleless", "Nito": "Gravelord Nito", "Gwyndolin": "Dark Sun Gwyndolin",
    "Gwyn": "Gwyn, Lord of Cinder", "Manus": "Manus, Father of the Abyss",
    "Kalameet": "Black Dragon Kalameet", "Artorias": "Knight Artorias", "Mildred": "Maneater Mildred",
    "Jeremiah": "King Jeremiah", "Kirk": "Knight Kirk", "Tarkus": "Iron Tarkus",
    "Beatrice": "Witch Beatrice", "Leeroy": "Paladin Leeroy", "Gaping Dragon": "Gaping Dragon",
    "Hellkite": "Hellkite Drake", "Sentinel": "Royal Sentinel", "Lautrec": "Knight Lautrec",
    "Titanite Demon": "Prowling Demon", "Blowdart Sniper": "Blowdart Hollow",
    "Wheel Skeleton": "Bonewheel Skeleton", "Engorged Zombie": "Engorged Hollow",
    "Stone Demon": "Demonic Statue", "Everlasting Dragon": "Stone Dragon", "forest cat": "Great Feline",
    "Firesage": "Firesage Demon", "armored boar": "Armored Tusk", "Man-Eater Shell": "Maneater Clam",
    "Solaire": "Knight Solaire", "Dusk": "Dusk of Oolacile", "Big Hat Logan": "Logan",
}


# Keys in `places` for landmarks that have no name of their own.
BONFIRE, MECHANISM, LOOT = "<bonfire>", "<door or lever>", "<loot>"
# For a step that names nothing, words that say what kind of landmark it's about. Loot words
# must also appear in the item's name.
LANDMARK_WORDS = [(BONFIRE, r"bonfire"), (MECHANISM, r"lever|door|gate|shortcut|unlock|mechanism|switch"),
                  (LOOT, r"humanit|titanite|ember|soul|moss|key")]
MAX_DETOUR = 100.0  # metres a guessed landmark may add to the walk from the last spot to the next

# Phrases that contain an item name but aren't the item (e.g. the NPC Big Hat Logan).
MASKED = ["Fire Keeper Soul near the dogs"]

# The verb governing a name decides whether the step sends you to it. "Collect the X" and
# "defeat X" do; "give him the X", "buy the X", "with the X" mean you already have it.
GO_VERBS = {"collect", "retrieve", "obtain", "find", "get", "loot", "receive", "pick", "search",
            "defeat", "kill", "cut", "reach", "activate", "summon", "explore", "gather", "eliminate",
            "encounter", "fight", "challenge", "destroy", "rescue", "free", "meet", "enter"}
USE_VERBS = {"give", "deliver", "buy", "purchase", "exchange", "carrying", "use", "using", "upgrade",
             "offer", "equip", "with", "request", "trade", "wearing", "against", "speak", "examine"}
# A name after these is only what the step is about ("ask Petrus about Reah", "watch for the
# Hellkite Drake"), never a place.
TOPIC_WORDS = {"about", "regarding", "watch", "beware", "avoid"}


def governing_verb(text, pos):
    """The last go/use verb before `pos` in the same sentence."""
    for word in reversed(re.findall(r"[a-z]+", text[:pos].lower())):
        if word.endswith("ing") and word not in USE_VERBS:
            word = next((v for v in GO_VERBS | USE_VERBS if word in (v + "ing", v[:-1] + "ing")), word)
        if word in GO_VERBS or word in USE_VERBS or word in TOPIC_WORDS:
            return word
    return None


@dataclass
class Place:
    name: str
    stem: str
    pos: tuple
    flag: int = 0      # pickup flag; 0 for characters
    kind: str = "item"  # item | set | character


@dataclass
class WalkStep:
    section: str
    number: int
    total: int
    text: str
    goals: list = field(default_factory=list)   # Place
    names: list = field(default_factory=list)   # every item / boss name the text mentions
    choices: list = field(default_factory=list)  # [Place] per name found in several spots


def parse(path):
    lines = [l.strip() for l in open(path, encoding="utf-8-sig")]
    numbered = [(i, re.match(r"^(\d+)\.\s+(.+)$", l)) for i, l in enumerate(lines)]
    numbered = [(i, m) for i, m in numbered if m]
    sections, current = [], None
    for k, (_i, m) in enumerate(numbered):
        # A header is the numbered line right before a "1." (its steps restart numbering).
        nxt = numbered[k + 1][1] if k + 1 < len(numbered) else None
        if nxt is not None and nxt.group(1) == "1":
            current = (m.group(2), [])
            sections.append(current)
        elif current is not None:
            current[1].append(m.group(2))
    return sections


def maps_for(section):
    for key, stems in SECTION_MAPS:
        if key.lower() in section.lower():
            return stems
    return []


class Matcher:
    def __init__(self, places, item_names, character_names):
        self.places = places                       # name -> [Place]
        self.used = set()                          # (stem, flag) already assigned
        names = set(item_names) | set(character_names) | set(ALIASES)
        names = [n for n in names if len(n) >= 4 or n in ALIASES]
        # Longest first, so "Black Knight Shield" wins over "Black Knight".
        self.names = sorted(names, key=len, reverse=True)
        self.patterns = {n: re.compile(r"(?<![\w'])" + re.escape(n) + r"(?:'s|s|es)?(?![\w'])", re.I)
                         for n in self.names}
        self.set_pattern = re.compile(r"((?:[A-Z][\w'\-]*\s)+)Set\b")

    def pick(self, name, stems):
        """(places, ambiguous): one place, or every candidate when the name is in several spots
        (Black Knights, Demon Titanite), to be settled by which one is on the way."""
        everywhere = self.places.get(name, [])
        if everywhere and everywhere[0].kind == "character":
            # People stay put between visits, so they can be a destination any number of times.
            # In the maps this section passes through; someone from another area ("Give the Very
            # Large Ember to Andre") is found anywhere.
            local = [p for p in everywhere if p.stem in stems] or everywhere
            return local, len({p.pos for p in local}) > 1
        cands = [p for p in everywhere if (p.stem, p.flag, p.pos) not in self.used]
        local = [p for p in cands if p.stem in stems]
        # Items come from the maps this section passes through ("Lautrec's Sunlight Medal" is
        # his gift, not the Anor Londo pickup); anywhere only when the section has no maps.
        for group in (local, cands if not stems else []):
            if len({(p.stem, p.flag) for p in group}) == 1:
                return group[:1], False
        return local, len(local) > 1

    def set_pieces(self, prefix, stems):
        """Unused armor pickups named '<prefix> <piece>' in this section's maps."""
        out = []
        for name, places in self.places.items():
            if name.startswith(prefix + " "):
                out += [p for p in places if p.kind == "armor" and p.stem in stems
                        and (p.stem, p.flag, p.pos) not in self.used]
        return out

    def match(self, text, stems):
        taken = [False] * len(text)
        for phrase in MASKED:
            for m in re.finditer(re.escape(phrase), text):
                for i in range(m.start(), m.end()):
                    taken[i] = True
        found = []  # (position in text, name, places)
        for m in self.set_pattern.finditer(text):
            words = m.group(1).split()
            # "Collect Havel's Set": drop leading words until real armor pieces turn up.
            for k in range(len(words)):
                prefix = " ".join(words[k:])
                pieces = self.set_pieces(prefix, stems)
                if pieces:
                    found.append((m.start(), prefix + " Set", pieces))
                    for i in range(m.start(), m.end()):
                        taken[i] = True
                    break
        for n in self.names:
            for m in self.patterns[n].finditer(text):
                if any(taken[m.start():m.end()]):
                    continue
                for i in range(m.start(), m.end()):
                    taken[i] = True
                name = ALIASES.get(n, n)
                found.append((m.start(), name, None))
        found.sort(key=lambda f: f[0])

        goals, names, choices = [], [], []
        for pos, name, places in found:
            names.append(name)
            picked, ambiguous = (places, False) if places is not None else self.pick(name, stems)
            # An item you already carry isn't a destination, but someone you talk to, give
            # something to or buy from is.
            verb = governing_verb(text, pos)
            if verb in TOPIC_WORDS or (verb in USE_VERBS and not (picked and picked[0].kind == "character")):
                continue
            if ambiguous:
                choices.append(picked)
                continue
            for p in picked:
                self.used.add((p.stem, p.flag, p.pos))
                goals.append(p)
        return goals, names, choices


def resolve_spot(expr, places):
    """Candidate places for a tools/step_spots.txt entry (see that file for the syntax)."""
    m = re.match(r"(.+?)(?:\s+in\s+(m\d\d_\d\d_\d\d_\d\d))?$", expr.strip())
    what, stem = m.group(1).strip(), m.group(2)

    def named(name, stem):
        coords = re.match(r"^(-?[\d.]+),\s*(-?[\d.]+),\s*(-?[\d.]+)$", name)
        if coords:
            return [Place(name, stem or "", tuple(float(c) for c in coords.groups()), 0, "landmark")]
        return [p for p in places.get(name, []) if stem is None or p.stem == stem]

    b = re.match(r"^bonfire(?:\s+near\s+(.+))?$", what)
    if not b:
        return named(what, stem)
    bonfires = places.get(BONFIRE, [])
    if not b.group(1):
        return [p for p in bonfires if p.stem == stem]
    anchors = named(b.group(1), None)
    if not anchors:
        return []
    a = anchors[0]
    pool = [p for p in bonfires if p.stem == (stem or a.stem)]
    best = min(pool, key=lambda p: _dist(p.pos, a.pos), default=None)
    return [best] if best else []


def build_steps(path, places, item_names, character_names, step_spots=None):
    sections = parse(path)
    # Only names that occur somewhere in the walkthrough are worth a regex.
    everything = " ".join(t for _, items in sections for t in items).lower()
    item_names = [n for n in item_names if n.lower() in everything]
    character_names = [n for n in character_names if n.lower() in everything]
    matcher = Matcher(places, item_names, character_names)
    steps = []
    for section, items in sections:
        stems = maps_for(section)
        for n, text in enumerate(items, 1):
            goals, names, choices = matcher.match(text, stems)
            step = WalkStep(section, n, len(items), text, goals, names, choices)
            sid = "wt-" + re.sub(r"\W+", "-", section.lower()).strip("-") + f"-{n}"
            if sid in (step_spots or {}):
                cands = [Place(p.name, p.stem, p.pos, 0, "landmark") for p in resolve_spot(step_spots[sid], places)]
                if not cands:
                    print(f"step_spots.txt: nothing found for {sid} = {step_spots[sid]}")
                step.goals, step.choices = (cands, []) if len(cands) == 1 else ([], [cands] if cands else [])
            steps.append(step)
    settle_choices(steps, matcher.used)
    follow_people(steps)
    fill_landmarks(steps, places, matcher.used)
    return steps


def _dist(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5


def _neighbours(steps, i):
    """The destination before step i and the next one after it, either may be None."""
    prev = next((steps[j].goals[-1].pos for j in range(i - 1, -1, -1) if steps[j].goals), None)
    nxt = next((steps[j].goals[0].pos for j in range(i + 1, len(steps)) if steps[j].goals), None)
    return prev, nxt


def _detour(prev, nxt, pos):
    if prev is None or nxt is None:
        return _dist(prev or nxt, pos)
    return _dist(prev, pos) + _dist(pos, nxt) - _dist(prev, nxt)


def settle_choices(steps, used):
    """For a name found in several spots, takes the one most on the way from the previous
    destination to the next. Without a pickup flag: it may not be the copy the walkthrough means."""
    picked = defaultdict(set)  # name -> spots already chosen, for "the second Black Knight"
    for i, s in enumerate(steps):
        for cands in s.choices:
            prev, nxt = _neighbours(steps, i)
            cands = [p for p in cands if p.kind == "character" or (p.stem, p.flag, p.pos) not in used]
            first_word = re.escape(cands[0].name.split()[0]) if cands else "$^"
            another = re.search(r"\b(second|third|fourth|next|another|other)\s+(?:\w+\s+){0,2}" + first_word,
                                s.text, re.I)
            if another:
                cands = [p for p in cands if p.pos not in picked[p.name]] or cands
            if not cands:
                continue
            stems = maps_for(s.section)
            if s.goals:
                # Next to the others the step sends you to ("Speak with Griggs, Logan and Siegmeyer").
                best = min(cands, key=lambda p: min(_dist(p.pos, g.pos) for g in s.goals))
            else:
                # A person is usually in the section's own map (Siegmeyer in Izalith, not Ash Lake).
                if cands[0].kind == "character" and stems:
                    cands = [p for p in cands if p.stem == stems[0]] or cands
                best = cands[0] if prev is None and nxt is None else \
                    min(cands, key=lambda p: _detour(prev, nxt, p.pos))
            used.add((best.stem, best.flag, best.pos))
            picked[best.name].add(best.pos)
            s.goals.append(Place(best.name, best.stem, best.pos, 0, best.kind))


def follow_people(steps):
    """"Speak with him again", "Buy Purple Moss ...", "Receive the Pierce Shield": the person
    from the step before."""
    for i, s in enumerate(steps[1:], 1):
        if s.goals or not re.search(r"\b(him|her|his)\b|^(buy|purchase|receive)\b", s.text, re.I):
            continue
        before = [p for p in steps[i - 1].goals if p.kind == "character"]
        if before:
            s.goals.append(before[-1])


def fill_landmarks(steps, places, used):
    """Gives a step that names nothing the landmark it talks about ("Activate the bonfire",
    "Unlock the shortcut door"): the one of that kind that's most on the way from the previous
    destination to the next. Guessed spots carry no pickup flag, so they never tick a step off."""
    for i, s in enumerate(steps):
        if s.goals:
            continue
        prev, nxt = _neighbours(steps, i)
        if prev is None and nxt is None:
            continue
        stems = maps_for(s.section)
        text = s.text.lower()
        for key, words in LANDMARK_WORDS:
            hit = re.search(words, text)
            if not hit:
                continue
            cands = [p for p in places.get(key, []) if p.stem in stems]
            if key == LOOT:
                word = hit.group(0)
                cands = [p for p in cands if word in p.name.lower() and (p.stem, p.flag, p.pos) not in used]

            def detour(p):
                return _detour(prev, nxt, p.pos)

            best = min(cands, key=detour, default=None)
            if best and detour(best) <= MAX_DETOUR:
                if key == LOOT:
                    used.add((best.stem, best.flag, best.pos))
                s.goals.append(Place(best.name, best.stem, best.pos, 0, best.kind))
                break
