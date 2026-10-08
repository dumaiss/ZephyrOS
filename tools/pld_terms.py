#!/usr/bin/env python3
"""Product-term budget for the revision-13 memory decoder.

The 22V10 the memory decoder is fitted into has a per-output product-term
budget of 10 terms on RAM_A16 (pin 15), 12 on RAM_A17 and 14 on RAM_A18.
This tool derives, by exact two-level minimisation, the number of product
terms each RAM_A output needs for the programmed design:

* ``revision-13`` -- the mode-11 6000h-DFFFh window (bank 7 - page) plus
                     the mode-10 8000h-BFFFh far aperture, which opens
                     only when the page field is non-zero so that P = 0
                     stays revision 12 in every mode.

The function is the PLD equation (same term names as the .pld file)
evaluated over the complete input space: A13-A15, the two mode bits, the
three bank bits and the three page bits (2048 minterms).  Minimisation is
Quine-McCluskey over that space followed by a minimum-cover search, so the
printed counts are the exact minimum for a two-level AND-OR fit, not a
hand estimate.

Usage::

    python3 tools/pld_terms.py               # the table
    python3 tools/pld_terms.py --json PATH   # machine-readable copy
"""

import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Input variable order; bit i of a minterm selects variable i.
VARIABLES = ["A13", "A14", "A15", "M0", "M1", "P0", "P1", "P2",
             "Q0", "Q1", "Q2"]

BUDGETS = {"RAM_A16": 10, "RAM_A17": 12, "RAM_A18": 14}

# Pin names used in the .pld files, mapped to the VARIABLES shorthand.
PIN_ALIASES = dict((name, name) for name in VARIABLES)
PIN_ALIASES.update({"MEM_MODE0": "M0", "MEM_MODE1": "M1",
                    "PAGE0": "P0", "PAGE1": "P1", "PAGE2": "P2",
                    "BANK_Q0": "Q0", "BANK_Q1": "Q1", "BANK_Q2": "Q2"})

def bits(value):
    return [(value >> i) & 1 for i in range(len(VARIABLES))]


def revision13_bits(a13, a14, a15, m0, m1, p0, p1, p2, q0, q1, q2):
    """The revision-13 decoder equations (architecture section 36.2).

    Mode 11: the 6000h-DFFFh window shows bank 7 - page, 2000h-5FFFh is
    forced to bank 7 and 0000h-1FFFh keeps the bank field.  Mode 10: the
    8000h-BFFFh aperture opens only when the page field is non-zero and
    shows bank 7 - page.  Everything else is revision 12."""
    if m1 and not m0 and a15 and not a14 and (p0 | p1 | p2):
        return 1 - p0, 1 - p1, 1 - p2
    common_8k = a15 & a14 & a13
    os_body = (a13 & (1 - a14)) | (a14 & (1 - a15)) | (a15 & (1 - a13))
    win = m1 & m0 & (((1 - a15) & a14 & a13) |
                     (a15 & (1 - (a14 & a13))))
    force_bank0 = m1 & common_8k
    force_b7 = m1 & m0 & os_body & (1 - win)
    a16 = (q0 & (1 - force_bank0) & (1 - win)) | force_b7 | \
          ((1 - p0) & win)
    a17 = (q1 & (1 - force_bank0) & (1 - win)) | force_b7 | \
          ((1 - p1) & win)
    a18 = (q2 & (1 - force_bank0) & (1 - win)) | force_b7 | \
          ((1 - p2) & win)
    return a16, a17, a18


def all_minterms(function):
    """The ON-set of a 3-output PLD function over the whole input space."""
    on = [[], [], []]
    for value in range(1 << len(VARIABLES)):
        args = bits(value)
        outputs = function(*args)
        for index, bit in enumerate(outputs):
            if bit:
                on[index].append(value)
    return on


def prime_implicants(ones):
    """Quine-McCluskey primes of a minterm list."""
    terms = [(m, 0) for m in ones]
    primes = []
    while terms:
        used = [False] * len(terms)
        by_mask = {}
        for index, (value, mask) in enumerate(terms):
            by_mask.setdefault(mask, []).append((value, index))
        merged = set()
        for mask, group in by_mask.items():
            for i in range(len(group)):
                v1, i1 = group[i]
                for j in range(i + 1, len(group)):
                    v2, i2 = group[j]
                    diff = v1 ^ v2
                    if diff & mask or (diff & (diff - 1)):
                        continue
                    used[i1] = used[i2] = True
                    merged.add((v1 & ~diff, mask | diff))
        for index, (value, mask) in enumerate(terms):
            if not used[index]:
                primes.append((value, mask))
        terms = sorted(merged)
    return sorted(set(primes))


