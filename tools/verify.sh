#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go, in the order that
# fails fastest.
#
#   tools/verify.sh
#
# Each check answers a question none of the others can:
#
#   build         a FRESH universal Release build. Not the dev build: CMake
#                 latches the architecture list at the first target, so the
#                 only build worth measuring is one configured from nothing.
#   shaders       does every shader compile, through a real GLSL compiler,
#                 before a host has to find out. The shaders are assembled at
#                 run time from their bodies, so the text compiled here is what
#                 `sntest --dump-shaders` writes: the exact strings the plugin
#                 hands the driver. Then a grep for every GLSL 4.10 reserved
#                 word used as an identifier, because Apple's compiler and
#                 glslc accept some (`packed`) that Mesa refuses.
#   offline       the checks that need no GL: names, the reference EDT against
#                 brute force, the cutter on random grids with an exact flood,
#                 the band's connectivity and width, the footprint's weights,
#                 and each one's negative control.
#   physics       every GL check, at TWO rasters: 320x180, which is what CI
#                 renders at, and 1280x720. Each measures its property out of
#                 the plugin's own picture with the harness's own geometry:
#                   --islands    no piece of sheet floats, labelled from the output
#                   --shortest   each bridge the shortest gap, against brute force
#                   --width      bridges the stated width, and 4-connected
#                   --overspray  the edge profile the cone's chord fraction
#                   --layers     n layers, n + 1 plateaus, dark over light
#                   --stability  bridges translate with the picture; slow-pan churn
#                   --resize     last frame's bridges survive a resize
#                   --negative   every one of those FAILS on a perturbed model
#                 and again on Apple's SOFTWARE renderer (CI's), which is not
#                 bit-repeatable and may round differently.
#   pipe          the fleet's frame contract, plus what a closed stdout (exit
#                 1, not SIGPIPE's 141), an unknown cue, a failed render, a
#                 stepped option, a stepped boolean, a stepped integer and a
#                 ramped slider do. (Stencil has no event but the About buttons.)
#   sweep         does every control change the picture. A GLSL uniform whose
#                 name does not match the C++ is ignored without a word.
#   bench         the render cost and the bridging's share, for the record.
#   registration  does the bundle contain a plugin at all -- a file-scope
#                 CFFGLPluginInfo nothing names, which a linker may drop while
#                 still producing a bundle that loads and exports plugMain.
#   lipo          is the build really universal.
#   plist         does CFBundleExecutable name the binary that is on disk.
#   codesign      the exact command the release job runs, against a copy.
#   oxbow         a real FFGL host loads the bundle and reports the name, id
#                 and type it sees -- the name field is not null-terminated
#                 and a host truncates silently past 16 characters.
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build-universal}"
failures=0

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures=$(( failures + 1 )); }

step "build (fresh universal Release, $BUILD)"
rm -rf "$BUILD"
if cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 \
   && cmake --build "$BUILD" --parallel >/dev/null 2>&1; then
	pass "builds"
else
	fail "build failed -- run: cmake -B $BUILD -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD"
	exit 1
fi

SNTEST="$BUILD/sntest"

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails. No shaders at all is a FAILURE: it means the dump broke.
#---------------------------------------------------------------------------
step "shaders"
dir="$( mktemp -d )"
"$SNTEST" --dump-shaders "$dir" >/dev/null
if ! command -v glslc >/dev/null 2>&1; then
	printf '   skipped: glslc not installed (brew install shaderc)\n'
else
	n=0; bad=0
	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done
	if [ "$n" -eq 0 ]; then
		fail "no shaders were dumped"
	elif [ "$bad" -eq 0 ]; then
		pass "all $n shaders compile"
	else
		fail "$bad of $n shaders do not compile"
	fi
