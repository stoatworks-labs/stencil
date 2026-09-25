"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means the copies have drifted: run
`python3 demo/tools/sync_shaders.py`, never a hand edit of the page. Jacquard's
check, in this repo's shape.

------------------------------------------------------------------- why

`demo/plugin.js` holds the ten GLSL bodies of `source/Shaders.cpp`, and
`demo/controls.js` holds the constants of Controls.h, Stencil.h, Shaders.h and
Palette.h, the five palettes and the option names. Two copies drift -- quietly,
because a demo that renders a *plausible* picture looks exactly like a demo that
renders the right one. The whole claim of the page is that it runs the plugin's
own shaders, so the claim needs something enforcing it. Nothing else can:
`sntest` drives the real plugin class and has no idea this page exists, and
verify.sh's glslc step compiles the C++ copies and never looks at the JS ones.

------------------------------------------------------------------- what it does

1. Pulls each `R"( ... )"` body out of the C++ and each matching backtick
   literal out of `plugin.js`, and compares them exactly -- no whitespace
   normalisation, no comment stripping. The plugin assembles each shader as
   `kVersion + body` (`shaders::assemble`); the page does the same with
   `assemble()`, so the bodies are compared and the one prepended line is
   checked as a literal. The only transformation is the decode of the three
   escapes a template literal needs (\\\\, \\` and \\${); any other backslash on
   the JS side is refused, since it could only be somebody hiding a difference.
   This half shares no code with sync_shaders.py.
2. Regenerates both generated blocks (`sync_shaders.py`'s shaders and
   constants) from `source/` and requires each block in the page to be exactly
   that text, so a constant of Controls.h or Stencil.h, a can of a palette or an
   option name that moved in the C++ fails here too.

------------------------------------------------------------------- what it cannot

Nothing here checks the *ported* half. The cutter (cutter.js, from Bridge.cpp),
the conversions, the inks and the footprint (controls.js, from Controls.cpp,
Palette.cpp and Spray.cpp) and the frame sequence in plugin.js
(Stencil::ProcessOpenGL) are hand translations, and only a reader can tell
whether they still agree. When you change one of those, change it there too --
a wrong port shows up on the page as a stencil that is subtly wrong, which
nobody will notice.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True  # no __pycache__ beside the page

import sync_shaders  # noqa: E402

# JS constant, C++ symbol in source/Shaders.cpp.
SHADERS = [
    ("VERTEX_BODY", "kVertexBody"),
    ("HASH_BODY", "kHashBody"),
    ("DETECT_BODY", "kDetectBody"),
    ("BLUR_BODY", "kBlurBody"),
    ("SEED_BODY", "kSeedBody"),
    ("FLOOD_BODY", "kFloodBody"),
    ("SECOND_BODY", "kSecondBody"),
    ("SPRAY_BODY", "kSprayBody"),
    ("SETTLE_BODY", "kSettleBody"),
    ("COMPOSITE_BODY", "kCompositeBody"),
]

# The line the plugin prepends to every body, and the page must too.
VERSION_CPP = 'const char* const kVersion = "#version 410 core\\n";'
VERSION_JS = "const VERSION = '#version 410 core\\n';"


def read(path):
    with open(os.path.join(REPO, path)) as handle:
        return handle.read()


def from_cpp(source, symbol):
    match = re.search(r'const char\* const ' + symbol + r' = R"\((.*?)\)";', source, re.S)
    return None if match is None else match.group(1)


def from_js(source, name):
    match = re.search(r'^const ' + name + r' = `(.*?)(?<!\\)`;$', source, re.S | re.M)
    if match is None:
        return None, None
    body = match.group(1)
    stray = re.search(r"\\(?![\\`]|\$\{)", body)
    if stray is not None:
        upto = body[: stray.start()]
        return None, f"backslash that is not one of the three template escapes, at line {upto.count(chr(10)) + 1}"
    decoded = re.sub(r"\\([\\`]|\$\{)", lambda m: m.group(1), body)
    return decoded, None


def main():
    js = read("demo/plugin.js")
    cpp_all = read("source/Shaders.cpp")

    problems = 0
    if VERSION_CPP not in cpp_all or VERSION_JS not in js:
        print("FAIL  the #version line the plugin prepends is not the one the page prepends")
        problems += 1
    else:
        print("ok    VERSION              matches kVersion")

    for name, symbol in SHADERS:
        cpp_text = from_cpp(cpp_all, symbol)
        js_text, complaint = from_js(js, name)
        if cpp_text is None:
            print(f"FAIL  {symbol} not found in source/Shaders.cpp")
            problems += 1
            continue
        if complaint is not None:
            print(f"FAIL  {name} in demo/plugin.js has a {complaint}")
            problems += 1
            continue
        if js_text is None:
            print(f"FAIL  {name} not found in demo/plugin.js")
            problems += 1
            continue
        if cpp_text == js_text:
            print(f"ok    {name:<20} matches {symbol} ({len(cpp_text)} chars)")
            continue
        problems += 1
        print(f"FAIL  {name} has drifted from {symbol} in source/Shaders.cpp")
        cpp_lines = cpp_text.splitlines()
        js_lines = js_text.splitlines()
        for i in range(max(len(cpp_lines), len(js_lines))):
            a = cpp_lines[i] if i < len(cpp_lines) else "<missing>"
            b = js_lines[i] if i < len(js_lines) else "<missing>"
            if a != b:
                print(f"        first difference at line {i + 1}")
                print(f"          C++: {a}")
                print(f"          js : {b}")
                break

    for block, path in sync_shaders.TARGETS.items():
        text = read(path)
        where = sync_shaders.region(text, block)
        if where is None:
            print(f"FAIL  {path} has no ({block}) generated block")
            problems += 1
            continue
        want = sync_shaders.BLOCKS[block]()
        got = text[where[0] : where[1]]
        if got == want:
            print(f"ok    ({block}) block in {path} is what source/ generates ({len(want.splitlines())} lines)")
        else:
            problems += 1
            print(f"FAIL  the ({block}) block in {path} is not what source/ generates")
            for i, (a, b) in enumerate(zip(want.splitlines() + ["<end>"], got.splitlines() + ["<end>"])):
                if a != b:
                    print(f"        first difference at line {i + 1}")
                    print(f"          source/: {a}")
                    print(f"          page   : {b}")
                    break

    print()
    if problems:
        print(f"{problems} difference(s) -- run python3 demo/tools/sync_shaders.py, do not edit the page by hand")
        return 1
    print(f"all {len(SHADERS)} shaders and both generated blocks are identical to the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main())
