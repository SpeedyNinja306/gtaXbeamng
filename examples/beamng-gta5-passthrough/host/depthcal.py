"""Calibrate BeamNG's depth buffer convention and fov for GTAxBeam.fx.

Needs BeamNG running with the gtaxbeam mod, a car spawned (run fakegta.py first) and the ReShade exporter installed
(beamng/install_reshade.ps1). Puts the camera level behind the car at several distances, reads the raw depth at
the picture's centre from the shared memory GTA's compositor reads, and fits reversed-Z near/far planes. Then
compares the car's width in pixels with what a vertical or a horizontal 50 degree fov would give:

    python host/depthcal.py
"""
import math
import mmap
import struct
import time

from fakegta import Link, norm

MAGIC = 0x46425847


def newest_frame():
    """(w, h, colour bytes, depth bytes) of the newest published BeamNG frame."""
    m = mmap.mmap(-1, 4096 + 3 * 3840 * 2160 * 8, tagname="Local\\GTAxBeamFrame", access=mmap.ACCESS_READ)
    try:
        magic, = struct.unpack_from("<I", m, 0)
        if magic != MAGIC:
            raise SystemExit("no BeamNG frame export (is the ReShade exporter installed?)")
        stride, = struct.unpack_from("<q", m, 16)
        slot, = struct.unpack_from("<i", m, 40)
        w, h = struct.unpack_from("<II", m, 256 + 128 * slot + 24)
        base = 4096 + stride * slot
        return w, h, m[base:base + w * h * 4], m[base + w * h * 4:base + w * h * 8]
    finally:
        m.close()


def centre_depth():
    w, h, _, depth = newest_frame()
    d, = struct.unpack_from("<f", depth, ((h // 2) * w + w // 2) * 4)
    return d


def hold(link, cam, seconds):
    end = time.perf_counter() + seconds
    while time.perf_counter() < end:
        link.pump()
        link.send(t="cam", **cam)
        time.sleep(1 / 60)


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
    centre = v["pos"]
    rows = []
    for dist in (8.0, 20.0, 60.0, 150.0):
        pos = [centre[0] - fwd[0] * dist, centre[1] - fwd[1] * dist, centre[2]]
        cam = dict(pos=pos, fwd=fwd, up=[0, 0, 1], fov=50.0, aspect=16 / 9)
        hold(link, cam, 1.5)
        d = centre_depth()
        rows.append((dist, d))
        print(f"camera {dist:6.1f} m from the car's centre: raw depth {d:.7g}")
    # reversed Z: d = A / z - A / f, z = dist - c (c: centre to the rear surface). Fit c, A, f by least squares over c.
    best = None
    for i in range(0, 400):
        c = i * 0.01
        xs = [1.0 / (dist - c) for dist, _ in rows]
        ys = [d for _, d in rows]
        n = len(xs)
        mx, my = sum(xs) / n, sum(ys) / n
        sxx = sum((x - mx) ** 2 for x in xs)
        a = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
        b = my - a * mx
        err = sum((a * x + b - y) ** 2 for x, y in zip(xs, ys))
        if best is None or err < best[0]:
            best = (err, c, a, b)
    err, c, a, b = best
    far = -a / b if b < 0 else float("inf")
    near = a * far / (far + a) if far != float("inf") else a
    print(f"rear surface {c:.2f} m behind the centre; A = {a:.6g}, B = {b:.6g}")
    print(f"reversed Z: near {near:.4f} m, far {far:.1f} m (residual {err:.3g})")

    dist = 8.0
    pos = [centre[0] - fwd[0] * dist, centre[1] - fwd[1] * dist, centre[2]]
    hold(link, dict(pos=pos, fwd=fwd, up=[0, 0, 1], fov=50.0, aspect=16 / 9), 1.5)
    w, h, colour, depth = newest_frame()
    # back-project every car pixel with its depth: with the right focal length the widest point is the car's half width
    pixels = []
    for y in range(0, h):
        for x in range(w):
            if colour[(y * w + x) * 4 + 3]:
                d, = struct.unpack_from("<f", depth, (y * w + x) * 4)
                if d > 0:
                    pixels.append((x + 0.5 - w / 2, near * far / (near + d * (far - near))))
    if not pixels:
        raise SystemExit("the car isn't in the picture")
    print(f"frame {w}x{h}, the car's half width is {v['half'][0]:.3f} m; widest point back-projected with a")
    for name, focal in (("vertical", (h / 2) / math.tan(math.radians(25))), ("horizontal", (w / 2) / math.tan(math.radians(25)))):
        print(f"  {name} 50 degree fov: {max(abs(px) / focal * z for px, z in pixels):.3f} m")


if __name__ == "__main__":
    main()
