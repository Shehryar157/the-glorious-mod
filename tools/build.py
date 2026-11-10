"""Build and install FoG2Access.

Two DLLs, both 32-bit like the game:
  version.dll     the loader (src/loader): the launcher and the game both load it at startup;
                  it loads the mod and reloads it whenever FoG2Access.dll changes.
  FoG2Access.dll  the mod (src/core).

  python tools/build.py            build the mod and copy it into the game folder; a running
                                   launcher or game reloads it within a quarter of a second
  python tools/build.py install    also build and install the loader and the NVDA client
                                   (launcher and game must be closed: Windows locks a loaded DLL)
  python tools/build.py uninstall  remove our files from the game folder
  python tools/build.py installer  build TheGloriousMod.exe (src/installer), the end user's installer
  python tools/build.py release 0.2.0
                                   build everything and publish GitHub release fog2-v0.2.0: only
                                   TheGloriousMod.exe, with the mod files (version.dll, FoG2Access.dll,
                                   the NVDA client) inside it
"""
import glob
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ZIG = r"E:\code\mods\tools\zig\zig.exe"
GAME = r"D:\steam\steamapps\common\Field of Glory II"
BUILD = os.path.join(ROOT, "build")
LOADER = os.path.join(BUILD, "version.dll")
MOD = os.path.join(BUILD, "FoG2Access.dll")
MARKER = "FoG2Access.txt"
OUR_FILES = ["version.dll", "FoG2Access.dll", "nvdaControllerClient32.dll", MARKER, "FoG2Access.log", "FoG2Access.log.old", "FoG2Access_loader.log"]


def compile_dll(out, folder, extra):
    os.makedirs(BUILD, exist_ok=True)
    sources = sorted(glob.glob(os.path.join(ROOT, "src", folder, "*.cpp")))
    subprocess.run([ZIG, "c++", "-target", "x86-windows-gnu", "-shared", "-O2", "-std=c++17",
                    "-o", out, *sources, *extra, "-luser32"], check=True)
    print(f"built {out}")


def build_mod():
    compile_dll(MOD, "core", [])


def deploy_mod():
    build_mod()
    if not os.path.exists(os.path.join(GAME, MARKER)):
        print("loader not installed yet: run  python tools/build.py install  with the launcher and game closed")
        return
    # Copy then rename, so the loader never sees a half-written file.
    tmp = os.path.join(GAME, "FoG2Access.dll.tmp")
    shutil.copyfile(MOD, tmp)
    os.replace(tmp, os.path.join(GAME, "FoG2Access.dll"))
    print("deployed; a running launcher or game reloads it now")


def install():
    compile_dll(LOADER, "loader", [os.path.join(ROOT, "src", "loader", "version.def")])
    target = os.path.join(GAME, "version.dll")
    marker = os.path.join(GAME, MARKER)
    # Never overwrite somebody else's version.dll (e.g. another mod's).
    if os.path.exists(target) and not os.path.exists(marker):
        sys.exit(f"{target} exists and isn't ours; not overwriting it")
    shutil.copyfile(LOADER, target)
    shutil.copyfile(os.path.join(ROOT, "native", "nvdaControllerClient32.dll"),
                    os.path.join(GAME, "nvdaControllerClient32.dll"))
    with open(marker, "w", encoding="utf-8") as f:
        f.write("FoG2Access installed this version.dll\n")
    print(f"installed the loader into {GAME}")
    deploy_mod()


INSTALLER = os.path.join(BUILD, "TheGloriousMod.exe")
GH = r"E:\code\tools\gh\bin\gh.exe"
REPO = "Shehryar157/the-glorious-mod"


