"""Read BeamNG's exported picture from the shared memory GTA's compositor reads (layout: shared/gxb_frame.h)."""
import mmap
import struct
from dataclasses import dataclass, field

MAGIC = 0x46425847
SLOTS, MAX_W, MAX_H = 3, 3840, 2160


@dataclass
class Frame:
    w: int
    h: int
    colour: bytes            # whole picture, RGBA8 (zeros outside the car's rectangle)
    depth: bytes             # whole picture, float32
    version: int = 1
    published: int = 0
    frame: int = -1          # BeamNG's frame counter
    tag: int = -1            # tag of the camera it was rendered with
    rect: tuple = (0, 0, 0, 0)
    relative: bool = False
    fov: float = 0.0
    pos: list = field(default_factory=list)
    fwd: list = field(default_factory=list)
    up: list = field(default_factory=list)


def _open():
    return mmap.mmap(-1, 4096 + SLOTS * MAX_W * MAX_H * 8, tagname="Local\\GTAxBeamFrame", access=mmap.ACCESS_READ)


def published():
    """The exporter's published counter (cheap: no pixels)."""
    m = _open()
    try:
        return struct.unpack_from("<q", m, 32)[0]
    finally:
        m.close()


def newest_frame(pixels=True):
    m = _open()
    try:
        magic, version = struct.unpack_from("<II", m, 0)
        if magic != MAGIC:
            raise SystemExit("no BeamNG frame export (is the ReShade exporter installed?)")
        stride, = struct.unpack_from("<q", m, 16)
        pub, = struct.unpack_from("<q", m, 32)
        slot, = struct.unpack_from("<i", m, 40)
        d = 256 + 128 * slot
        w, h = struct.unpack_from("<II", m, d + 24)
        base = 4096 + stride * slot
        f = Frame(w, h, b"", b"", version=version, published=pub, frame=struct.unpack_from("<q", m, d + 8)[0])
        if version == 1:
            f.rect = (0, 0, w, h)
            if pixels:
                f.colour, f.depth = m[base:base + w * h * 4], m[base + w * h * 4:base + w * h * 8]
            return f
        f.tag, = struct.unpack_from("<q", m, d + 16)
        f.rect = struct.unpack_from("<IIII", m, d + 32)
        flags, f.fov = struct.unpack_from("<If", m, d + 48)
        f.relative = bool(flags & 1)
        vals = struct.unpack_from("<9f", m, d + 56)
        f.pos, f.fwd, f.up = list(vals[0:3]), list(vals[3:6]), list(vals[6:9])
        if pixels:
            rx, ry, rw, rh = f.rect
            colour, depth = bytearray(w * h * 4), bytearray(w * h * 4)
            cs, ds = m[base:base + rw * rh * 4], m[base + rw * rh * 4:base + rw * rh * 8]
            for y in range(rh):
                o = ((ry + y) * w + rx) * 4
                colour[o:o + rw * 4] = cs[y * rw * 4:(y + 1) * rw * 4]
                depth[o:o + rw * 4] = ds[y * rw * 4:(y + 1) * rw * 4]
            f.colour, f.depth = bytes(colour), bytes(depth)
        return f
    finally:
        m.close()


def coverage(f, step=2):
    """(pixel count, centroid x, centroid y) of the car in the picture, sampling every step-th pixel."""
    n = sx = sy = 0
    rx, ry, rw, rh = f.rect
    for y in range(ry, ry + rh, step):
        row = y * f.w * 4
        for x in range(rx, rx + rw, step):
            if f.colour[row + x * 4 + 3]:
                n += 1
                sx += x
                sy += y
    return (n, sx / n, sy / n) if n else (0, None, None)
