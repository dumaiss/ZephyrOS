#!/usr/bin/env python3
"""Generate the MEM_DECODER revision-13 WinCUPL simulation vectors.

``platform/zephyr80/hdl/MEM_DECODER_rev13.pld`` is the programmed decoder
(architecture section 36.2) and this tool writes its simulation input file
(``.si``) from one table so the vectors cannot drift from the equations:

* the models below are literal transcriptions of the revision-12 reference
  equations and the revision-13 equations (same term names, same
  operators);
* every mode x address region x bank x page combination is emitted once,
  as a read cycle, plus a small set of write/disable cycles for the chip
  selects;
* the tool proves, before writing, that revision 13 equals revision 12 for
  every case with page = 0, that modes 00/01 ignore the page field, and
  that the mode-10 aperture and the mode-11 window answer exactly where
  the architecture says they do; it fails loudly if the equations do not.

The same table is what ``tests/test_decoder_rev13.py`` drives the
simulator's decoder model with, so the PLD (proven by CSIM against the
``.si``), the ``.si`` vectors and the simulator cannot drift.

Usage::

    python3 tools/rev13_vectors.py            # rewrite the committed .si
    python3 tools/rev13_vectors.py --check    # fail if it is out of date
"""

import argparse
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLD = os.path.join(ROOT, "platform", "zephyr80", "hdl",
                   "MEM_DECODER_rev13.pld")
SI = os.path.join(ROOT, "platform", "zephyr80", "hdl",
                  "MEM_DECODER_rev13.si")
# The revision-12 cross-check file is a build artifact: it is compiled
# against a copy of the read-only reference revision-12 PLD, which is not
# part of this repository, so it is written to the build directory with
# --rev12-si (never committed).

# Address regions by (A15, A14, A13).  The PLD only sees these three bits.
REGIONS = [
    (0, 0, 0, "0000-1FFF"),
    (0, 0, 1, "2000-3FFF"),
    (0, 1, 0, "4000-5FFF"),
    (0, 1, 1, "6000-7FFF"),
    (1, 0, 0, "8000-9FFF"),
    (1, 0, 1, "A000-BFFF"),
    (1, 1, 0, "C000-DFFF"),
    (1, 1, 1, "E000-FFFF"),
]

MODES = [
    (0, 0, "00 ROM read"),
    (0, 1, "01 flat SRAM"),
    (1, 0, "10 application"),
    (1, 1, "11 OS"),
]

WINDOW_BASE = 0x6000
WINDOW_END = 0xE000
APERTURE_BASE = 0x8000
APERTURE_END = 0xC000


def _common_terms(a15, a14, a13, m1):
    common_8k = a15 & a14 & a13
    os_body = (a13 & (1 - a14)) | (a14 & (1 - a15)) | (a15 & (1 - a13))
    return m1 & common_8k, os_body


def _decoder12(a15, a14, a13, m1, m0, q2, q1, q0):
    """The revision-12 reference equations: mode 11 forces bank 7 over
    2000h-DFFFh, common is bank 0, every other mode is the bank field."""
    force_bank0, os_body = _common_terms(a15, a14, a13, m1)
    force_b7 = m1 & m0 & os_body
    a16 = (q0 & (1 - force_bank0)) | force_b7
    a17 = (q1 & (1 - force_bank0)) | force_b7
    a18 = (q2 & (1 - force_bank0)) | force_b7
    return a16, a17, a18


