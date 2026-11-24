#!/usr/bin/env python3
"""Build tests/data/synthetic.grib2: two small, valid GRIB2 messages.

Sections 0,1,3 (template 3.0 lat/lon), 4 (4.0), 5 (5.0 simple packing),
6 (no bitmap), 7, 8. 4x3 grid, 12-bit packing, R=2500.0, E=0, D=1, so
value = (2500 + X) / 10.

Synthetic test data, not real model output. Validate the real thing against
eccodes (grib_get_data) once M1 decodes NOAA files.
"""
import struct
import sys
from pathlib import Path


def sm(value, nbytes):
    """GRIB sign-magnitude integer."""
    mag = abs(value)
    top = 1 << (nbytes * 8 - 1)
    assert mag < top
    return (mag | (top if value < 0 else 0)).to_bytes(nbytes, "big")


def pack_bits(values, nbits):
    acc, n, out = 0, 0, bytearray()
    for v in values:
        acc = (acc << nbits) | v
        n += nbits
        while n >= 8:
            n -= 8
            out.append((acc >> n) & 0xFF)
    if n:
        out.append((acc << (8 - n)) & 0xFF)
    return bytes(out)


def section(num, body):
    return struct.pack(">IB", 5 + len(body), num) + body


def message(discipline, category, number, raw):
    ni, nj = 4, 3
    sec1 = section(1, struct.pack(">HHBBBHBBBBBBB", 7, 0, 2, 1, 1, 2026, 11, 23, 12, 0, 0, 0, 1))
    tmpl30 = (bytes([6, 0]) + (0).to_bytes(4, "big") + bytes([0]) + (0).to_bytes(4, "big") + bytes([0])
              + (0).to_bytes(4, "big") + struct.pack(">IIII", ni, nj, 0, 0xFFFFFFFF)
              + sm(40_000_000, 4) + sm(-105_000_000, 4) + bytes([48])
              + sm(38_000_000, 4) + sm(-102_000_000, 4)
              + struct.pack(">II", 1_000_000, 1_000_000) + bytes([0]))
    sec3 = section(3, bytes([0]) + struct.pack(">I", ni * nj) + bytes([0, 0]) + struct.pack(">H", 0) + tmpl30)
    sec4 = section(4, struct.pack(">HH", 0, 0) + bytes([category, number, 2, 0, 96]) + struct.pack(">H", 0)
                   + bytes([0, 1]) + struct.pack(">I", 6) + bytes([103, 0]) + struct.pack(">I", 2)
                   + bytes([255, 0]) + struct.pack(">I", 0))
    nbits = 12
    sec5 = section(5, struct.pack(">IH", ni * nj, 0) + struct.pack(">f", 2500.0) + sm(0, 2) + sm(1, 2)
                   + bytes([nbits, 0]))
    sec6 = section(6, bytes([255]))
    sec7 = section(7, pack_bits(raw, nbits))
    body = sec1 + sec3 + sec4 + sec5 + sec6 + sec7 + b"7777"
    total = 16 + len(body)
    sec0 = b"GRIB" + b"\0\0" + bytes([discipline, 2]) + struct.pack(">Q", total)
    return sec0 + body


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).parent / "data" / "synthetic.grib2")
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = [i * 37 % 400 for i in range(12)]          # 250.0 .. 289.9 K
    ugrd = [(i * 211) % 4095 for i in range(12)]     # exercises the full 12-bit range
    data = message(0, 0, 0, tmp) + message(0, 2, 2, ugrd)
    out.write_bytes(data)
    for name, raw in (("TMP", tmp), ("UGRD", ugrd)):
        vals = [(2500 + x) / 10 for x in raw]
        print(f"{name}: min={min(vals):.1f} max={max(vals):.1f} first={vals[0]:.1f} last={vals[-1]:.1f}")


if __name__ == "__main__":
    main()
