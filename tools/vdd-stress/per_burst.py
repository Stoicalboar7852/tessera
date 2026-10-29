"""Worst stall per burst: per_burst.py <run dir> [steps...]
A burst is one batch, or the run of one-at-a-time changes of one kind in one cycle (add 8, then remove 8). It owns the
time until the next burst starts, so the hold after it (its aftermath) counts too. For each burst: the longest compose
and ui gap overlapping it, and the share of its window the compositor was stalled. Then median / p90 / max per step,
op and count, and the share of bursts whose longest stall exceeds 250 ms."""
import csv, sys
from pathlib import Path

root = Path(sys.argv[1])
steps = sys.argv[2:] or [p.name for p in sorted(root.iterdir()) if (p / "events.csv").exists()]


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(p / 100 * (len(v) - 1) + 0.5))] if v else 0


for step in steps:
    d = root / step
    if not (d / "events.csv").exists():
        continue
    ev = sorted(csv.DictReader(open(d / "events.csv", encoding="utf-8")), key=lambda e: float(e["t_ms"]))
    if not ev:
        continue
    gaps = list(csv.DictReader(open(d / "gaps.csv", encoding="utf-8")))
    bursts = []
    for e in ev:
        key = (e["op"], e["count"], e.get("cycle", ""))
        if bursts and bursts[-1]["key"] == key:
            bursts[-1]["n"] += 1
            bursts[-1]["last"] = e
        else:
            bursts.append({"key": key, "t0": float(e["t_ms"]), "n": 1, "last": e})
    for i, b in enumerate(bursts):
        last = b["last"]
        b["t1"] = bursts[i + 1]["t0"] if i + 1 < len(bursts) else float(last["t_ms"]) + max(float(last["visible_ms"]), 0) + 3000
    g = {p: [(float(r["t_ms"]), float(r["duration_ms"])) for r in gaps if r["probe"] == p] for p in ("compose", "ui")}
    print(f"=== {step}: {len(ev)} ops in {len(bursts)} bursts")
    for op, count in sorted({(b["key"][0], b["key"][1]) for b in bursts}, key=lambda k: (k[0], int(k[1]))):
        bs = [b for b in bursts if b["key"][0] == op and b["key"][1] == count]
        line = f"  {op:12} x{count:>2} ({len(bs):3} bursts, {bs[0]['n']} per burst)"
        for probe in ("compose", "ui"):
            worst = [max((dur for (t, dur) in g[probe] if t < b["t1"] and t + dur > b["t0"]), default=0) for b in bs]
            over = sum(1 for w in worst if w > 250) / len(worst) * 100
            line += f" | {probe} med {pct(worst, 50):6.0f} p90 {pct(worst, 90):6.0f} max {max(worst):6.0f} >250 {over:3.0f}%"
        print(line)
    # Share of the step the compositor was stalled (gaps above the probe threshold).
    span = bursts[-1]["t1"] - bursts[0]["t0"]
    stalled = sum(dur for (_, dur) in g["compose"])
    print(f"  compose stalled {stalled / 1000:.0f} s of {span / 1000:.0f} s ({stalled / span * 100:.0f} %), "
          f"{stalled / len(ev):.0f} ms per change")
