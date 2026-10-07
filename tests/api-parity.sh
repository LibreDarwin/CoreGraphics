#!/bin/sh
# Copyright (C) 2026, LibreDarwin
# SPDX-License-Identifier: BSD-3-Clause
#
# Differential parity harness for the accessor surface: how the public
# CGColorSpace functions report a wide matrix of spaces.  api-parity.c is
# compiled twice from the same source:
#
#   oracle  against Apple's CoreGraphics, headers from the SDK
#   ours    against the framework we just built, headers from src/
#
# Both build the same matrix -- every named space from argv plus a fixed set
# of device, calibrated, Lab, ICC-from-data, pattern and indexed
# constructors, each with its extended, linearized and extended-linearized
# derivatives -- and print one line per space carrying the accessor surface.
# The transcripts are compared label by label; any difference is a parity
# failure.
#
# The profile creation-date window (bytes 24..35) is masked by the fixture,
# exactly as profile-parity.c masks it, so the generic Lab profile's live
# timestamp does not hide or invent a difference anywhere else.

set -eu

here=$(dirname "$0")
top=$(cd "$here/.." && pwd)
CONFIG=${CONFIG:-release}
FW=$top/build/$CONFIG/CoreGraphics.framework

SDK=${SDK:-/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk}
CC=${CC:-/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang}

[ -d "$FW" ] || { echo "missing $FW -- run make first" >&2; exit 1; }

mkdir -p "$top/build/$CONFIG/include"
ln -sfn "$top/src" "$top/build/$CONFIG/include/CoreGraphics"

out=${TMPDIR:-/tmp}/cg-api-parity.$$
mkdir -p "$out"

case $CONFIG in
    asan) SAN="-fsanitize=address,undefined -fno-sanitize-recover=all" ;;
    *)    SAN="" ;;
esac

# $1 = output binary, rest = extra flags
build() {
    _bin=$1; shift
    # shellcheck disable=SC2086
    $CC -O1 -std=c11 -D_DARWIN_C_SOURCE -mmacosx-version-min=26.5 \
        -isysroot "$SDK" -Wall -Wextra $SAN \
        -o "$_bin" "$here/api-parity.c" \
        -framework CoreGraphics -framework CoreFoundation "$@"
}

echo "building oracle (Apple CoreGraphics)"
build "$out/oracle"

echo "building ours ($FW)"
build "$out/ours" \
    -I "$top/build/$CONFIG/include" \
    -F "$top/build/$CONFIG" \
    -Wl,-rpath,"$top/build/$CONFIG"

# Refuse to run if the oracle picked up our build tree: the whole point is
# comparing two different implementations, so the binding must really differ.
if otool -L "$out/oracle" | grep -q "$top/build"; then
    echo "oracle is bound to our build tree; refusing to compare" >&2
    exit 1
fi
if ! otool -L "$out/ours" | grep -q '@rpath/CoreGraphics.framework'; then
    echo "ours is not bound to our framework; refusing to compare" >&2
    exit 1
fi

# The names are geometry-parity.c's resolves[] table, so every harness runs
# the same list and there is no second copy to keep in step.
names=$(sed -n '/static const char \*const resolves\[\] = {$/,/^            };$/p' \
    "$here/geometry-parity.c" \
    | sed -nE 's/^                "([A-Za-z0-9_]+)",$/\1/p')
namecount=$(printf '%s\n' "$names" | wc -l | tr -d ' ')
printf 'probing %s named spaces\n' "$namecount"

set -- $names
"$out/oracle" "$@" > "$out/oracle.txt"
"$out/ours"   "$@" > "$out/ours.txt"

total=$(wc -l < "$out/oracle.txt" | tr -d ' ')

# MAXREPORT caps how many mismatches are printed; 0 means all of them.  The
# count in the summary line is always exact.
MAXREPORT=${MAXREPORT:-60}

# Divergences we have investigated, understood as far as we can from the
# outside, and consciously accept.  Set this to the empty string to disable
# the allowlist and require every compared case to match.
ACCEPTED_DIVERGENCES=${ACCEPTED_DIVERGENCES-}

if python3 - "$out/oracle.txt" "$out/ours.txt" "$total" "$MAXREPORT" "$ACCEPTED_DIVERGENCES" <<'PY'
import sys

def load(path):
    recs = {}
    order = []
    with open(path) as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            recs.setdefault(parts[0], []).append(' '.join(parts[1:]))
            order.append(parts[0])
    return recs, order

a, order = load(sys.argv[1])
b, border = load(sys.argv[2])
total = int(sys.argv[3])
maxreport = int(sys.argv[4]) or len(order)
accepted = tuple(p for p in sys.argv[5].split() if p)

def accepted_divergence(label):
    return any(label.startswith(p) for p in accepted)

bad = 0
known = 0
seen = set()
for label in order:
    if label in seen:
        continue
    seen.add(label)
    av = a.get(label, [])
    bv = b.get(label, [])
    if av == bv:
        continue
    if accepted_divergence(label):
        known += 1
        if known <= maxreport:
            print('ACCEPTED %s (documented divergence, not a failure)' % label)
        continue
    bad += 1
    if bad <= maxreport:
        print('MISMATCH %s' % label)
        for i in range(max(len(av), len(bv))):
            x = av[i] if i < len(av) else '<missing>'
            y = bv[i] if i < len(bv) else '<missing>'
            if x != y:
                print('   [%d] apple %s' % (i, x))
                print('        ours  %s' % y)

only_a = [l for l in order if l not in b]
only_b = [l for l in border if l not in a]
for l in only_a[:10]:
    if accepted_divergence(l):
        known += 1
        print('ACCEPTED %s (documented divergence: absent on our side)' % l)
        continue
    print('ONLY-IN-APPLE %s' % l); bad += 1
for l in only_b[:10]:
    if accepted_divergence(l):
        known += 1
        print('ACCEPTED %s (documented divergence: absent on the Apple side)' % l)
        continue
    print('ONLY-IN-OURS %s' % l); bad += 1

if len(only_a) > 10 or len(only_b) > 10:
    print('(truncated ONLY-IN-* lists)')

print()
if known:
    print('%d distinct cases, %d mismatched, %d accepted divergence(s)'
          % (len(seen), bad, known))
else:
    print('%d distinct cases, %d mismatched' % (len(seen), bad))
sys.exit(1 if bad else 0)
PY
then
    echo "PARITY OK ($namecount named + extra cases, $total lines compared; documented private divergences allowed)"
    exit 0
else
    echo "PARITY FAILED"
    exit 1
fi