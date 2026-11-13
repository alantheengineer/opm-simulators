#!/usr/bin/env python3
"""Write an SPE1 variant for the control tests.

Usage: make-deck.py VARIANT SPE1_DECK TARGET

idle adds a well, IDLE, that is never given a control, so it reports no
control mode.

The other variants turn INJ into a water injector. brine adds BRINE, water
properties that depend on salt (PVTWSALT) and an initial salt
concentration. tracer adds one water tracer, SEA, starting at zero.
thermal adds THERMAL, rock and fluid heat properties, a uniform initial
temperature and an injection temperature equal to it.
"""

import re
import sys


def replace_once(text, old, new):
    if text.count(old) != 1:
        sys.exit(f"expected one {old!r} in the SPE1 deck")
    return text.replace(old, new)


def water_injector(text):
    text, n = re.subn(r"('INJ'\s+'G1'\s+1\s+1\s+8335\s+)'GAS'", r"\1'WATER'", text)
    if n != 1:
        sys.exit("expected one WELSPECS record for INJ")
    text, n = re.subn(r"'INJ'\s+'GAS'\s+'OPEN'\s+'RATE'\s+100000", "'INJ' 'WATER' 'OPEN' 'RATE' 5000", text)
    if n != 1:
        sys.exit("expected one WCONINJE record for INJ")
    return text


def brine(text):
    text = replace_once(text, "\nDISGAS\n", "\nDISGAS\nBRINE\n")
    text, n = re.subn(
        r"\nPVTW\n.*?4017\.55 1\.038 3\.22E-6 0\.318 0\.0 /\n",
        "\nPVTWSALT\n 4017.55 0.0 /\n 0.0 1.038 3.22E-6 0.318 0.0\n 20.0 1.030 3.0E-6 0.450 0.0 /\n",
        text,
        flags=re.S,
    )
    if n != 1:
        sys.exit("expected one PVTW keyword")
    return replace_once(text, "\n8450 1.270 /\n", "\n8450 1.270 /\n\nSALTVD\n 8300 5.0\n 8450 5.0 /\n")


def tracer(text):
    text = replace_once(text, "\nDISGAS\n", "\nDISGAS\nTRACERS\n 0 1 0 0 'NODIFF' /\n")
    text = replace_once(text, "3.22E-6 0.318 0.0 /\n", "3.22E-6 0.318 0.0 /\n\nTRACER\n 'SEA' 'WAT' /\n/\n")
    return replace_once(text, "\n8450 1.270 /\n", "\n8450 1.270 /\n\nTBLKFSEA\n 300*0.0 /\n")


def thermal(text):
    text = replace_once(text, "\nDISGAS\n", "\nDISGAS\nTHERMAL\n")
    text = replace_once(text, "\nPROPS\n", "\nTHCONR\n 300*35 /\n\nPROPS\n")
    text = replace_once(
        text,
        "3.22E-6 0.318 0.0 /\n",
        "3.22E-6 0.318 0.0 /\n\nSPECROCK\n 60 35\n 300 35 /\n\nSPECHEAT\n 60 0.5 1.0 0.5\n 300 0.5 1.0 0.5 /\n",
    )
    text = replace_once(text, "\n8450 1.270 /\n", "\n8450 1.270 /\n\nRTEMPVD\n 8300 200\n 8450 200 /\n")
    # A thermal model needs the temperature of every injector.
    return replace_once(text, "\nTSTEP\n", "\nWTEMP\n 'INJ' 200 /\n/\n\nTSTEP\n")


def idle(text):
    text = replace_once(text, "\n   2 1 1 2 /\n", "\n   3 1 1 3 /\n")
    text = replace_once(
        text,
        "\n/\n-- Coordinates in item 3-4",
        "\n\t'IDLE'\t  'G1'\t   5\t  5\t   8400\t       'OIL' /\n/\n-- Coordinates in item 3-4",
    )
    return replace_once(
        text,
        "\n/\n-- Coordinates in item 2-5",
        "\n\t'IDLE'\t 5\t   5\t 3\t   3\t    'OPEN'\t    1*    1*      0.5 /\n/\n-- Coordinates in item 2-5",
    )


def main():
    variant, source, target = sys.argv[1:4]
    text = open(source).read()
    if variant == "idle":
        text = idle(text)
    else:
        text = water_injector(text)
        text = {"brine": brine, "tracer": tracer, "thermal": thermal}[variant](text)
    open(target, "w").write(text)


if __name__ == "__main__":
    main()