def min_cover(ones):
    """A minimum two-level cover of the ON-set, as (value, mask) primes.

    Same search as min_terms, but returns the chosen implicants so the
    minimal equation can be written out (a PLD whose natural form exceeds
    the part's budget can still be programmed in its exact minimum form)."""
    if not ones:
        return []
    primes = prime_implicants(ones)
    covers = []
    for value, mask in primes:
        covered = frozenset(m for m in ones if (m & ~mask) == value)
        covers.append(((value, mask), covered))
    remaining = set(ones)
    chosen = []
    # Essential primes first.
    while True:
        counts = {}
        for _, covered in covers:
            for m in (covered & remaining):
                counts[m] = counts.get(m, 0) + 1
        essential = [(prime, covered) for prime, covered in covers
                     if any(counts[m] == 1 for m in (covered & remaining))]
        if not essential:
            break
        for prime, covered in essential:
            chosen.append(prime)
            remaining -= covered
        covers = [(prime, covered) for prime, covered in covers
                  if covered & remaining]
        if not remaining:
            return chosen
    # Branch and bound on what is left.
    best = [len(covers) + 1, None]

    def search(depth, uncovered, picked):
        if depth >= best[0]:
            return
        if not uncovered:
            best[0] = depth
            best[1] = list(picked)
            return
        pivot = min(uncovered, key=lambda m: sum(
            1 for _, covered in covers if m in covered))
        for prime, covered in covers:
            if pivot in covered:
                picked.append(prime)
                search(depth + 1, uncovered - covered, picked)
                picked.pop()

    search(0, remaining, [])
    return chosen + best[1]


def off_set(ones):
    """The complement of an ON-set over the whole input space."""
    present = set(ones)
    return [m for m in range(1 << len(VARIABLES)) if m not in present]


def min_terms(ones):
    """Minimum number of product terms covering the ON-set."""
    return len(min_cover(ones))


def format_term(value, mask):
    """One product term of the VOCABULARY variables, CUPL syntax."""
    literals = []
    for index, name in enumerate(VARIABLES):
        if (mask >> index) & 1:
            continue
        literals.append("%s%s" % ("" if (value >> index) & 1 else "!",
                                  name))
    return " & ".join(literals)


def variant_functions():
    """The named PLD functions, in the `variants()` order."""
    return (
        ("revision-13", revision13_bits),
    )


def format_equations(name, inverted=False):
    """The exact minimum-cover equations of one variant, as CUPL lines.

    `inverted` writes each output as its complement (!RAM_A1x = ...),
    which the 22V10's output polarity fuse programs directly."""
    on = variant_on_sets(name)
    lines = []
    for index, output in enumerate(("RAM_A16", "RAM_A17", "RAM_A18")):
        cover = min_cover(off_set(on[index]) if inverted else on[index])
        lines.append("%s%s = %s;" % ("!" if inverted else "", output,
                                      " # ".join(
            format_term(value, mask) for value, mask in cover)))
    return "\n".join(lines)


def canonical_term(term):
    """One product term in canonical form, or None if it is not a plain
    product of known pin literals (parenthesised subexpressions, unknown
    names): such a term is never a term of the generated minimum cover."""
    literals = []
    for literal in term.split("&"):
        literal = literal.strip()
        if not literal:
            return None
        negated = literal.startswith("!")
        name = literal.lstrip("!")
        if name not in PIN_ALIASES:
            return None
        literals.append("%s%s" % ("!" if negated else "", PIN_ALIASES[name]))
    return "&".join(sorted(literals))


def parse_pld_equations(path):
    """The RAM_A covers of a .pld file, as {output: set of terms}."""
    with open(path) as handle:
        text = handle.read()
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    covers = {}
    for match in re.finditer(r"(!?)\b(RAM_A1[678])\s*=\s*(.*?);", text,
                             re.S):
        negated, output, expression = match.groups()
        if negated:
            output = "!" + output
        terms = set()
        for term in expression.split("#"):
            canonical = canonical_term(term)
            terms.add(canonical if canonical
                      else " ".join(term.split()) or "?")
        covers[output] = terms
    return covers


