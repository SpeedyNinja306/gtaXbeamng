"""Ram test for the bridge's ghost cars (GTA traffic mirrored into BeamNG).

With a BeamNG car spawned (run fakegta.py first), pretends a GTA car is parked 20 m behind it, waits for the bridge
to spawn a ghost for it, checks the ghost doesn't show in the exported picture (needs the ReShade exporter), then
drives that GTA car into the BeamNG car at 15 m/s. Passes when the ghost is hidden and the BeamNG car takes damage
and is pushed forwards.

    python host/ramtest.py
"""
import time

from depthcal import newest_frame
from fakegta import Link, chase_cam, norm

GTA_ID = 4242
HALF = [0.95, 2.4, 0.7]


def main():
    link = Link()
    end = time.perf_counter() + 5
    while not link.veh and time.perf_counter() < end:
        link.send(t="hello", v=1)
        link.pump()
        time.sleep(0.05)
    if not link.veh:
        raise SystemExit("no BeamNG car (run fakegta.py first)")
    v = link.veh
    fwd = norm([v["fwd"][0], v["fwd"][1], 0.0])
    start = [v["pos"][i] - fwd[i] * 20 for i in range(2)] + [v["pos"][2]]
    dmg0, pos0 = v["dmg"], list(v["pos"])

    def car_at(c, speed):
        return {"id": GTA_ID, "c": c, "f": fwd, "u": [0, 0, 1],
                "v": [fwd[0] * speed, fwd[1] * speed, 0.0], "h": HALF}

    def step(c, speed):
        link.pump()
        link.send(t="traffic", cars=[car_at(c, speed)])
        chase_cam(link)
        time.sleep(1 / 60)

    print("parked GTA car 20 m behind: waiting for the ghost (spawns stall BeamNG briefly)")
    end = time.perf_counter() + 8
    while time.perf_counter() < end:
        step(start, 0.0)

    # the ghost must not show in the exported picture: park the GTA car beside the BeamNG car, in the chase cam's view
    def covered():
        w, h, colour, _ = newest_frame()
        return sum(1 for i in range(3, w * h * 4, 4 * 7) if colour[i])
    side = [v["pos"][0] + fwd[1] * 4, v["pos"][1] - fwd[0] * 4, v["pos"][2]]
    for _ in range(90):
        step(side, 0.0)
    with_ghost = covered()
    for _ in range(90):
        step(start, 0.0)
    without = covered()
    hidden = with_ghost < without * 1.3 + 50
    print(f"car pixels with the GTA car beside it {with_ghost}, without {without}: ghost {'hidden' if hidden else 'VISIBLE'}")
    print("ramming at 15 m/s")
    c = list(start)
    t0 = time.perf_counter()
    last = t0
    while time.perf_counter() - t0 < 2.5:
        now = time.perf_counter()
        dt = now - last
        last = now
        # the GTA car stops where it meets the BeamNG car (GTA's proxy would stop it there)
        gap = sum((link.veh["pos"][i] - c[i]) * fwd[i] for i in range(2))
        speed = 15.0 if gap > 2 * HALF[1] - 0.3 else 0.0
        c = [c[0] + fwd[0] * speed * dt, c[1] + fwd[1] * speed * dt, c[2]]
        step(c, speed)
    for _ in range(60):
        step(c, 0.0)
    link.send(t="traffic", cars=[])
    v = link.veh
    moved = sum((v["pos"][i] - pos0[i]) * fwd[i] for i in range(2))
    ok = v["dmg"] > dmg0 + 50 and moved > 0.2 and hidden
    print(f"damage {dmg0:.0f} -> {v['dmg']:.0f}, pushed {moved:.2f} m forwards: {'PASS' if ok else 'FAIL'}")
    raise SystemExit(0 if ok else 1)


if __name__ == "__main__":
    main()
