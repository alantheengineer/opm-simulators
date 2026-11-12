"""Reader for unformatted ECLIPSE output files, used by the control tests."""

import struct

_SIZES = {"INTE": 4, "REAL": 4, "LOGI": 4, "DOUB": 8, "CHAR": 8}


def records(path):
    """Yield (keyword, type, count, raw data) for each record in the file."""
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
        item = int(kind[1:]) if kind.startswith("C0") else _SIZES.get(kind, 0)
        remaining = count * item
        body = b""
        while remaining > 0:
            (size,) = struct.unpack(">i", data[pos : pos + 4])
            body += data[pos + 4 : pos + 4 + size]
            pos += size + 8
            remaining -= size
        yield name, kind, count, body


def values(kind, count, body):
    """Decode numeric record data."""
    code = {"INTE": "i", "REAL": "f", "DOUB": "d", "LOGI": "i"}[kind]
    return list(struct.unpack(f">{count}{code}", body))
