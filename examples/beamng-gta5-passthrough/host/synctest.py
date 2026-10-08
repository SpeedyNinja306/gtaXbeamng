"""Motion sync test for the compositor's BeamNG half (no GTA needed).

Needs BeamNG running with the gtaxbeam mod and the ReShade exporter, and a car on the fakegta.py test pad (run
fakegta.py first). Three measurements:

  wander    drives the car at ~12 m/s with a chase camera, first in world space (the camera sent from the newest state,
            as GTA used to), then relative to the car; reports how far the car's centre wanders in the picture (px).
            Car-relative should hold it still: the car's motion drops out of the picture.
  latch     swings a car-relative camera from side to side and checks that the camera each picture is tagged with is
            the one it was rendered with: the car's position in the picture is compared with the tagged camera and
            with the cameras sent a few messages before and after it (the best match should be offset 0).
  latency   how long from sending a camera until the picture rendered with it is published (ms).

    python host/synctest.py
"""
import math
import statistics
import time

import frame
from fakegta import ORIGIN, GROUND_Z, Link, cross, norm, wait_for

SEND_HZ = 120
BEHIND, ABOVE = 8.0, 2.5


def rel_camera(yaw_deg=0.0):
    """Camera behind and above the car, looking at its centre, turned by yaw (degrees, positive = left)."""
    p = [0.0, -BEHIND, ABOVE]
    f = norm([-p[0], -p[1], -p[2]])
    a = math.radians(yaw_deg)
    f = [f[0] * math.cos(a) - f[1] * math.sin(a), f[0] * math.sin(a) + f[1] * math.cos(a), f[2]]
    r = norm(cross(f, [0, 0, 1]))
    return p, f, cross(r, f)


def car_axes(v):
    f = norm(v["fwd"])
    u = v["up"]
    d = sum(u[i] * f[i] for i in range(3))
    u = norm([u[i] - f[i] * d for i in range(3)])
    return cross(f, u), f, u


def to_world(v, p, f, u):
    r, fw, up = car_axes(v)
    c = v["pos"]
    pw = [c[i] + r[i] * p[0] + fw[i] * p[1] + up[i] * p[2] for i in range(3)]
    fwv = [r[i] * f[0] + fw[i] * f[1] + up[i] * f[2] for i in range(3)]
    upv = [r[i] * u[0] + fw[i] * u[1] + up[i] * u[2] for i in range(3)]
    return pw, fwv, upv


class Sender:
    def __init__(self, link):
        self.link = link
        self.tag = 0
        self.sent = {}  # tag -> (time, rel pose or None)
        self.next = time.perf_counter()

    def due(self):
        return time.perf_counter() >= self.next

    def send(self, rel, inp=None, fov=50.0):
        self.next += 1.0 / SEND_HZ
        if time.perf_counter() > self.next + 0.1:
            self.next = time.perf_counter()
        self.tag += 1
        v = self.link.veh
        p, f, u = rel
        pw, fw, uw = to_world(v, p, f, u)
        msg = dict(t="cam", f=self.tag, pos=pw, fwd=fw, up=uw, fov=fov, aspect=16 / 9)
        if rel is not None and self.relative:
            msg["rel"] = dict(p=p, f=f, u=u)
        self.link.send(**msg)
        if inp is not None:
            self.link.send(t="input", **inp)
        self.sent[self.tag] = (time.perf_counter(), rel)

    relative = True


def project_centre(fr, p, f, u):
    """Pixel x, y of the car's box centre for a car-relative camera p, f, u."""
    fw = norm(f)
    d = sum(u[i] * fw[i] for i in range(3))
    up = norm([u[i] - fw[i] * d for i in range(3)])
    right = cross(fw, up)
    q = [-p[0], -p[1], -p[2]]
    x, y, z = sum(right[i] * q[i] for i in range(3)), sum(up[i] * q[i] for i in range(3)), sum(fw[i] * q[i] for i in range(3))
    t = math.tan(math.radians((fr.fov or 50.0) / 2))
    return fr.w / 2 + x / z / (t * fr.w / fr.h) * fr.w / 2, fr.h / 2 - y / z / t * fr.h / 2


def wander(link, sender, relative, seconds):
    sender.relative = relative
    link.send(t="place", pos=[ORIGIN[0], ORIGIN[1] - 80, GROUND_Z], fwd=[0, 1, 0], up=[0, 0, 1])
    rel = rel_camera()
    hold = dict(th=0, br=0, st=0, hb=1)
    end = time.perf_counter() + 2.0
    while time.perf_counter() < end:
        link.pump()
        if sender.due():
            sender.send(rel, hold)
        time.sleep(0.001)
    xs, ys, speeds = [], [], []
    last_pub = frame.published()
    end = time.perf_counter() + seconds
    while time.perf_counter() < end:
        link.pump()
        v = link.veh
        sp = math.sqrt(sum(c * c for c in v["vel"]))
        inp = dict(th=0.6 if sp < 12 else 0.0, br=0, st=0, hb=0)
        if sender.due():
            sender.send(rel, inp)
        pub = frame.published()
        if pub != last_pub and sp > 8:
            last_pub = pub
            fr = frame.newest_frame()
            n, cx, cy = frame.coverage(fr, 3)
            if n:
                xs.append(cx)
                ys.append(cy)
                speeds.append(sp)
        time.sleep(0.001)
    link.send(t="input", th=0, br=0, st=0, hb=1)
    if len(xs) < 5:
        return None
    return dict(samples=len(xs), speed=statistics.mean(speeds), sx=statistics.pstdev(xs), sy=statistics.pstdev(ys),
                range_y=max(ys) - min(ys))


