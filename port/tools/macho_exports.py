#!/usr/bin/env python3
"""Export list of libre4_main for the Mach-O build (stands in for libre4_main.ver).

Every external symbol of the objects, including the inline functions the compiler marks
weak_def_can_be_hidden (the linker would hide those: sibling modules call them), except the
game's operator new / delete, which must stay local to libre4_main (see runtime/game_heap.cpp)
or they take over the C++ runtime's operators in every other image (weak definitions coalesce
onto the strong one).
usage: macho_exports.py OUT OBJECT...   (or @responsefile)
"""
import subprocess
import sys

LOCAL = {"__Znwm", "__Znam", "__ZdlPv", "__ZdaPv", "__Znwj", "__Znaj"}


def main() -> int:
    out, *objects = sys.argv[1:]
    if len(objects) == 1 and objects[0].startswith("@"):
        objects = open(objects[0][1:]).read().split()
    names = set()
    for i in range(0, len(objects), 200):
        text = subprocess.run(["nm", "-m", *objects[i:i + 200]], capture_output=True, text=True, check=True).stdout
        for line in text.splitlines():
            # "0000... (__TEXT,__text) weak external automatically hidden __Z..."
            parts = line.split()
            if len(parts) < 4 or "undefined" in line or "external" not in line or "non-external" in line:
                continue
            if "private external" in line:
                continue
            names.add(parts[-1])
    names -= LOCAL
    with open(out, "w") as f:
        f.write("\n".join(sorted(names)) + "\n")
    print(f"macho_exports: {len(names)} symbols")
    return 0


if __name__ == "__main__":
    sys.exit(main())
