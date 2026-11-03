"""Readers for the Dark Souls Remastered file formats the guide generator needs.

Only reads; nothing here writes game files. Formats (all little-endian, 32-bit offsets in DSR):
  DCX  zlib-compressed wrapper
  BND3 archive of named files
  PARAM table + PARAMDEF row layout (the game ships its own paramdefs)
  FMG  text table (UTF-16)
  MSB  map layout: parts (objects, enemies, ...) and events (treasure, ...)
"""

import re
import struct
import zlib
from dataclasses import dataclass, field


def read_dcx(path):
    data = open(path, "rb").read()
    if data[:4] != b"DCX\0":
        return data
    return zlib.decompress(data[data.index(b"DCA\0") + 8:])


def read_bnd3(data):
    """Returns {file name: bytes}. Names are reduced to the part after the last backslash."""
    assert data[:4] == b"BND3", "not a BND3 archive"
    count = struct.unpack_from("<I", data, 0x10)[0]
    files = {}
    for i in range(count):
        _flags, size, offset, _id, name_off, _usize = struct.unpack_from("<6I", data, 0x20 + i * 0x18)
        name = data[name_off:data.index(b"\0", name_off)].decode("cp932")
        files[name.split("\\")[-1]] = data[offset:offset + size]
    return files


def _cstr(data, offset, size=None, encoding="cp932"):
    end = data.index(b"\0", offset) if size is None else offset + size
    return data[offset:end].split(b"\0")[0].decode(encoding, "replace").strip()


# ---------------------------------------------------------------------------------- params

_SCALAR = {"s8": "b", "u8": "B", "s16": "h", "u16": "H", "s32": "i", "u32": "I", "f32": "f"}


def read_paramdef(data):
    """Returns [(name, offset, struct fmt or None, bit, bits)] and the row size."""
    field_count, field_size = struct.unpack_from("<HH", data, 0x08)
    header_size = struct.unpack_from("<H", data, 0x04)[0]
    fields, offset = [], 0
    bit_container = None  # (offset, type, size, next bit)
    for i in range(field_count):
        base = header_size + i * field_size
        byte_count = struct.unpack_from("<i", data, base + 0x64)[0]
        # The internal type may be an enum name (e.g. ITEMLOT_CATEGORY); the display type is the
        # primitive. Padding is "dummy8" in both.
        typ = _cstr(data, base + 0x40, 8)
        if _cstr(data, base + 0x6C, 0x20) == "dummy8":
            typ = "dummy8"
        name = _cstr(data, base + 0x8C, 0x20)
        m = re.match(r"(\w+)\s*:\s*(\d+)", name)
        if m and typ in _SCALAR:
            name, bits = m.group(1), int(m.group(2))
            if (bit_container is None or bit_container[1] != typ
                    or bit_container[3] + bits > byte_count * 8):
                bit_container = [offset, typ, byte_count, 0]
                offset += byte_count
            fields.append((name, bit_container[0], "<" + _SCALAR[typ], bit_container[3], bits))
            bit_container[3] += bits
            continue
        bit_container = None
        name = re.sub(r"\[.*", "", name)
        fields.append((name, offset, "<" + _SCALAR[typ] if typ in _SCALAR else None, 0, 0))
        offset += byte_count
    return fields, offset


def read_param(data, paramdef):
    """Returns {row id: {field: value}} using the layout from read_paramdef."""
    fields, row_size = paramdef
    row_count = struct.unpack_from("<H", data, 0x0A)[0]
    rows = {}
    for i in range(row_count):
        row_id, data_off, _name_off = struct.unpack_from("<iII", data, 0x30 + i * 12)
        row = {}
        for name, off, fmt, bit, bits in fields:
            if fmt is None:
                continue
            value = struct.unpack_from(fmt, data, data_off + off)[0]
            if bits:
                value = (value >> bit) & ((1 << bits) - 1)
            row[name] = value
        rows[row_id] = row
    return rows


