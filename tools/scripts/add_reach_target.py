"""Add SDS_NPC_ReachTarget / SDS_NPC_ReachTargetEnd to attack clips, with hkanno64 (dump -> edit -> update).

Usage: add_reach_target.py <hkanno64.exe> <folder with the clip COPIES> <report.md> [--dry]

For every HitFrame the dash before it is found in the clip's animmotion path: from the previous hit (or the clip start),
the window starts where the forward speed first reaches 25% of that stretch's peak, and ends on the hit.
  ref   = forward distance the animation covers in the window (steps within 90 deg of ahead)
  the clip's final animmotion distance (its last key) >= 250: as Elden Ring, min = 0.3 ref, max = 1.2 ref
  the clip's final animmotion distance < 250:                 gentle,         min = 0.5 ref, max = 1.1 ref
  arrive = 200 (centre to centre, the hit should land this far from the target)
Windows under 30 units are left alone. Clips that already carry SDS_NPC_ReachTarget are skipped.
hkanno reads the wrong file when the path has [brackets], so every clip is worked on in a plain temp folder.
"""
import math
import pathlib
import shutil
import subprocess
import sys
import tempfile

ARRIVE = 200
LONG = 250


def hkanno(exe, *args):
    r = subprocess.run([exe, *args], cwd=str(pathlib.Path(exe).parent), capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"hkanno {' '.join(args)} failed: {r.stdout[-300:]} {r.stderr[-300:]}")


def read_dump(path):
    header, rows = [], []
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.rstrip("\n")
        if not line.strip():
            continue
        if line.startswith("#"):
            header.append(line)
            continue
        t, _, text = line.partition(" ")
        rows.append((float(t), text))
    return header, rows


def path_of(rows):
    pts = [(0.0, 0.0, 0.0)]
    for t, text in sorted(rows):
        if text.startswith("animmotion "):
            v = text.split()[1:4]
            pts.append((t, float(v[0]), float(v[1])))
    return pts


def windows(rows):
    pts = path_of(rows)
    final = math.hypot(pts[-1][1], pts[-1][2])
    hits = []
    for t, text in sorted(rows):
        if text == "HitFrame" and (not hits or t - hits[-1] > 0.05):
            hits.append(t)
    out = []
    prev = 0.0
    for hit in hits:
        steps = [(a, b) for a, b in zip(pts, pts[1:]) if prev - 1e-4 <= a[0] and b[0] <= hit + 1e-4 and b[0] > a[0]]
        fwd = [max(0.0, b[2] - a[2]) / (b[0] - a[0]) for a, b in steps]
        if steps and max(fwd) > 0:
            peak = max(fwd)
            first = next(i for i, v in enumerate(fwd) if v >= 0.25 * peak)
            start = steps[first][0][0]
            ref = 0.0
            for a, b in steps[first:]:
                dx, dy = b[1] - a[1], b[2] - a[2]
                if math.hypot(dx, dy) > 1e-6 and abs(math.degrees(math.atan2(dx, dy))) < 90:
                    ref += math.hypot(dx, dy)
            if ref >= 30:
                lo, hi = (0.3, 1.2) if final >= LONG else (0.5, 1.1)
                out.append((round(start, 6), round(hit, 6), round(ref), round(lo * ref), round(hi * ref), round(final)))
        prev = hit
    return out


def main():
    exe, root, report = sys.argv[1], pathlib.Path(sys.argv[2]), sys.argv[3]
    dry = "--dry" in sys.argv
    lines = ["| Клип | Вся атака | Окно (с) | ref | min | max | arrive | Тип |", "|---|---|---|---|---|---|---|---|"]
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="hkanno_"))
    for original in sorted(root.rglob("*.hkx")):
        rel = original.relative_to(root).as_posix()
        hkx = tmp / "clip.hkx"
        shutil.copyfile(original, hkx)
        dump = tmp / "clip.anno.txt"
        hkanno(exe, "dump", "-o", str(dump), str(hkx))
        header, rows = read_dump(dump)
        if any(text.startswith("SDS_NPC_ReachTarget") for _, text in rows):
            lines.append(f"| {rel} | | уже есть ReachTarget, пропущен | | | | | |")
            dump.unlink()
            continue
        wins = windows(rows)
        if not wins:
            lines.append(f"| {rel} | | нет рывка перед ударом | | | | | |")
            dump.unlink()
            continue
        added = []
        for start, end, ref, lo, hi, final in wins:
            added.append((start, f"SDS_NPC_ReachTarget.{ref}|{lo}|{hi}|{ARRIVE}"))
            added.append((end, "SDS_NPC_ReachTargetEnd"))
            kind = "как в ER" if final >= LONG else "мягкий"
            lines.append(f"| {rel} | {final} | {start:.3f}–{end:.3f} | {ref} | {lo} | {hi} | {ARRIVE} | {kind} |")
        if not dry:
            merged = sorted(rows + added, key=lambda r: r[0])
            with open(dump, "w", encoding="utf-8", newline="\n") as f:
                for t, text in merged:
                    f.write(f"{t:.6f} {text}\n")
            hkanno(exe, "update", "-i", str(dump), str(hkx))
            check = hkx.with_suffix(".check.txt")
            hkanno(exe, "dump", "-o", str(check), str(hkx))
            _, back = read_dump(check)
            assert len(back) == len(merged), f"{rel}: {len(back)} annotations after update, expected {len(merged)}"
            check.unlink()
            shutil.copyfile(hkx, original)
        dump.unlink()
    shutil.rmtree(tmp, ignore_errors=True)
    pathlib.Path(report).write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
