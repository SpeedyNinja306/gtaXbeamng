"""Fake GTA host: drives the BeamNG half of the passthrough without GTA.

Run with any Python 3.10+ (stdlib only) while BeamNG.drive runs with the gtaxbeam mod:

    python host/fakegta.py [--model etk800] [--out fakegta_out] [--visible]

It loads the void level, streams flat ground tiles and a wall at GTA-like coordinates, spawns a vehicle,
flies a chase camera behind it and runs checks:
  rest      the car settles on the proxy ground instead of falling through
  drive     forwarded throttle accelerates it
  wall      it stops at the proxy wall and takes damage
  steer     steering -1 turns it left (counter-clockwise from above)
  watchdog  when the host goes quiet BeamNG pauses, so the car doesn't move
Writes <out>/report.json and <out>/states.csv. Exit code 0 when every check passes.
"""

import argparse
import csv
import json
import math
import os
import socket
import sys
import time

BEAMNG = ("127.0.0.1", 47801)

ORIGIN = (100.0, 200.0)   # GTA-like world position of the test pad
GROUND_Z = 30.0
TILE_N, TILE_STEP = 33, 2.0
TILE_SIZE = (TILE_N - 1) * TILE_STEP
WALL_Y = ORIGIN[1] + 60.0


class Link:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.setblocking(False)
        self.veh = None
        self.hello = None
        self.crashes = []
        self.gone = False
        self.states = []
        self.probe = None
        self.t0 = time.perf_counter()

    def send(self, **msg):
        self.sock.sendto(json.dumps(msg, separators=(",", ":")).encode(), BEAMNG)

    def pump(self):
        while True:
            try:
                data, _ = self.sock.recvfrom(65535)
            except (BlockingIOError, ConnectionResetError):
                return
            m = json.loads(data)
            t = m.get("t")
            if t == "veh":
                m["_t"] = time.perf_counter() - self.t0
                self.veh = m
                self.states.append(m)
            elif t == "hello":
                self.hello = m
            elif t == "crash":
                self.crashes.append(m)
            elif t == "gone":
                self.gone = True
            elif t == "probe":
                self.probe = m

    def ask_probe(self, inp):
        self.probe = None
        self.send(t="probe")
        wait_for(self, lambda: self.probe is not None, 2.0, lambda: self.send(t="input", **inp))
        return self.probe


def tile_msg(ix, iy):
    ox = ORIGIN[0] + ix * TILE_SIZE - TILE_SIZE / 2
    oy = ORIGIN[1] + iy * TILE_SIZE - TILE_SIZE / 2
    return dict(t="tile", id=f"t{ix}_{iy}", ox=ox, oy=oy, n=TILE_N, step=TILE_STEP,
                h=[GROUND_Z] * (TILE_N * TILE_N))


def norm(v):
    length = math.sqrt(sum(c * c for c in v)) or 1.0
    return [c / length for c in v]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def chase_cam(link, fov=50.0):
    v = link.veh
    if not v:
        return
    fwd = norm([v["fwd"][0], v["fwd"][1], 0.0])
    pos = [v["pos"][0] - fwd[0] * 8, v["pos"][1] - fwd[1] * 8, v["pos"][2] + 3]
    look = norm([v["pos"][i] - pos[i] for i in range(3)])
    right = norm(cross(look, [0, 0, 1]))
    up = cross(right, look)
    link.send(t="cam", pos=pos, fwd=look, up=up, fov=fov, aspect=16 / 9)


def heading(v):
    return math.degrees(math.atan2(-v["fwd"][0], v["fwd"][1]))  # GTA convention: CCW from north


def speed(v):
    return math.sqrt(sum(c * c for c in v["vel"]))


def run_for(link, seconds, inp=None, cam=True, send=True):
    end = time.perf_counter() + seconds
    while time.perf_counter() < end:
        link.pump()
        if send:
            if inp is not None:
                link.send(t="input", **inp)
            if cam:
                chase_cam(link)
            else:
                link.send(t="hello", v=1)
        time.sleep(1 / 60)


