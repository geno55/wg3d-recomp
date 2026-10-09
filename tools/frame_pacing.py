"""Frame-pacing analysis of a WG3D_FRAME_LOG (tools/run_scenario.py --frame-log).

The log has, in graphics-thread order, a D line per gfx task (the framebuffer it draws into) and an S
line per VI (the VI origin being scanned out). A "frame" is one framebuffer *generation*: the content
left by the display lists drawn into it since it was last drawn. Per scene (the stretch before each
capture in the scenario file) this reports:

  shown fps        distinct frames presented per second
  hold             how many VIs each presented frame stayed up (30 fps double buffering: all 2s)
  dropped          frames rendered and then overwritten without ever being shown
  flip-backs       showing an older frame again after a newer one was shown (visible jitter)
  drawn on screen  display lists drawing into the framebuffer currently being scanned out (the N64
                   would tear). ultramodern queues each screen update with the registers latched at
                   the *previous* VI (events.cpp: ScreenUpdateAction before update_vi), so the origin
                   being scanned out when a D line is processed is the one in the *next* S line.

Usage: python tools/frame_pacing.py <scenario> [<scenario>...]   (reads build/scen/<name>/frames.log)
"""
import sys
import tomllib
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def fb_of(origin, fbs):
    """The framebuffer whose start is just below the VI origin (origin = fb + line offset)."""
    best = None
    for fb in fbs:
        if 0 <= origin - fb <= 0x2000 and (best is None or fb > best):
            best = fb
    return best


def analyse(events, t0, t1):
    gen = Counter()          # framebuffer -> generation counter
    fbs = set()
    shown = []               # (t, fb, gen) per VI
    shown_gens = set()
    drawn_gens = []          # (fb, gen) as rendered
    drawn_on_screen = 0
    # Origin latched at each event: the next S line's origin (see the module docstring).
    next_origin = [None] * len(events)
    upcoming = None
    for i in range(len(events) - 1, -1, -1):
        if events[i][0] == "S":
            upcoming = events[i][2]
        next_origin[i] = upcoming
    all_fbs = {e[3] for e in events if e[0] == "D" and e[3]}
    for i, (kind, t, a, b) in enumerate(events):
        if kind == "D":
            if b == 0:
                continue
            fbs.add(b)
            gen[b] += 1
            if t0 <= t < t1:
                drawn_gens.append((b, gen[b]))
                latched = next_origin[i]
                if latched is not None and fb_of(latched, all_fbs) == b:
                    drawn_on_screen += 1
        else:
            fb = fb_of(a, fbs)
            if fb is None:
                continue
            if t0 <= t < t1:
                shown.append((t, fb, gen[fb]))
    if not shown:
        return None
    holds, flipbacks = [], 0
    run, prev = 0, None
    seen_order = {}
    for i, (t, fb, g) in enumerate(shown):
        key = (fb, g)
        shown_gens.add(key)
        if key == prev:
            run += 1
            continue
        if prev is not None:
            holds.append(run)
        if key in seen_order:
            flipbacks += 1
        seen_order[key] = i
        prev, run = key, 1
    holds.append(run)
    # The first and last runs are cut by the window boundaries, so they are partial holds.
    holds = holds[1:-1] if len(holds) > 2 else holds
    # A rendered generation is "dropped" if it was overwritten (a later generation of the same
    # framebuffer exists) without ever being presented.
    last_gen = {}
    for fb, g in drawn_gens:
        last_gen[fb] = max(last_gen.get(fb, 0), g)
    dropped = sum(1 for fb, g in drawn_gens if (fb, g) not in shown_gens and g < last_gen[fb])
    dur = shown[-1][0] - shown[0][0] or 1
    return {
        "vis": len(shown), "frames": len(holds), "fps": len(holds) / dur, "hold": Counter(holds),
        "dropped": dropped, "rendered": len(drawn_gens), "flipbacks": flipbacks, "on_screen": drawn_on_screen,
    }


def main() -> int:
    names = sys.argv[1:]
    if not names:
        print(__doc__)
        return 2
    for name in names:
        log = ROOT / "build" / "scen" / name / "frames.log"
        sc = tomllib.loads((ROOT / "tests" / "scenarios" / f"{name}.toml").read_text(encoding="utf-8"))
        events = []
        for line in log.read_text().splitlines():
            p = line.split()
            events.append((p[0], float(p[1]), int(p[2], 16), int(p[3], 16) if len(p) > 3 else 0))
        caps = sorted((c[0], c[1]) for c in sc.get("captures", []))
        bounds = [(0.0, "start")] + caps + [(float(sc["seconds"]) + 5, "end")]
        print(f"== {name}")
        print(f"   {'scene (ends at capture)':28} {'VIs':>5} {'fps':>5}  {'hold (VIs x count)':28} {'drop':>4} {'flip':>4} {'onscr':>5}")
        for (ta, _), (tb, label) in zip(bounds, bounds[1:]):
            r = analyse(events, ta, tb)
            if not r:
                continue
            hold = " ".join(f"{k}x{v}" for k, v in sorted(r["hold"].items()))
            print(f"   {label[:28]:28} {r['vis']:5} {r['fps']:5.1f}  {hold[:28]:28} {r['dropped']:4} {r['flipbacks']:4} {r['on_screen']:5}")
        r = analyse(events, 0, 1e9)
        print(f"   {'TOTAL':28} {r['vis']:5} {r['fps']:5.1f}  {' '.join(f'{k}x{v}' for k, v in sorted(r['hold'].items()))[:28]:28} "
              f"{r['dropped']:4} {r['flipbacks']:4} {r['on_screen']:5}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
