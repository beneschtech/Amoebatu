#!/usr/bin/env python3
"""Generate a large METAR bulletin for benchmarking parallel decoding.

    python3 tests/make_metar_load.py 20000 > /tmp/metar_load.txt
    dolphin --stats --threads 1 examples/metar.dol /tmp/metar_load.txt > /dev/null
    dolphin --stats examples/metar.dol /tmp/metar_load.txt > /dev/null

Reports are the real ones from data/metar_bulletin.txt plus the synthetic
edge cases, cycled with synthetic station IDs. That keeps the uneven mix of
short and long reports, which is the case batching has to handle.
"""
import itertools
import sys
from pathlib import Path


def reports(path):
    text = path.read_bytes().decode("ascii", "replace")
    body = " ".join(l.strip(" \r\x01\x03") for l in text.split("\n")[1:]
                    if l.strip(" \r\x01\x03") not in ("", "METAR", "SPECI") and not l.strip().isdigit())
    out = []
    for r in body.split("="):
        g = r.split()
        if g and g[0] in ("METAR", "SPECI"):
            g = g[1:]
        if len(g) >= 2 and len(g[1]) == 7 and g[1].endswith("Z"):   # skips WMO header lines
            out.append(" ".join(g))
    return out


def main():
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 20000
    data = Path(__file__).parent / "data"
    pool = reports(data / "metar_bulletin.txt") + reports(data / "metar_edge.txt")
    out = ["SAUS99 KXXX 241200", "METAR"]
    for i, rep in zip(range(n), itertools.cycle(pool)):
        groups = rep.split()
        if groups[0] in ("METAR", "SPECI"):
            groups = groups[1:]
        groups[0] = "K%03d" % (i % 1000) if i % 2 else "X%03d" % (i % 1000)
        out.append(" ".join(groups) + "=")
    print("\n".join(out))


if __name__ == "__main__":
    main()