def wait_for(link, pred, timeout, keepalive=None):
    end = time.perf_counter() + timeout
    while time.perf_counter() < end:
        link.pump()
        if pred():
            return True
        if keepalive:
            keepalive()
        time.sleep(0.05)
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model", default="etk800")
    ap.add_argument("--out", default="fakegta_out")
    ap.add_argument("--visible", action="store_true", help="draw the proxy collision in green")
    ap.add_argument("--winding", choices=("ccw", "cw", "both"), help="override the collision face winding")
    ap.add_argument("--reload", action="store_true", help="re-read bridge.lua from the mod folder first")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    link = Link()
    checks = {}

    print("waiting for BeamNG bridge on 127.0.0.1:47801 ...")
    if not wait_for(link, lambda: link.hello is not None, 300, lambda: link.send(t="hello", v=1)):
        sys.exit("no answer from BeamNG: is it running with the gtaxbeam mod?")
    if args.reload:
        link.send(t="reload")
        time.sleep(1.0)
        link.hello = None
        if not wait_for(link, lambda: link.hello is not None, 30, lambda: link.send(t="hello", v=1)):
            sys.exit("bridge did not come back after reload")
    print("bridge:", link.hello)

    if not link.hello.get("level"):
        print("loading void level ...")
        link.hello = None
        link.send(t="level")
        ok = wait_for(link, lambda: link.hello is not None and link.hello.get("level"), 180,
                      lambda: link.send(t="hello", v=1))
        if not ok:
            sys.exit("void level did not load")
        time.sleep(2)

    link.send(t="clear")
    if args.winding:
        link.send(t="cfg", winding=args.winding)
    link.send(t="debug", on=bool(args.visible))
    link.send(t="tod", h=13)
    for ix in (-1, 0, 1):
        for iy in (-1, 0, 1, 2):
            link.send(**tile_msg(ix, iy))
            time.sleep(0.01)
    segs = [[ORIGIN[0] - 30 + 6 * k, WALL_Y, ORIGIN[0] - 24 + 6 * k, WALL_Y, GROUND_Z - 1, GROUND_Z + 4]
            for k in range(10)]
    link.send(t="walls", id="wall_north", segs=segs)
    run_for(link, 0.5, cam=False)

    link.gone = False
    link.send(t="spawn", model=args.model, pos=[ORIGIN[0], ORIGIN[1], GROUND_Z], fwd=[0, 1, 0], up=[0, 0, 1])
    if not wait_for(link, lambda: link.veh is not None, 60, lambda: link.send(t="hello", v=1)):
        sys.exit("vehicle never reported state")
    print("vehicle", link.veh["vid"], "at", link.veh["pos"])

    hold = dict(th=0, br=0, st=0, hb=1)
    run_for(link, 4, hold)
    v = link.veh
    ref_z = v["ref"][2]
    rest_ok = GROUND_Z - 0.2 < ref_z < GROUND_Z + 2.5 and abs(v["vel"][2]) < 0.3
    checks["rest"] = dict(ok=rest_ok, ref_z=ref_z, ground_z=GROUND_Z, vz=v["vel"][2],
                          fwd=v["fwd"], heading=heading(v), dmg=v["dmg"], probe=link.ask_probe(hold))
    print("rest:", checks["rest"])

    h0 = heading(link.veh)
    run_for(link, 2.5, dict(th=0.4, br=0, st=-1, hb=0))
    h1 = heading(link.veh)
    dh = (h1 - h0 + 540) % 360 - 180
    checks["steer"] = dict(ok=dh > 5, heading_before=h0, heading_after=h1, delta=dh, note="forward, steer -1")
    print("steer:", checks["steer"])
    run_for(link, 1.5, dict(th=0, br=1, st=0, hb=0))

    link.send(t="place", pos=[ORIGIN[0], ORIGIN[1], GROUND_Z], fwd=[0, 1, 0], up=[0, 0, 1])
    run_for(link, 2.0, hold)
    v = link.veh
    checks["place"] = dict(ok=math.dist(v["pos"][:2], ORIGIN) < 3 and abs(heading(v)) < 10,
                           pos=v["pos"], heading=heading(v))
    print("place:", checks["place"])

    dmg0 = link.veh["dmg"]
    vmax = 0.0
    start_y = link.veh["pos"][1]
    drive_inp = dict(th=0.7, br=0, st=0, hb=0)
    run_for(link, 1.0, drive_inp)
    drive_probe = link.ask_probe(drive_inp)
    end = time.perf_counter() + 15
    while time.perf_counter() < end:
        run_for(link, 0.1, drive_inp)
        vmax = max(vmax, speed(link.veh))
        if link.veh["pos"][1] > WALL_Y + 3:
            break
        if vmax > 5 and speed(link.veh) < 1.0:
            break
    run_for(link, 1.0, dict(th=0, br=1, st=0, hb=0))
    v = link.veh
    checks["drive"] = dict(ok=vmax > 5, vmax=vmax, travelled=v["pos"][1] - start_y, probe=drive_probe)
    wall_ok = v["pos"][1] < WALL_Y and v["dmg"] > dmg0 + 1
    checks["wall"] = dict(ok=wall_ok, y=v["pos"][1], wall_y=WALL_Y, dmg_before=dmg0, dmg_after=v["dmg"],
                          crash_events=len(link.crashes))
    print("drive:", checks["drive"])
    print("wall:", checks["wall"])

    link.send(t="place", pos=[ORIGIN[0], ORIGIN[1], GROUND_Z], fwd=[0, 1, 0], up=[0, 0, 1])
    run_for(link, 1.5, hold)
    run_for(link, 1.0, dict(th=0.8, br=0, st=0, hb=0))
    run_for(link, 0.8, send=False)
    p0 = list(link.veh["pos"])
    run_for(link, 1.0, send=False)
    p1 = list(link.veh["pos"])
    moved = math.dist(p0, p1)
    checks["watchdog"] = dict(ok=moved < 0.5, moved=moved)
    print("watchdog:", checks["watchdog"])
    run_for(link, 1.0, dict(th=0, br=1, st=0, hb=1))

    with open(os.path.join(args.out, "states.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t", "seq", "x", "y", "z", "fx", "fy", "fz", "vx", "vy", "vz", "dmg"])
        for s in link.states:
            w.writerow([round(s["_t"], 4), s["seq"], *s["pos"], *s["fwd"], *s["vel"], s["dmg"]])
    report = dict(model=args.model, checks=checks, crashes=link.crashes[:20], states=len(link.states))
    with open(os.path.join(args.out, "report.json"), "w") as f:
        json.dump(report, f, indent=2)
    passed = all(c["ok"] for c in checks.values())
    print("PASS" if passed else "FAIL", {k: c["ok"] for k, c in checks.items()})
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
