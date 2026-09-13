#!/usr/bin/env python3
"""Scons entry point for the Godot 3.6 renderer.

`godot3/` is pristine upstream Godot, so the_gates is built into it as an
out-of-tree custom module rather than being committed to the submodule.
The Godot 4 launcher and renderer keep using godot/tools/build.py.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

MODULES_DIR = Path(__file__).resolve().parent
REPO_ROOT = MODULES_DIR.parent
GODOT3_DIR = REPO_ROOT / "godot3"

GODOT_VERSION = "3.6"

PROFILES: dict[str, list[str]] = {
    "renderer3": [
        "target=release_debug",
        "tools=no",
        "debug_symbols=yes",
        # libzmq needs exceptions; Godot disables them by default.
        "disable_exceptions=no",
    ],
    "renderer3-release": [
        "target=release",
        "tools=no",
        "lto=full",
        "disable_exceptions=no",
    ],
}

# Nothing a gate renders needs these, and dropping them takes a few minutes off
# a cold build. Add back with `-- module_<name>_enabled=yes` if a gate needs one.
DISABLED_MODULES = [
    "module_mono_enabled=no",
    "module_webm_enabled=no",
    "module_mobile_vr_enabled=no",
]


def default_jobs() -> int:
    cpu = os.cpu_count() or 4
    return max(1, cpu - 2)


# The file name the launcher's renderer_executable.tres looks up per platform.
STAGED_NAMES = {
    "x11": "Renderer-godot_v%s.x86_64" % GODOT_VERSION,
    "osx": "Renderer-godot_v%s.universal" % GODOT_VERSION,
}


def default_platform() -> str:
    return "osx" if sys.platform == "darwin" else "x11"


def built_binary(profile: str, platform: str) -> Path | None:
    prefix = "godot.%s.%s." % (platform, "opt.debug" if profile == "renderer3" else "opt")
    # The arch suffix differs per platform: .64 on x11, .arm64 or .x86_64 on osx.
    binaries = [p for p in (GODOT3_DIR / "bin").glob(prefix + "*") if not p.name[len(prefix) :].startswith("debug")]
    return max(binaries, key=lambda p: p.stat().st_mtime, default=None)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("profile", choices=sorted(PROFILES), nargs="?", default="renderer3")
    parser.add_argument("-j", "--jobs", type=int, default=default_jobs())
    parser.add_argument("--platform", choices=sorted(STAGED_NAMES), default=default_platform())
    parser.add_argument("--dry-run", action="store_true", help="print the scons command and exit")
    parser.add_argument("--stage-to", type=Path, help="copy the built binary here under the launcher's renderer name")
    parser.add_argument("extra", nargs="*", help="extra scons args (after --)")
    args = parser.parse_args()

    if not (GODOT3_DIR / "SConstruct").is_file():
        print("godot3/ is empty. Run: git submodule update --init godot3", file=sys.stderr)
        return 1
    if not (REPO_ROOT / "godot" / "thirdparty" / "libzmq").is_dir():
        print("godot/thirdparty/libzmq is missing. Run: git submodule update --init godot", file=sys.stderr)
        return 1

    cmd = ["scons", "-j", str(args.jobs), "platform=%s" % args.platform]
    cmd += PROFILES[args.profile]
    cmd += DISABLED_MODULES
    cmd.append("custom_modules=%s" % MODULES_DIR)
    cmd += args.extra

    print("+ cd %s && %s" % (GODOT3_DIR, " ".join(cmd)))
    if args.dry_run:
        return 0

    result = subprocess.run(cmd, cwd=GODOT3_DIR)
    if result.returncode != 0:
        return result.returncode

    binary = built_binary(args.profile, args.platform)
    if binary is None:
        print("build reported success but no %s binary is in %s" % (args.platform, GODOT3_DIR / "bin"), file=sys.stderr)
        return 1
    print("built %s" % binary)

    if args.stage_to:
        args.stage_to.mkdir(parents=True, exist_ok=True)
        target = args.stage_to / STAGED_NAMES[args.platform]
        shutil.copy2(binary, target)
        os.chmod(target, 0o755)
        print("staged %s" % target)

    return 0


if __name__ == "__main__":
    sys.exit(main())
