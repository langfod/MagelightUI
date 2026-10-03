"""Where do upstream's swapchain/DXGI changes overlap ours?

Does upstream still keep the DXGI surface in src/Magelight.cpp (which we split
into src/host/Hooks.cpp), and do their changes touch the same symbols?

Writes tools/_swap.txt because the terminal swallows stdout.
"""
import io
import re
import subprocess
import traceback

OUT = io.open("tools/_swap.txt", "w", encoding="utf-8")


def w(s=""):
    OUT.write(str(s) + "\n")


def git(*a):
    r = subprocess.run(["git"] + list(a), capture_output=True, text=True)
    return r.stdout


BASE = "538ebec"
OURS = "cleanup4"
THEIRS = "v0.31.2"

SWAP_COMMITS = [
    ("42082fb", "0.30.5: survive frame-generation swapchains, say why the renderer died"),
    ("d39dace", "0.30.5: draw late under NVIDIA Smooth Motion, stop on a lost device"),
    ("cf05ad7", "0.30.5: engine mode under NVidia Smooth Motion, no flicker"),
    ("d51d1ab", "Release 0.30.4: UI-pass composite behind NVIDIA Streamline"),
    ("9a8bb08", "Hide the vanilla cursor by its movie root, not the movie"),
    ("3d8019f", "Drawn cursor and a hidden vanilla cursor in the UI pass"),
    ("815a6a2", "0.31.0: per-view cursors, cursor: none honoured on flat"),
    ("a53ff84", "0.31.0: freeze the world behind a paused fullscreen page"),
    ("be0ed5e", "MSAA for page shapes"),
    ("1710ad6", "0.31.0 MSAA review fixes"),
    ("003615c", "0.31.0: stagger first page loads"),
    ("138a631", "0.31.2: toggleKey takes a key name"),
    ("30b80f0", "0.31.1: SetViewCursorTint"),
]

try:
    w("=" * 78)
    w("UPSTREAM COMMITS TOUCHING THE SWAPCHAIN / PRESENT PATH")
    w("=" * 78)
    for sha, subj in SWAP_COMMITS:
        files = [f for f in git("show", "--name-only", "--format=", sha).split()
                 if f.endswith((".cpp", ".h"))]
        w("")
        w("%s  %s" % (sha[:7], subj))
        for f in files:
            note = ""
            if f == "src/Magelight.cpp":
                note = "  <-- WE SPLIT THIS (now src/host/*.cpp)"
            elif f.startswith("src/host/"):
                note = "  <-- this file is OURS"
            elif f.startswith("src/include/"):
                note = "  <-- our layout"
            w("    %-46s%s" % (f, note))

    theirs_ml = git("show", "%s:src/Magelight.cpp" % THEIRS)
    ours_ml = git("show", "%s:src/Magelight.cpp" % OURS)
    base_ml = git("show", "%s:src/Magelight.cpp" % BASE)

    w("")
    w("=" * 78)
    w("DOES UPSTREAM'S Magelight.cpp STILL CONTAIN THE SURFACE WE SPLIT OUT?")
    w("=" * 78)
    w("  Magelight.cpp lines: base %d -> ours %d | theirs %d"
      % (len(base_ml.split("\n")), len(ours_ml.split("\n")),
         len(theirs_ml.split("\n"))))
    w("")
    for sym in ("HookPresent", "HookPresent1", "LatePresent", "LatePresent1",
                "ComposeOnce", "EndPresent", "InstallLatePresent",
                "GuardedFrameWork", "FrameWork", "HideOverlayCarrier",
                "UiCompositeWanted", "LoadHostSettings", "SetViewCursorTint",
                "msaa", "Freeze"):
        w("  %-24s ours:%-6s theirs:%s"
          % (sym, sym in ours_ml, sym in theirs_ml))

    def tree(rev):
        return [f for f in git("ls-tree", "-r", "--name-only", rev).split()
                if f.endswith((".cpp", ".h"))]

    def defines(rev, sym):
        pat = re.compile(r"^[^\S\r\n]*(?:static\s+|constexpr\s+|inline\s+)*"
                         r"[\w:<>,\s\*&]*?\b%s\b[^\S\r\n]*\(" % re.escape(sym), re.M)
        return [f for f in tree(rev) if pat.search(git("show", "%s:%s" % (rev, f)))]

    w("")
    w("=" * 78)
    w("WHERE EACH DXGI SYMBOL IS DEFINED (per side)")
    w("=" * 78)
    for sym in ("HookPresent", "HookPresent1", "LatePresent", "LatePresent1",
                "ComposeOnce", "EndPresent", "AfterCompose", "StampPresent",
                "OnOurDevice", "InstallLatePresent", "ModuleOf", "NotePresenter",
                "LoadHostSettings"):
        a = defines(OURS, sym)
        b = defines(THEIRS, sym)
        w("  %-20s ours: %-40s theirs: %s"
          % (sym, ",".join(a) or "-", ",".join(b) or "-"))

    w("")
    w("=" * 78)
    w("UPSTREAM'S NEW FILES vs OUR LAYOUT")
    w("=" * 78)
    for f in ("src/Magelight.h", "src/MagelightCursorArt.h", "src/MagelightApi4.h",
              "src/HotkeyNames.h", "api/MagelightUI_API.h", "gpu/MagelightGpuApi.h",
              "src/MagelightApi4/Registry.h", "src/MagelightVR/State.h",
              "src/MagelightApi4.cpp"):
        in_ours = subprocess.run(["git", "cat-file", "-e", "%s:%s" % (OURS, f)],
                                 capture_output=True).returncode == 0
        note = ""
        if not in_ours and f.startswith("src/") and not f.startswith("src/include/"):
            alt = "src/include/" + f[4:]
            if subprocess.run(["git", "cat-file", "-e", "%s:%s" % (OURS, alt)],
                              capture_output=True).returncode == 0:
                note = "  -> ours has %s instead" % alt
        w("  %-32s ours:%-6s%s" % (f, in_ours, note))
except Exception:
    w("")
    w("EXCEPTION:")
    w(traceback.format_exc())
finally:
    OUT.close()
