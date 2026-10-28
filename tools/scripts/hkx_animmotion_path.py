"""Print the animmotion / animrotation / SDS_NPC_* annotations of Havok packfile clips with their times, and the
direction of each animmotion step (0 = forward, + = right).

Usage: hkx_animmotion_path.py file1.hkx [file2.hkx ...]

An hkaAnnotationTrack::Annotation is { float time; <pad>; hkStringPtr text } (16 bytes on 64-bit). The text pointer
is resolved through the section's local fixups (src -> dst pairs), so the time sits 8 bytes before the pointer slot.
"""
import math
import re
import struct
import sys


def sections(b):
    nsec = struct.unpack_from("<i", b, 20)[0]
    off = 64 if struct.unpack_from("<i", b, 12)[0] < 11 else 64 + 16
    out = []
    for i in range(nsec):
        s = b[off + i * 48: off + i * 48 + 48]
        name = s[:19].split(b"\x00")[0].decode(errors="replace")
        out.append((name, *struct.unpack_from("<7I", s, 20)))
    return out


def annotations(b):
    for name, start, lf, gf, vf, exp, imp, end in sections(b):
        if name != "__data__":
            continue
        fix = {}
        for i in range(start + lf, start + gf, 8):
            src, dst = struct.unpack_from("<ii", b, i)
            if src == -1:
                continue
            fix[src] = dst
        for src, dst in fix.items():
            text = b[start + dst: b.index(b"\x00", start + dst)]
            if not re.match(rb"(animmotion|animrotation|SDS_NPC_|RotateEvent|BFCO_NPCAttackStart|HitFrame)", text):
                continue
            time = struct.unpack_from("<f", b, start + src - 8)[0]
            yield time, text.decode(errors="replace")


for path in sys.argv[1:]:
    b = open(path, "rb").read()
    rows = sorted(set(annotations(b)))
    print(f"\n=== {'/'.join(path.replace(chr(92), '/').split('/')[-2:])}")
    px = py = 0.0
    for t, text in rows:
        extra = ""
        if text.startswith("animmotion"):
            x, y, z = (float(v) for v in text.split()[1:4])
            dx, dy = x - px, y - py
            if math.hypot(dx, dy) > 0.5:
                extra = f"   step {math.hypot(dx, dy):6.1f} at {math.degrees(math.atan2(dx, dy)):+5.0f} deg"
            px, py = x, y
            print(f"  {t:6.3f}  motion x={x:7.1f} y={y:7.1f}{extra}")
        else:
            print(f"  {t:6.3f}  {text}")
