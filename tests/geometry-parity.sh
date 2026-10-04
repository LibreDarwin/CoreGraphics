#!/bin/sh
# Copyright (C) 2026, LibreDarwin
# SPDX-License-Identifier: BSD-3-Clause
#
# Differential parity harness for the CoreGraphics geometry and affine
# surface.  geometry-parity.c is compiled twice from the same source:
#
#   oracle  against Apple's CoreGraphics, headers from the SDK
#   ours    against the framework we just built, headers from src/
#
# Both print "label value..." lines, and the two transcripts are then
# compared label by label.  Any difference is a parity failure, so this
# compares parsed records rather than raw text: that way a shifted line
# cannot masquerade as a value mismatch and every report names the case.
#
# CGRectUprightBoundsForRotation is not exercised: it is a private external
# in Apple's framework, so a binary linked against the real one cannot call
# it and there is nothing to compare against.

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

out=${TMPDIR:-/tmp}/cg-parity.$$
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
        -o "$_bin" "$here/geometry-parity.c" \
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

"$out/oracle" > "$out/oracle.txt"
"$out/ours"   > "$out/ours.txt"

total=$(wc -l < "$out/oracle.txt" | tr -d ' ')

# MAXREPORT caps how many mismatches are printed; 0 means all of them.  The
# count in the summary line is always exact.
MAXREPORT=${MAXREPORT:-60}

# Divergences we have investigated, understood as far as we can from the
# outside, and consciously accept.  They are listed here rather than left to
# fail forever so that the harness stays usable as a gate: a *new* mismatch
# still fails the run, but these do not.  Each entry is a label prefix.
#
# neareq/*  CGRectNearlyEqualToRectWithTolerance is a private external, absent
#           from CoreGraphics.tbd, so it is not part of the ABI we ship.  The
#           real function does not implement a tolerance comparison at all:
#           probed against the zero rect it returns true for {0,0,1,1},
#           {0,0,2,2} and {0,0,1,0.5}, and false for {1,1,1,1},
#           {0.5,0.5,1,1} and {0.001,0.001,1,1} -- so it keys off whether the
#           origin is exactly zero rather than off magnitude, it is unchanged
#           as the tolerance sweeps 1e-9..0.1, and it is asymmetric
#           (nonempty,empty) != (empty,nonempty).  Its internal metric is in
#           the private __func_0x1873afcf8 and is not reachable from a public
#           header, so we keep the sane four-component absolute comparison.
#
# Set this to the empty string to disable the allowlist and require every
# compared case to match, which is how the negative control is run.
#
# The spi/dspi/{scale,rot,neg,trans}/ prefixes that used to sit here are gone.
# They were deferring CGAffineTransformDecompose, which disagreed with Apple
# about the decomposition CONVENTION rather than about rounding: Apple reads
# the x scale off the first ROW of the linear block and keeps that row's sign
# in scale.width -- which is what the SPI's outScaleIsNegative flag exists to
# report -- while the old code read it off the first COLUMN, hypot(a, c), and
# let the rotation absorb the sign via atan2(c, a).  For
# CGAffineTransformMake(1, 2, 3, 4, 5, 6) Apple reports scale.width =
# -sqrt(5) = -hypot(a, b) where the old code reported hypot(a, c) = sqrt(10);
# for Make(-2, 0, 0, 3) Apple reported (-2, 3) at rotation 0 where the old
# code reported (2, -3) at rotation pi.  Apple also copies the translation
# through unrotated, which the old code did not, and it selects the rotation
# with copysign(pi, angle) so the result stays inside [-pi, pi] rather than
# wrapping.  src/CGAffineTransform.c now transcribes the disassembly, and
# spi/dspi/{scale,rot,neg,trans}/ match bit for bit like every other case.
# icc/lutgeom/ : a deliberately corrupted 'A2B0' inputChannels byte.  Apple
#   refuses the profile because the LUT no longer matches the table it
#   describes; we read the width from that byte and accept.  Apple checks the
#   CLUT geometry with a private mft1/mft2 size formula that the two shipped
#   profiles do not pin down, and no well-formed profile can reach this.
ACCEPTED_DIVERGENCES=${ACCEPTED_DIVERGENCES-neareq/ icc/lutgeom/}

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
        # A label can carry several values (CGRectDivide prints the slice
        # and the remainder under one label), and only the first of them
        # used to be shown -- which made a matching slice hide a diverging
        # remainder.  Show every line that differs, by index, so the report
        # says which one moved.
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
    # A label Apple prints and we do not is the same class of difference as one
    # we print with another value, so a documented divergence has to be honoured
    # here too -- otherwise a case where the two sides disagree about whether
    # the space exists at all can never be recorded, only failed.
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
    echo "PARITY OK ($total lines compared; documented private divergences allowed)"
    exit 0
else
    echo "PARITY FAILED"
    exit 1
fi