def check_pld(path, variant="revision-13"):
    """Problems where a .pld's RAM_A equations differ from the minimum."""
    on = variant_on_sets(variant)
    problems = []
    covers = parse_pld_equations(path)
    for index, output in enumerate(("RAM_A16", "RAM_A17", "RAM_A18")):
        want = set()
        for value, mask in min_cover(on[index]):
            want.add(canonical_term(format_term(value, mask)))
        have = covers.get(output)
        if have is None and ("!" + output) in covers:
            # Inverted polarity: the equation is the complement's cover.
            have = covers["!" + output]
            want = set()
            for value, mask in min_cover(off_set(on[index])):
                want.add(canonical_term(format_term(value, mask)))
        if have is None:
            problems.append("%s: no equation" % output)
            continue
        for term in sorted(want - have):
            problems.append("%s: missing term %s" % (output, term))
        for term in sorted(have - want):
            problems.append("%s: not part of the minimum cover: %s"
                            % (output, term))
    return problems


def variant_on_sets(name):
    functions = dict(variant_functions())
    if name not in functions:
        raise KeyError("unknown variant %r" % name)
    return all_minterms(functions[name])


def variants():
    table = []
    for name, function in variant_functions():
        on = all_minterms(function)
        counts = [min_terms(ones) for ones in on]
        inverted = [min_terms(off_set(ones)) for ones in on]
        keys = ("RAM_A16", "RAM_A17", "RAM_A18")
        table.append({
            "variant": name,
            "terms": dict(zip(keys, counts)),
            # The 22V10 output polarity fuse lets each output be written
            # as its complement (!RAM_A1x = ...): that cover counts too.
            "terms_inverted": dict(zip(keys, inverted)),
            "budget": dict(BUDGETS),
            "fit_true_polarity": all(counts[i] <= BUDGETS[key]
                                     for i, key in enumerate(keys)),
            "fit": all(min(counts[i], inverted[i]) <= BUDGETS[key]
                       for i, key in enumerate(keys)),
        })
    return table


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", metavar="PATH",
                        help="also write the table as JSON")
    parser.add_argument("--equations", metavar="VARIANT", action="append",
                        default=[], help="print the exact minimum-cover "
                        "equations of a variant (repeatable)")
    parser.add_argument("--inverted", action="store_true",
                        help="with --equations: print the inverted-polarity "
                        "(!RAM_A1x = ...) covers")
    parser.add_argument("--check-pld", metavar="PATH",
                        help="verify that a .pld's RAM_A equations are the "
                        "exact minimum cover of --pld-variant")
    parser.add_argument("--pld-variant", metavar="VARIANT",
                        default="revision-13",
                        help="the variant --check-pld compares against "
                        "(default revision-13)")
    args = parser.parse_args(argv)
    table = variants()
    if args.check_pld:
        if not os.path.isfile(args.check_pld):
            parser.error("no such file: %s" % args.check_pld)
        problems = check_pld(args.check_pld, args.pld_variant)
        for problem in problems:
            print("pld check: %s: %s" % (args.check_pld, problem),
                  file=sys.stderr)
        if problems:
            return 1
        print("pld check: %s is the exact minimum cover of %s"
              % (args.check_pld, args.pld_variant), file=sys.stderr)
    if args.equations:
        known = {row["variant"] for row in table}
        for name in args.equations:
            if name not in known:
                parser.error("unknown variant %r (choose from %s)"
                             % (name, ", ".join(sorted(known))))
        for name in args.equations:
            print("/* %s */" % name)
            print(format_equations(name, args.inverted))
        return 0
    print("minimum two-level product terms per RAM_A output "
          "(budgets: A16=%d, A17=%d, A18=%d)"
          % (BUDGETS["RAM_A16"], BUDGETS["RAM_A17"], BUDGETS["RAM_A18"]))
    print("(true polarity / inverted polarity; the 22V10 polarity fuse "
          "lets either form be programmed)")
    print("%-28s %-8s %-8s %-8s %s"
          % ("variant", "RAM_A16", "RAM_A17", "RAM_A18", "fit"))
    for row in table:
        counts, inv = row["terms"], row["terms_inverted"]
        cells = ["%d/%d" % (counts[k], inv[k])
                 for k in ("RAM_A16", "RAM_A17", "RAM_A18")]
        print("%-28s %-8s %-8s %-8s %s"
              % (row["variant"], cells[0], cells[1], cells[2],
                 "yes" if row["fit"] else "NO"))
    if args.json:
        with open(args.json, "w") as handle:
            json.dump({"variables": VARIABLES, "budgets": BUDGETS,
                       "variants": table}, handle, indent=2)
        print("wrote %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
