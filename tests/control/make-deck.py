#!/usr/bin/env python3
"""Write an SPE1 variant for the composition tests.

Usage: make-deck.py VARIANT SPE1_DECK TARGET

Both variants turn INJ into a water injector. brine adds BRINE, water
properties that depend on salt (PVTWSALT) and an initial salt
concentration. tracer adds one water tracer, SEA, starting at zero.
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


def main():
    variant, source, target = sys.argv[1:4]
    text = water_injector(open(source).read())
    text = {"brine": brine, "tracer": tracer}[variant](text)
    open(target, "w").write(text)


if __name__ == "__main__":
    main()
