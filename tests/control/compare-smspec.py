#!/usr/bin/env python3
"""Compare two SMSPEC files, ignoring the wall-clock fields of RUNTIMEI.

RUNTIMEI holds the time the run started and the time the file was written,
which differ between any two runs. Everything else must match exactly.
"""

import struct
import sys

# RUNTIMEI entries 3 to 14: compute start and current time, year to second.
CLOCK_FIELDS = range(3, 15)


def records(path):
    """Yield (keyword, raw data) pairs from an unformatted ECLIPSE file."""
    with open(path, "rb") as f:
        data = f.read()
    pos = 0
    while pos < len(data):
        (size,) = struct.unpack(">i", data[pos : pos + 4])
        header = data[pos + 4 : pos + 4 + size]
        pos += size + 8
        name = header[:8].decode().strip()
        (count,) = struct.unpack(">i", header[8:12])
        kind = header[12:16].decode()
        item = 8 if kind in ("CHAR", "DOUB") else 4
        if kind.startswith("C0"):
            item = int(kind[1:])
        remaining = count * item
        body = b""
        while remaining > 0:
            (size,) = struct.unpack(">i", data[pos : pos + 4])
            body += data[pos + 4 : pos + 4 + size]
            pos += size + 8
            remaining -= size
        if name == "RUNTIMEI":
            values = list(struct.unpack(f">{count}i", body))
            for i in CLOCK_FIELDS:
                values[i] = 0
            body = struct.pack(f">{count}i", *values)
        yield name, body


def main():
    first = list(records(sys.argv[1]))
    second = list(records(sys.argv[2]))
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