def latch(link, sender, seconds):
    sender.relative = True
    link.send(t="place", pos=[ORIGIN[0], ORIGIN[1], GROUND_Z], fwd=[0, 1, 0], up=[0, 0, 1])
    hold = dict(th=0, br=0, st=0, hb=1)
    t0 = time.perf_counter()
    last_pub = frame.published()
    rows, lat = [], []
    while time.perf_counter() - t0 < seconds:
        link.pump()
        if sender.due():
            t = time.perf_counter() - t0
            sender.send(rel_camera(12.0 * math.sin(2 * math.pi * 1.5 * t)), hold)
        pub = frame.published()
        if pub != last_pub:
            last_pub = pub
            now = time.perf_counter()
            fr = frame.newest_frame(pixels=time.perf_counter() - t0 > 1.0)
            if fr.version < 2:
                raise SystemExit("the exporter in BeamNG is the old version (restart BeamNG after beamng/install_reshade.ps1)")
            if fr.tag in sender.sent:
                lat.append((now - sender.sent[fr.tag][0]) * 1000)
            if fr.colour and fr.tag in sender.sent and fr.relative:
                n, cx, _ = frame.coverage(fr, 3)
                if n:
                    rows.append((fr, cx))
        time.sleep(0.0005)
    if not rows:
        return None, lat
    result = {}
    for k in range(-3, 4):
        errs = []
        for fr, cx in rows:
            s = sender.sent.get(fr.tag + k)
            if s and s[1]:
                errs.append(cx - project_centre(fr, *s[1])[0])
        if len(errs) > 5:
            m = statistics.mean(errs)
            result[k] = math.sqrt(statistics.mean((e - m) ** 2 for e in errs))
    # the latched pose itself, as published
    errs = [cx - project_centre(fr, fr.pos, fr.fwd, fr.up)[0] for fr, cx in rows]
    m = statistics.mean(errs)
    result["latched"] = math.sqrt(statistics.mean((e - m) ** 2 for e in errs))
    return result, lat


def main():
    link = Link()
    if not wait_for(link, lambda: link.veh is not None, 5, lambda: link.send(t="hello", v=1)):
        raise SystemExit("no BeamNG car (run fakegta.py first)")
    sender = Sender(link)
    fr = frame.newest_frame(pixels=False)
    print(f"exporter layout v{fr.version}, picture {fr.w}x{fr.h}")
    # a fresh car (fakegta.py leaves it wrapped around its test wall)
    vid = link.veh["vid"]
    link.send(t="spawn", model="etk800", pos=[ORIGIN[0], ORIGIN[1] - 80, GROUND_Z], fwd=[0, 1, 0], up=[0, 0, 1])
    link.veh["dmg"] = 1e9  # until a state from the new car arrives
    end = time.perf_counter() + 20.0
    settled = None
    while time.perf_counter() < end:
        link.pump()
        if link.veh and sender.due():
            sender.send(rel_camera(), dict(th=0, br=0, st=0, hb=1))
        if settled is None and link.veh["dmg"] < 100:
            settled = time.perf_counter() + 2.0
        if settled and time.perf_counter() > settled:
            break
        time.sleep(0.002)
    v = link.veh
    if v["dmg"] > 100 or abs(v["pos"][2] - GROUND_Z) > 2:
        raise SystemExit(f"no fresh car on the test pad (damage {v['dmg']:.0f}, z {v['pos'][2]:.1f}): run fakegta.py first")

    for relative in (False, True):
        r = wander(link, sender, relative, 3.5)
        name = "car-relative" if relative else "world camera"
        if r:
            print(f"wander, {name}: {r['samples']} pictures at {r['speed']:.1f} m/s: car centre spread x {r['sx']:.1f} px, "
                  f"y {r['sy']:.1f} px (range {r['range_y']:.1f} px)")
        else:
            print(f"wander, {name}: too few pictures")

    if fr.version >= 2:
        res, lat = latch(link, sender, 6.0)
        if res:
            print("latch: rms error of the car's x in the picture against the camera sent k messages after the tag "
                  f"({SEND_HZ} Hz):")
            for k, e in res.items():
                print(f"  {k!s:>8}: {e:6.2f} px")
        if lat:
            lat.sort()
            print(f"latency camera sent -> picture published: median {lat[len(lat) // 2]:.1f} ms, "
                  f"90% {lat[int(len(lat) * 0.9)]:.1f} ms, max {lat[-1]:.1f} ms ({len(lat)} pictures)")
    link.send(t="input", th=0, br=0, st=0, hb=1)


if __name__ == "__main__":
    main()
