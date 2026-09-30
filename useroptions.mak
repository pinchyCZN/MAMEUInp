###########################################################################
#
#   useroptions.mak
#
#   Build configuration for this tree. MAME's makefile does
#   "-include useroptions.mak" (see the USER-CONFIGURABLE OPTIONS block)
#   before it applies any defaults, so everything set here behaves exactly
#   as if it had been typed on the make command line.
#
#   THIS FILE IS THE POINT. Without it the options live only in somebody's
#   shell history, and a fresh clone running "make vs2017" silently gets a
#   different build. With it:
#
#       make vs2017
#
#   is the whole command, and it reproduces the configuration described in
#   notes/build-config.txt.
#
###########################################################################

# --- what we are building -------------------------------------------------
OSD        = winui
TARGET     = mame
SUBTARGET  = coinop
PTR64      = 1

# --- required to build at all with MSVC -----------------------------------

# genie.lua puts a raw "/WX" in buildoptions for vs20*. Upstream left it
# unguarded so NOWERROR did nothing for MSVC; we guarded it, and this is
# what switches it off. Without it, upstream code fails on warnings in a
# Debug build - C4701 in i386.cpp/athlon.cpp, C4390 in starrider.cpp.
# See notes/build-config.txt 3.2.
NOWERROR   = 1

# scripts/build/msgfmt.py calls array.array.tostring(), removed in Python
# 3.9. Every translation conversion dies without this. It is upstream's own
# escape hatch: the recipe becomes "... || exit 0". Cost is an English-only
# UI. See notes/build-config.txt 3.5.
IGNORE_BAD_LOCALISATION = 1

# --- keep build output out of the repo root -------------------------------

# Without this, scripts/src/main.lua does targetdir(MAME_DIR) and drops the
# exe, pdb, ilk, exp and lib in the root - 4.9 GB for a Debug build. With
# it they go to build/vs2017/bin/<platform>/<config>/.
SEPARATE_BIN = 1

# --- deliberately NOT set here --------------------------------------------
#
# DEBUG_DIR   defaults to the MAME root, which is what we want. MAME's data
#             paths (roms, hash, artwork, plugins, language, bgfx, ctrlr,
#             cheat, samples) are all bare CWD-relative names pointing at
#             repo content, so the working directory has to BE the repo
#             root. Writable output is redirected instead, in mame.ini.
#
# DEBUG_ARGS  genie defaults it to "-window". Note that ANY argument makes
#             MAMEUI hand off to plain MAME - see notes/build-config.txt -
#             so F5 in Visual Studio gives you stock MAME, not the
#             frontend. Run the exe with no arguments for the frontend, or
#             set DEBUG_ARGS to a game name to launch straight into it.