def _decoder13(a15, a14, a13, m1, m0, q2, q1, q0, p2, p1, p0):
    """The revision-13 equations.

    Mode 11: the 6000h-DFFFh window shows bank 7 - page, 2000h-5FFFh
    stays forced to bank 7, 0000h-1FFFh keeps the bank field.  Mode 10:
    the 8000h-BFFFh aperture opens only when the page field is non-zero
    and shows bank 7 - page.  Everything else is revision 12 (the page
    field is only read in those two places)."""
    if m1 and not m0 and a15 and not a14 and (p0 | p1 | p2):
        # The mode-10 far aperture overrides the application view.
        return 1 - p0, 1 - p1, 1 - p2
    force_bank0, os_body = _common_terms(a15, a14, a13, m1)
    win = m1 & m0 & (((1 - a15) & a14 & a13) |
                     (a15 & (1 - (a14 & a13))))
    force_b7 = m1 & m0 & os_body & (1 - win)
    a16 = (q0 & (1 - force_bank0) & (1 - win)) | force_b7 | \
          ((1 - p0) & win)
    a17 = (q1 & (1 - force_bank0) & (1 - win)) | force_b7 | \
          ((1 - p1) & win)
    a18 = (q2 & (1 - force_bank0) & (1 - win)) | force_b7 | \
          ((1 - p2) & win)
    return a16, a17, a18


def region(a15, a14, a13):
    for r in REGIONS:
        if (r[0], r[1], r[2]) == (a15, a14, a13):
            return r
    raise AssertionError("no region for %d%d%d" % (a15, a14, a13))


def case_bits(case):
    a15, a14, a13, m1, m0, bank, page = case
    return (a15, a14, a13, m1, m0,
            (bank >> 2) & 1, (bank >> 1) & 1, bank & 1,
            (page >> 2) & 1, (page >> 1) & 1, page & 1)


def model_for(case, revision):
    a15, a14, a13, m1, m0, q2, q1, q0, p2, p1, p0 = case_bits(case)
    if revision == 12:
        return _decoder12(a15, a14, a13, m1, m0, q2, q1, q0)
    return _decoder13(a15, a14, a13, m1, m0, q2, q1, q0, p2, p1, p0)


def equivalence_cases():
    """Cases for the revision-12 cross-check file.

    Revision 12 has no PAGE pins, so one page-0 vector per mode x region
    x bank is the whole input space it can see.  The revision-13 file
    covers the same cases at every page (that is what makes the P = 0
    rule testable: the same expectations pass with the page field
    varied)."""
    for m1, m0, _ in MODES:
        for a15, a14, a13, _ in REGIONS:
            for bank in range(8):
                yield (a15, a14, a13, m1, m0, bank, 0)


def window_cases():
    """Every mode x region x bank x page combination."""
    for m1, m0, _ in MODES:
        for a15, a14, a13, _ in REGIONS:
            for bank in range(8):
                for page in range(8):
                    yield (a15, a14, a13, m1, m0, bank, page)


def check_equivalence():
    """Prove the revision-13 equations equal revision 12 whenever P = 0.

    Page 0 is the whole revision-12 input space: the page field is only
    read by the mode-11 window and the mode-10 aperture, and neither is
    active then."""
    for case in window_cases():
        if case[6] != 0:
            continue
        new = model_for(case, 13)
        old = model_for(case, 12)
        if new != old:
            raise SystemExit(
                "revision 13 differs from revision 12 at "
                "%s mode=%d%d bank=%d page=0: %s vs %s"
                % (region(*case[:3])[3], case[3], case[4], case[5],
                   new, old))


