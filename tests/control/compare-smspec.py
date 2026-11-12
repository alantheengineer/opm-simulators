#!/usr/bin/env python3
"""Compare two SMSPEC files, ignoring the wall-clock fields of RUNTIMEI.

RUNTIMEI holds the time the run started and the time the file was written,
which differ between any two runs. Everything else must match exactly.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ecl_records import records, values  # noqa: E402

# RUNTIMEI entries 3 to 14: compute start and current time, year to second.
CLOCK_FIELDS = range(3, 15)


def keywords(path):
    for name, kind, count, body in records(path):
        if name == "RUNTIMEI":
            ints = values(kind, count, body)
            for i in CLOCK_FIELDS:
                ints[i] = 0
            body = struct.pack(f">{count}i", *ints)
        yield name, body


def main():
    first = list(keywords(sys.argv[1]))
    second = list(keywords(sys.argv[2]))
    if first == second:
        return 0
    for (name_a, body_a), (name_b, body_b) in zip(first, second):
        if name_a != name_b or body_a != body_b:
            print(f"SMSPEC differs at keyword {name_a}")
            return 1
    print("SMSPEC files have different numbers of keywords")
    return 1


if __name__ == "__main__":
    sys.exit(main())
