#!/bin/sh
# Compile and simulate a WinCUPL PLD (MEM_DECODER revision 13), and
# optionally cross-check a second PLD (the read-only revision-12
# reference) against the generated P = 0 equivalence vectors.
#
# Usage:
#   tools/wincupl_build.sh <rev13.pld> <out-dir> [<rev12-reference.pld>]
#
# Outputs land under <out-dir>/rev13/ and <out-dir>/rev12/: the JEDEC fuse
# map, the compiler documentation (product terms, fuse plot), the CSIM
# listing (.so) and waveform file.  Exit codes:
#   0  compiled and simulated clean
#   3  no WinCUPL toolchain found (the caller may skip)
#   1  compile or simulation failure
#
# Notes:
#  * SI_FILE=<vectors.si> selects the simulation vectors of the first
#    design; it defaults to platform/zephyr80/hdl/MEM_DECODER_rev13.si.
#  * CUPL and CSIM are DOS-era programs: they look for the simulation input
#    file under the *source file base name* truncated to 8.3.  The source is
#    therefore copied into the work directory under a short name together
#    with its .si.
#  * CSIM reports failures in the .so listing while the compiler still exits
#    0, so the listing is what decides success.
#  * The generated JEDEC file is named after the PLD's Name field, so the
#    two designs (which share the name Z80_MEM_DECODE) live in separate
#    directories.

set -eu

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "usage: $0 <rev13.pld> <out-dir> [<rev12-reference.pld>]" >&2
    exit 2
fi

REV13_PLD=$1
OUT_DIR=$2
REV12_PLD=${3:-}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

WINCUPL_EXE=${CUPL:-$HOME/.wine/drive_c/WinCUPL/Shared/cupl.exe}
CUPLFLAGS=${CUPLFLAGS:--jaxfs -u C:\\WinCUPL\\Shared\\cupl.dl}

if [ -f "$WINCUPL_EXE" ]; then
    CUPL_BIN=$WINCUPL_EXE
elif command -v cupl >/dev/null 2>&1; then
    CUPL_BIN=cupl
else
    echo "wincupl_build: no WinCUPL (set CUPL=/path/to/cupl.exe)" >&2
    exit 3
fi

run_design() {
    # $1 = source .pld, $2 = work dir, $3 = short base name
    pld=$1
    work=$2
    base=$3
    rm -rf "$work"
    mkdir -p "$work"
    cp "$pld" "$work/$base.pld"
    if [ ! -f "$(dirname "$pld")/$base.si" ] && [ -n "${SI_FILE:-}" ]; then
        cp "$SI_FILE" "$work/$base.si"
    fi
    if [ ! -f "$work/$base.si" ]; then
        echo "wincupl_build: no simulation input $work/$base.si" >&2
        exit 1
    fi
    (
        cd "$work"
        case "$CUPL_BIN" in
        *.exe|*.EXE)
            # shellcheck disable=SC2086
            WINEDEBUG=${WINEDEBUG:--all} wine "$CUPL_BIN" $CUPLFLAGS \
                "$base.pld"
            ;;
        *)
            # shellcheck disable=SC2086
            "$CUPL_BIN" $CUPLFLAGS "$base.pld"
            ;;
        esac
    ) >"$work/cupl.out" 2>&1 || true
    if [ ! -f "$work/$base.jed" ] && ! ls "$work"/*.jed >/dev/null 2>&1; then
        echo "wincupl_build: no JED for $pld:" >&2
        sed -n '1,40p' "$work/cupl.out" >&2
        exit 1
    fi
    # CSIM diagnostics: [NNNNsa] lines in the listing file.
    so=$(ls "$work"/*.so 2>/dev/null | head -1)
    if [ -z "$so" ]; then
        echo "wincupl_build: no simulation listing for $pld" >&2
        exit 1
    fi
    if tr -d '\r' <"$so" | grep -q 'sa\]'; then
        echo "wincupl_build: simulation errors for $pld:" >&2
        tr -d '\r' <"$so" | grep 'sa\]' | sort | uniq -c >&2
        exit 1
    fi
    if grep -q 'Error Code = 1' "$work/cupl.out"; then
        echo "wincupl_build: compiler reported an error for $pld:" >&2
        sed -n '1,40p' "$work/cupl.out" >&2
        exit 1
    fi
    rm -f "$work/$base.pld" "$work/$base.si" "$work/cupl.out" \
        "$work/tmpcsim.im"
    echo "wincupl_build: $base OK -> $work"
}

mkdir -p "$OUT_DIR/rev13"
: "${SI_FILE:=$ROOT/platform/zephyr80/hdl/MEM_DECODER_rev13.si}"
run_design "$REV13_PLD" "$OUT_DIR/rev13" REV13DEC

if [ -n "$REV12_PLD" ]; then
    if [ ! -f "$REV12_PLD" ]; then
        echo "wincupl_build: revision-12 reference not found: $REV12_PLD" >&2
        exit 3
    fi
    rev12_revision=$(sed -n 's/^[Rr]evision[[:space:]]\+\([^;]*\);.*/\1/p' \
        "$REV12_PLD" | head -1 | tr -d '[:space:]')
    case "$rev12_revision" in
    12*)
        ;;
    *)
        echo "wincupl_build: $REV12_PLD is not the revision-12 reference" \
            "(Revision $rev12_revision); cross-check skipped" >&2
        exit 3
        ;;
    esac
    mkdir -p "$OUT_DIR/rev12"
    "$ROOT/tools/rev13_vectors.py" --rev12-si "$OUT_DIR/rev12_si.tmp" \
        --rev12-pld "$REV12_PLD" >/dev/null
    SI_FILE="$OUT_DIR/rev12_si.tmp" \
        run_design "$REV12_PLD" "$OUT_DIR/rev12" REV12DEC
    rm -f "$OUT_DIR/rev12_si.tmp"
fi

exit 0