def check_page_rules():
    """Prove the page field does exactly what the architecture says.

    Modes 00/01 ignore it; mode 10 equals revision 12 outside the
    aperture and shows bank 7 - page inside it; mode 11 keeps the bank
    field over 0000h-1FFFh, forces bank 7 over 2000h-5FFFh, shows bank
    7 - page over 6000h-DFFFh and bank 0 over common memory."""
    for case in window_cases():
        a15, a14, a13, m1, m0, bank, page = case
        addr = (a15 << 15) | (a14 << 14) | (a13 << 13)
        bank_bits = (bank & 1, (bank >> 1) & 1, (bank >> 2) & 1)
        new = model_for(case, 13)
        if not m1:
            # Modes 00 and 01 ignore the page field entirely: revision 12.
            want = model_for(case, 12)
        elif not m0:
            if APERTURE_BASE <= addr < APERTURE_END and page != 0:
                far = 7 - page
                want = (far & 1, (far >> 1) & 1, (far >> 2) & 1)
            else:
                want = model_for(case, 12)
        else:
            if addr < 0x2000:
                want = bank_bits
            elif addr < WINDOW_BASE:
                want = (1, 1, 1)
            elif addr < WINDOW_END:
                far = 7 - page
                want = (far & 1, (far >> 1) & 1, (far >> 2) & 1)
            else:
                want = (0, 0, 0)
        if new != want:
            raise SystemExit(
                "revision 13 is wrong at %s mode=%d%d bank=%d page=%d: "
                "%s vs %s" % (region(*case[:3])[3], m1, m0, bank, page,
                              new, want))


def pld_header(path=None):
    """The header block of a .pld, copied verbatim."""
    lines = []
    with open(path or PLD) as handle:
        source = handle.read().splitlines()
    for line in source:
        lines.append(line.rstrip("\n"))
        if line.strip().startswith("Device"):
            break
    return lines


INPUTS_BASE = [
    "A13", "A14", "A15", "MREQ", "MEM_MODE0", "MEM_MODE1", "!WR", "!RD",
    "BANK_Q0", "BANK_Q1", "BANK_Q2",
]
INPUTS_PAGE = ["PAGE0", "PAGE1", "PAGE2"]
OUTPUTS = [
    "!SRAM_CS", "!ROM_CS", "!CART_CS", "RAM_A16", "RAM_A17", "RAM_A18",
]
# Revision 12 has no PAGE pins at all; its equivalence file therefore has
# no page columns and only page 0 needs enumerating there.
ORDER = {
    13: INPUTS_BASE + INPUTS_PAGE + OUTPUTS,
    12: INPUTS_BASE + OUTPUTS,
}

TITLES = {
    13: ("Revision 13 (programmed): mode-11 6000h-DFFFh window\n"
         " * (bank 7 - page) and mode-10 8000h-BFFFh far aperture,\n"
         " * open only when page != 0.  P = 0 is revision 12 in every\n"
         " * mode; modes 00/01 ignore the page field.\n"
         " * Generated from the equations in the .pld header."),
    12: ("Revision 12 cross-check vectors: the revision-12 input\n"
         " * space (no PAGE pins) at page 0, with expectations from\n"
         " * the revision-12 equations.  Compiling an unmodified copy\n"
         " * of the reference revision-12 PLD against this file, and\n"
         " * the revision-13 PLD against its own file, proves the two\n"
         " * agree on the equivalence domain: page = 0 for every\n"
         " * mode/region/bank."),
}


def vector(a15, a14, a13, m1, m0, bank, page, mreq, rd, wr, revision):
    """One vector line: inputs then output expectations (pin levels)."""
    a16, a17, a18 = model_for((a15, a14, a13, m1, m0, bank, page),
                              revision)
    q2, q1, q0 = (bank >> 2) & 1, (bank >> 1) & 1, bank & 1
    p2, p1, p0 = (page >> 2) & 1, (page >> 1) & 1, page & 1
    rom_read = (1 - m1) & (1 - m0)
    # Chip-select activity from the equations (1 = selected).
    rom_active = 1 if (mreq == 0 and rd == 0 and rom_read) else 0
    sram_active = 1 if (mreq == 0 and (wr == 0 or (rd == 0 and not rom_read))) \
        else 0
    inputs = "".join(str(v) for v in (
        a13, a14, a15, mreq, m0, m1, wr, rd, q0, q1, q2))
    if revision == 13:
        inputs += "".join(str(v) for v in (p0, p1, p2))
    outputs = [
        "L" if sram_active else "H",
        "L" if rom_active else "H",
        "N",
        "H" if a16 else "L",
        "H" if a17 else "L",
        "H" if a18 else "L",
    ]
    return "%s %s" % (inputs, "".join(outputs))


