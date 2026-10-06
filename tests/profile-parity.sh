#!/bin/sh
# Copyright (C) 2026, LibreDarwin
# SPDX-License-Identifier: BSD-3-Clause
#
# Differential parity harness for the full ICC profile surface: the named
# profiles CGColorSpaceCreateWithName hands back.  profile-parity.c is
# compiled twice from the same source:
#
#   oracle  against Apple's CoreGraphics, headers from the SDK
#   ours    against the framework we just built, headers from src/
#
# Both parse the same list of names -- taken from geometry-parity.c's
# resolves[] table, so the two harnesses cannot drift -- and print a
# structured dump of each profile: the header fields, the tag table, a
# parsed record for every block whose type is understood, and hex of every
# block.  The transcripts are then compared label by label; any difference
# is a parity failure.
#
# The masked creation-date window (bytes 24..35) is the one deliberate
# exception, for the same reason geometry-parity.c masks it: the generic Lab
# profile carries a live timestamp.  Masking that window on both sides cannot
# hide a difference in any of the other forty-three byte-identical profiles.

set -eu

here=$(dirname "$0")
top=$(cd "$here/.." && pwd)
CONFIG=${CONFIG:-release}
FW=$top/build/$CONFIG/CoreGraphics.framework

SDK=${SDK:-/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk}
# Apple's Xcode toolchain, not the from-source tree under
# xnuports-root/devel/xcode-tools: that one ships only the profiling
# compiler runtimes and no libclang_rt.asan_* at all.
CC=${CC:-/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin/clang}

[ -d "$FW" ] || { echo "missing $FW -- run make first" >&2; exit 1; }

# The framework ships no Headers directory, so give the compiler the
# framework-style include path the install target would produce.
mkdir -p "$top/build/$CONFIG/include"
ln -sfn "$top/src" "$top/build/$CONFIG/include/CoreGraphics"

out=${TMPDIR:-/tmp}/cg-profile-parity.$$
mkdir -p "$out"

# Under CONFIG=asan the framework is instrumented, but the fixture that
# calls into it is not, so the interesting half of the work would go
# unchecked.  Mirror the config's sanitizer flags onto the fixture too and
# make UBSan fatal, so a divergence shows up as a crash rather than as a
# "runtime error:" line on stderr that scrolls past the transcript
# comparison.  SAN is empty for every other config.
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
        -o "$_bin" "$here/profile-parity.c" \
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

# The names are geometry-parity.c's resolves[] table, so both harnesses run
# the same list and there is no second copy to keep in step.
names=$(sed -n '/static const char \*const resolves\[\] = {$/,/^            };$/p' \
    "$here/geometry-parity.c" \
    | sed -nE 's/^                "([A-Za-z0-9_]+)",$/\1/p')
namecount=$(printf '%s\n' "$names" | wc -l | tr -d ' ')
printf 'dumping %s profiles\n' "$namecount"

set -- $names
"$out/oracle" "$@" > "$out/oracle.txt"
"$out/ours"   "$@" > "$out/ours.txt"

total=$(wc -l < "$out/oracle.txt" | tr -d ' ')

# MAXREPORT caps how many mismatches are printed; 0 means all of them.  The
# count in the summary line is always exact.
MAXREPORT=${MAXREPORT:-60}

# Divergences we have investigated, understood as far as we can from the
# outside, and consciously accept.  Set this to the empty string to disable
# the allowlist and require every compared case to match.  The full-dump
# differential currently has none: the profiles are byte-identical (already
# gated), so a mismatch here is a genuine regression.
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
    echo "PARITY OK ($namecount profiles, $total lines compared; documented private divergences allowed)"
    exit 0
else
    echo "PARITY FAILED"
    exit 1
fi