def build_installer():
    # A plain Win32 program (list box, buttons, progress bar): WinHTTP for the download, comctl32 for the
    # progress bar, the shell's folder picker (ole32/uuid). --subsystem windows: no console window.
    os.makedirs(BUILD, exist_ok=True)
    subprocess.run([ZIG, "c++", "-target", "x86-windows-gnu", "-O2", "-std=c++17", "-o", INSTALLER,
                    os.path.join(ROOT, "src", "installer", "installer.cpp"), "-lwinhttp", "-lcomctl32", "-lole32",
                    "-luuid", "-lshell32", "-ladvapi32", "-luser32", "-lgdi32", "-Wl,--subsystem,windows"],
                   check=True)
    print(f"built {INSTALLER}")


def add_payload(tag):
    # The mod files go inside the installer (installer.cpp readPayload): after the program, entries
    # {uint32 name length, name, uint32 size, data}, then {uint32 offset of the first entry, "GLORIOUSMOD1"}.
    # Windows ignores data after a program's last section, so the exe still runs.
    import struct
    files = {"fog2/version.dll": open(LOADER, "rb").read(),
             "fog2/FoG2Access.dll": open(MOD, "rb").read(),
             "fog2/nvdaControllerClient32.dll": open(os.path.join(ROOT, "native", "nvdaControllerClient32.dll"), "rb").read(),
             "fog2/version.txt": tag.encode()}
    exe = open(INSTALLER, "rb").read()
    out = bytearray(exe)
    start = len(out)
    for name, data in files.items():
        out += struct.pack("<I", len(name)) + name.encode() + struct.pack("<I", len(data)) + data
    out += struct.pack("<I", start) + b"GLORIOUSMOD1"
    open(INSTALLER, "wb").write(out)
    print(f"put {len(files)} files ({tag}) inside {INSTALLER}")


END_OF_CHANGES = "<!-- end of changes -->"  # installer.cpp END_OF_CHANGES: the Changes box stops here


def changelog_section(tag):
    """The lines under '## <tag>' in CHANGELOG.md, up to the next '## ' heading; '' if there's none."""
    path = os.path.join(ROOT, "CHANGELOG.md")
    if not os.path.exists(path):
        return ""
    out, inside = [], False
    for line in open(path, encoding="utf-8").read().splitlines():
        if line.startswith("## "):
            inside = line[3:].strip() == tag
            continue
        if inside:
            out.append(line)
    return "\n".join(out).strip()


def release(version):
    compile_dll(LOADER, "loader", [os.path.join(ROOT, "src", "loader", "version.def")])
    build_mod()
    build_installer()
    tag = f"fog2-v{version}"
    changes = changelog_section(tag)
    if not changes:
        sys.exit(f"CHANGELOG.md has no '## {tag}' section: write what changed first (the installer shows it)")
    add_payload(tag)
    # One download for players (user, 2026-11-09): the release holds only the installer. The notes start
    # with the CHANGELOG section, which the installer's Changes box shows up to END_OF_CHANGES (user,
    # 2026-11-11); the install line after it is for people reading the release page.
    notes = (changes + "\n\n" + END_OF_CHANGES + "\n\n"
             "Download TheGloriousMod.exe and run it: choose Field of Glory II, then Install. "
             "Run it again later to update, reinstall or uninstall.")
    subprocess.run([GH, "release", "create", tag, INSTALLER, "--repo", REPO,
                    "--title", f"Field of Glory II {version}", "--notes", notes], check=True)
    print(f"published {tag}")


def uninstall():
    live = [os.path.basename(p) for p in glob.glob(os.path.join(GAME, "FoG2Access.*.live.dll"))]
    for name in OUR_FILES + live:
        path = os.path.join(GAME, name)
        if os.path.exists(path):
            os.remove(path)
            print(f"removed {path}")


if __name__ == "__main__":
    action = sys.argv[1] if len(sys.argv) > 1 else "deploy"
    if action == "release":
        release(sys.argv[2])
    else:
        {"deploy": deploy_mod, "build": build_mod, "install": install, "uninstall": uninstall,
         "installer": build_installer}[action]()
