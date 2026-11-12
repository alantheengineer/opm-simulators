#!/usr/bin/env python3
"""Check pressure read through the control interface against a restart file.

Usage: check-pressure.py OUTPUT_DIR CASE PRESSURE_FILE

Compares with the last PRESSURE record written, from the unified restart file
if there is one and otherwise from the highest numbered X file.
"""

import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ecl_records import records, values  # noqa: E402

# Restart arrays are single precision.
TOLERANCE = 1e-6


def last_pressure(out, case):
    unified = os.path.join(out, case + ".UNRST")
    files = [unified] if os.path.exists(unified) else sorted(glob.glob(os.path.join(out, case + ".X[0-9]*")))[-1:]
    if not files:
        sys.exit("No restart file found")
    pressure = None
    for name, kind, count, body in records(files[0]):
        if name == "PRESSURE":
            pressure = values(kind, count, body)
    if pressure is None:
        sys.exit(f"No PRESSURE in {files[0]}")
    return pressure


def main():
    out, case, path = sys.argv[1:4]
    expected = last_pressure(out, case)
    with open(path) as f:
        actual = [float(line) for line in f]
    if len(actual) != len(expected):
        print(f"Got {len(actual)} cells, the restart file has {len(expected)}")
        return 1
    worst = max(abs(a - e) / max(abs(e), 1.0) for a, e in zip(actual, expected))
    if worst > TOLERANCE:
        print(f"Pressure differs from the restart file, largest relative difference {worst:.3g}")
        return 1
    print(f"Pressure of {len(actual)} cells matches the restart file")
    return 0


if __name__ == "__main__":
    sys.exit(main())