def build_si(revision, header=None, header_revision=None):
    out = []
    out.extend(pld_header(header))
    if header_revision is not None:
        out = ["Revision %d;" % header_revision if line.startswith("Revision")
               else line for line in out]
    out.append("")
    out.append("/* %s" % TITLES[revision])
    out.append(" *")
    out.append(" * Generated by tools/rev13_vectors.py -- do not edit.")
    out.append(" * One vector per mode x address region x bank%s, as a"
               % (" x page" if revision == 13 else ""))
    out.append(" * read cycle (MREQ=0, RD=0, WR=1), plus chip-select")
    out.append(" * disable/write cycles at the end.  !CART_CS is not tested")
    out.append(" * (N): pin 21 has no equation in either revision. */")
    out.append("")
    out.append("ORDER: %s;" % ", ".join(ORDER[revision]))
    out.append("")
    out.append("VECTORS:")
    out.append("$MSG \"    AAAAAAAAAAAA   SRO RRR\";")
    out.append("$MSG \"    111         MMM  AOR AAA\";")
    out.append("$MSG \"    345         REE  MMM 123\";")
    out.append("$MSG \"    ------------ ---- -------\";")
    cases = window_cases() if revision == 13 else equivalence_cases()
    last_mode = None
    for a15, a14, a13, m1, m0, bank, page in cases:
        if (m1, m0) != last_mode:
            last_mode = (m1, m0)
            out.append("/* mode %s */"
                       % [m[2] for m in MODES if (m[0], m[1]) == last_mode][0])
        suffix = "page %d" % page if revision == 13 else "no page pins"
        out.append("%s /* %s bank %d %s */" % (
            vector(a15, a14, a13, m1, m0, bank, page, 0, 0, 1, revision),
            region(a15, a14, a13)[3], bank, suffix))
    out.append("")
    out.append("/* chip-select cycles: MREQ high (no chip), read, write */")
    for (mreq, rd, wr, note) in (
            (1, 0, 1, "MREQ high: neither chip"),
            (0, 1, 0, "write cycle"),
            (0, 1, 1, "read cycle with no ROM in modes 10/11"),
            (0, 0, 1, "mode 00 ROM read")):
        for mode in MODES:
            out.append("%s /* %s, mode %s */" % (
                vector(0, 0, 0, mode[0], mode[1], 0, 0, mreq, rd, wr,
                       revision), note, mode[2]))
    out.append("")
    return "\n".join(out) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="exit 1 when the committed .si is out of date")
    parser.add_argument("--rev12-si", metavar="PATH",
                        help="write the revision-12 cross-check .si to PATH")
    parser.add_argument("--rev12-pld", metavar="PATH",
                        help="copy the header of this revision-12 PLD "
                             "into the cross-check .si (warning-free CSIM)")
    args = parser.parse_args(argv)

    check_equivalence()
    check_page_rules()
    rev13 = build_si(13)
    rev12 = build_si(12, args.rev12_pld, 12)

    if args.rev12_si:
        with open(args.rev12_si, "w") as handle:
            handle.write(rev12)
        print("wrote %s" % args.rev12_si)
        return 0

    if args.check:
        current = open(SI).read() if os.path.exists(SI) else None
        if current != rev13:
            print("%s is out of date; run tools/rev13_vectors.py"
                  % os.path.relpath(SI, ROOT), file=sys.stderr)
            return 1
        print("%s is up to date" % os.path.relpath(SI, ROOT))
        return 0
    old = open(SI).read() if os.path.exists(SI) else None
    if old != rev13:
        with open(SI, "w") as handle:
            handle.write(rev13)
        print("wrote %s (%d vectors)" % (os.path.relpath(SI, ROOT),
                                        rev13.count("\n")))
    else:
        print("%s already up to date" % os.path.relpath(SI, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