fi
# The GLSL 4.10 reserved words (s3.6 of the spec: keywords reserved for future
# use, plus the ones the fleet has been bitten by) as identifiers. Apple's
# compiler and glslc accept `packed`; Mesa's llvmpipe, which the Arena gate
# runs on, does not. Matched as whole words on lines with the comments cut.
reserved='common|partition|active|asm|class|union|enum|typedef|template|this|packed|resource|goto|inline|noinline|public|static|extern|external|interface|long|short|half|fixed|unsigned|superp|input|output|hvec2|hvec3|hvec4|fvec2|fvec3|fvec4|sampler3DRect|filter|sizeof|cast|namespace|using|row_major|patch|sample|subroutine|far|near'
hits=$( cat "$dir"/*.vert "$dir"/*.frag | sed 's|//.*||' | grep -nwE "($reserved)" || true )
if [ -z "$hits" ]; then
	pass "no GLSL 4.10 reserved word (nor MSVC's far/near) used as an identifier"
else
	fail "a GLSL 4.10 reserved word appears in a shader:"
	printf '%s\n' "$hits" | sed 's/^/      /'
fi
rm -rf "$dir"

# The Windows traps the addendum lists, in the C++: M_PI (MSVC has none) and
# far/near as identifiers (windef.h macros).
if grep -nwE 'M_PI|far|near' source/*.cpp source/*.h | grep -v '^\S*:[0-9]*:\s*//' | grep -vE '//.*\b(far|near)\b' >/dev/null; then
	fail "M_PI, far or near in source/ -- MSVC has no M_PI, and windef.h makes far and near macros:"
	grep -nwE 'M_PI|far|near' source/*.cpp source/*.h | grep -vE '//.*\b(far|near)\b' | sed 's/^/      /'
else
	pass "no M_PI, far or near in source/ (MSVC)"
fi

step "offline (no GL)"
if out=$("$SNTEST" --offline 2>&1); then
	pass "sntest --offline: $( printf '%s\n' "$out" | tail -1 )"
else
	fail "sntest --offline"
	printf '%s\n' "$out" | sed 's/^/      /'
fi

GLCHECKS="islands shortest width overspray layers stability resize negative"
for size in 320x180 1280x720; do
	step "physics at $size"
	for check in $GLCHECKS; do
		if out=$("$SNTEST" --$check --size $size 2>&1); then
			pass "sntest --$check: $( printf '%s\n' "$out" | grep -v '^$' | tail -1 )"
		else
			fail "sntest --$check at $size"
			printf '%s\n' "$out" | grep -v '^   ok' | sed 's/^/      /'
		fi
	done
done

# The whole physics list again on Apple's SOFTWARE renderer, which is what
# GitHub's macOS runners have. It is not repeatable at the last bit and may
# round a conversion to half float the other way, so a check that asserts
# exactness on this Mac's GPU is found here before CI finds it.
step "physics at 320x180 on the software renderer (CI's)"
for check in $GLCHECKS; do
	if out=$(SNTEST_RENDERER=software "$SNTEST" --$check --size 320x180 2>&1); then
		pass "sntest --$check (software): $( printf '%s\n' "$out" | grep -v '^$' | tail -1 )"
	else
		fail "sntest --$check at 320x180 on the software renderer -- run: SNTEST_RENDERER=software $SNTEST --$check --size 320x180"
		printf '%s\n' "$out" | grep -v '^   ok' | sed 's/^/      /'
	fi
done

#---------------------------------------------------------------------------
# --pipe, in the fleet's frame format. Two and a half frames in must be exactly
# two frames out and a clean exit -- a partial frame is the end of the stream,
# never a frame -- and a cue naming no parameter must be refused rather than
# silently doing nothing to the picture.
#---------------------------------------------------------------------------
step "pipe"
W=96; H=54
frame=$(( W * H * 4 ))
raw=$( mktemp ); cues=$( mktemp ); out=$( mktemp )
head -c $(( frame * 5 / 2 )) /dev/zero > "$raw"
got=$( "$SNTEST" --pipe --size ${W}x${H} < "$raw" 2>/dev/null | wc -c | tr -d ' ' )
status=${PIPESTATUS[0]}
if [ "$status" -eq 0 ] && [ "$got" = "$(( frame * 2 ))" ]; then
	pass "2.5 frames in, exactly 2 frames out, clean exit"
else
	fail "2.5 frames in gave $got bytes out (want $(( frame * 2 ))), exit $status"
fi
# Read from a file, not a pipe: a writer killed by SIGPIPE would fail the
# pipeline whatever sntest did, and the refusal would pass for the wrong reason.
printf '0 No Such Control 0.5\n' > "$cues"
"$SNTEST" --pipe --size ${W}x${H} --script "$cues" < "$raw" >/dev/null 2>&1
status=$?
if [ "$status" -eq 2 ]; then
	pass "a cue naming no parameter is refused (exit 2)"
else
	fail "a cue naming no parameter gave exit $status, not 2"
fi
# A reader that hangs up early (`| head -c 1`, ffmpeg dying) must end the run
# with exit 1 and a message, not SIGPIPE's silent 141. This is bash, so
# PIPESTATUS[0] is sntest's own status.
head -c $(( frame * 20 )) /dev/zero > "$raw"
"$SNTEST" --pipe --size ${W}x${H} < "$raw" 2>/dev/null | head -c 1 >/dev/null
status=${PIPESTATUS[0]}
if [ "$status" -eq 1 ]; then
	pass "a closed stdout (| head -c 1) ends the run with exit 1, not SIGPIPE's 141"
else
	fail "a closed stdout gave exit $status, not 1"
fi
# A failed render ends the run with exit 1 too.
"$SNTEST" --pipe --size ${W}x${H} --fail-render-at 3 < "$raw" >/dev/null 2>&1
status=$?
if [ "$status" -eq 1 ]; then
	pass "a failed render ends the run with exit 1"
else
	fail "a failed render gave exit $status, not 1"
fi
# Options, booleans and integers STEP between cues; sliders RAMP. The picture
# is a still, so each frame of a scripted run is compared with the same frame
# of a run that held one value throughout. Cue A at frame 0 and B at frame 4:
# frames 0-3 must be the A run's, 4-5 the B run's, and A and B must differ on
# frame 2 (else the comparison proves nothing). A ramp would land between them
# on frame 2 and match neither.
python3 - "$raw" $W $H <<'PY'
import sys
w, h = int(sys.argv[2]), int(sys.argv[3])
out = bytearray()
for f in range(6):
    for y in range(h):
        for x in range(w):
            # rings and a ramp: islands to bridge and tones to cut
            cx, cy = x - w / 2, y - h / 2
            r = (cx * cx + cy * cy) ** 0.5
            v = 30 if (8 < r < 14) or (18 < r < 23) else int(40 + 200 * x / w)
            out += bytes([v, v, v, 255])
open(sys.argv[1], 'wb').write(out)
PY
steps() {  # steps NAME A B : scripted A->B at frame 4 against constant A and constant B
	printf '0 %s %s\n4 %s %s\n' "$1" "$2" "$1" "$3" > "$cues"
	"$SNTEST" --pipe --size ${W}x${H} --script "$cues" < "$raw" > "$out" 2>/dev/null
	"$SNTEST" --pipe --size ${W}x${H} --set "$1=$2" < "$raw" > "$out.a" 2>/dev/null
	"$SNTEST" --pipe --size ${W}x${H} --set "$1=$3" < "$raw" > "$out.b" 2>/dev/null
	python3 - "$out" "$out.a" "$out.b" $frame <<'PY'
import sys
s, a, b = (open(p, 'rb').read() for p in sys.argv[1:4]); n = int(sys.argv[4])
f = lambda d, i: d[i * n:(i + 1) * n]
ok = len(s) == 6 * n and all(f(s, i) == f(a, i) for i in range(4)) and all(f(s, i) == f(b, i) for i in (4, 5)) and f(a, 2) != f(b, 2)
sys.exit(0 if ok else 1)
PY
}
if steps Wall 1 3; then
	pass "an option cue steps (Wall: frames 0-3 Brick, 4-5 Plain), no ramp between"
else
	fail "an option cue did not step -- see loadScript/valueAt in tools/sntest/main.cpp"
fi
if steps Invert 0 1; then
	pass "a boolean cue steps (Invert: frames 0-3 off, 4-5 on)"
else
	fail "a boolean cue did not step"
fi
if steps Layers 1 4; then
	pass "an integer cue steps (Layers: frames 0-3 one, 4-5 four; never two or three)"
else
	fail "an integer cue did not step"
fi
# A slider RAMPS: Mix 0 at frame 0 and 1 at frame 4 puts 0.5 on frame 2.
printf '0 Mix 0\n4 Mix 1\n' > "$cues"
"$SNTEST" --pipe --size ${W}x${H} --script "$cues" < "$raw" > "$out" 2>/dev/null
"$SNTEST" --pipe --size ${W}x${H} --set "Mix=0.5" < "$raw" > "$out.a" 2>/dev/null
if python3 - "$out" "$out.a" $frame <<'PY'
import sys
s, a = (open(p, 'rb').read() for p in sys.argv[1:3]); n = int(sys.argv[3])
sys.exit(0 if len(s) == 6 * n and s[2 * n:3 * n] == a[2 * n:3 * n] else 1)
PY
then
	pass "a slider cue ramps (Mix 0 -> 1 over frames 0-4 is 0.5 on frame 2)"
else
	fail "a slider cue did not ramp"
fi
rm -f "$out.a" "$out.b"
rm -f "$raw" "$cues" "$out"

step "sweep"
if out=$(python3 tools/sweep.py --binary "$SNTEST" 2>/dev/null); then
	pass "$( printf '%s\n' "$out" | tail -1 )"
else
	fail "tools/sweep.py reports a dead control"
	printf '%s\n' "$out" | grep -E '^DEAD|DEAD CONTROLS' | sed 's/^/      /'
fi

step "bench (for the record)"
"$SNTEST" --bench --frames 30 2>&1 | sed -n '3,6p' | sed 's/^/   /'

BUNDLE="$BUILD/Stencil.bundle"
BIN="$BUNDLE/Contents/MacOS/Stencil"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "registration"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under `set -o pipefail`:
	# grep exits at once, nm takes SIGPIPE, and the pipeline reports failure.
	# Capture and match instead of piping.
	syms=$(nm -gU "$BIN" 2>/dev/null)
	case "$syms" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle contains no plugin" ;;
	esac

	step "lipo"
	archs=$(lipo -archs "$BIN" 2>/dev/null)
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs) -- a universal build was asked for" ;; esac

	step "plist"
	exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	ident=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	version=$(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
	fi
	if [ "$ident" = "com.stoatworks.ffgl.stencil" ]; then
		pass "CFBundleIdentifier is $ident"
	else
		fail "CFBundleIdentifier is '$ident'"
	fi
	about=$(sed -n 's/.*versionFallback = "v\([^"]*\)".*/\1/p' source/StoatworksAbout.h)
	if [ "$version" = "0.1.0" ] && [ "$about" = "0.1.0" ]; then
		pass "version 0.1.0 in the plist (from CMakeLists) and in StoatworksAbout.h"
	else
		fail "version: plist '$version', StoatworksAbout.h '$about' -- both must be 0.1.0"
	fi

	step "codesign"
	tmp=$(mktemp -d)
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Stencil.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "ad-hoc signing failed"
	fi
	rm -rf "$tmp"

	step "oxbow"
	OXBOW="${OXBOW:-$HOME/Projects/resolume/oxbow/build/oxbow}"
	if [ -x "$OXBOW" ]; then
		probe=$("$OXBOW" probe "$BUNDLE" 2>&1)
		for want in "name:        SW Stencil" "id:          SN01" "type:        effect"; do
			case "$probe" in
				*"$want"*) pass "host sees '$want'" ;;
				*) fail "host does not see '$want' -- see: $OXBOW probe $BUNDLE" ;;
			esac
		done
		self=$("$OXBOW" selftest "$BUNDLE" 2>&1)
		case "$self" in
			*"selftest:    PASS"*) pass "instantiates through plugMain and renders 120 frames" ;;
			*) fail "oxbow selftest did not pass -- see: $OXBOW selftest $BUNDLE" ;;
		esac
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ? 1 : 0 ))