def load_params(game_dir, names):
    params = read_bnd3(read_dcx(f"{game_dir}/param/GameParam/GameParam.parambnd.dcx"))
    defs = read_bnd3(read_dcx(f"{game_dir}/paramdef/paramdef.paramdefbnd.dcx"))
    out = {}
    for name in names:
        raw = params[name + ".param"]
        type_name = _cstr(raw, 0x0C, 0x20)
        # Match the param to its paramdef by the row type name embedded in both.
        pdef = next(read_paramdef(d) for d in defs.values() if _cstr(d, 0x0C, 0x20) == type_name)
        out[name] = read_param(raw, pdef)
    return out


# ---------------------------------------------------------------------------------- text

def read_fmg(data):
    """Returns {id: text}."""
    group_count, _string_count, offsets_off = struct.unpack_from("<iiI", data, 0x0C)
    texts = {}
    for g in range(group_count):
        index, first, last = struct.unpack_from("<iii", data, 0x1C + g * 12)
        for k, text_id in enumerate(range(first, last + 1)):
            off = struct.unpack_from("<I", data, offsets_off + (index + k) * 4)[0]
            if off:
                end = off
                while data[end:end + 2] != b"\0\0":
                    end += 2
                texts[text_id] = data[off:end].decode("utf-16-le")
    return texts


def load_texts(game_dir, language="ENGLISH"):
    files = read_bnd3(read_dcx(f"{game_dir}/msg/{language}/item.msgbnd.dcx"))
    return {name[:-4]: read_fmg(data) for name, data in files.items() if name.endswith(".fmg")}


# ---------------------------------------------------------------------------------- maps

PART_ENEMY = 2
PART_DUMMY_ENEMY = 10  # an enemy or NPC that a script places, e.g. on a later visit
EVENT_TREASURE = 4
EVENT_MAP_OFFSET = 9


@dataclass
class Part:
    name: str
    type: int
    pos: tuple
    npc_param: int = -1  # enemies only


@dataclass
class Treasure:
    name: str
    part: int            # index into Msb.parts
    item_lots: list = field(default_factory=list)


@dataclass
class Msb:
    parts: list
    treasures: list
    offset: tuple = (0.0, 0.0, 0.0)  # add to local positions to get world positions


def _sections(data):
    off, out = 0, {}
    while True:
        _unk, name_off, count = struct.unpack_from("<III", data, off)
        offsets = struct.unpack_from(f"<{count}I", data, off + 12)
        out[_cstr(data, name_off)] = offsets[:-1]
        off = offsets[-1]
        if not off:
            return out


def read_msb(path):
    data = open(path, "rb").read()
    sections = _sections(data)

    parts = []
    for start in sections["PARTS_PARAM_ST"]:
        name_off, ptype = struct.unpack_from("<iI", data, start)
        pos = struct.unpack_from("<3f", data, start + 0x14)
        part = Part(_cstr(data, start + name_off), ptype, pos)
        if ptype in (PART_ENEMY, PART_DUMMY_ENEMY):
            type_off = struct.unpack_from("<i", data, start + 0x5C)[0]
            part.npc_param = struct.unpack_from("<i", data, start + type_off + 0x0C)[0]
        parts.append(part)

    treasures = []
    offset = (0.0, 0.0, 0.0)
    for start in sections["EVENT_PARAM_ST"]:
        name_off, _eid, etype, _idx, _base_off, type_off = struct.unpack_from("<6i", data, start)
        t = start + type_off
        if etype == EVENT_MAP_OFFSET:
            offset = struct.unpack_from("<3f", data, t)
        if etype != EVENT_TREASURE:
            continue
        # +0 unused, +4 part index, then 5 item lots each followed by -1
        part_index = struct.unpack_from("<i", data, t + 4)[0]
        lots = [struct.unpack_from("<i", data, t + 8 + i * 8)[0] for i in range(5)]
        treasures.append(Treasure(_cstr(data, start + name_off), part_index,
                                  [lot for lot in lots if lot > 0]))
    return Msb(parts, treasures, offset)
