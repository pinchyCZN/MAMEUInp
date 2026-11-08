#!/usr/bin/env python3
# license:BSD-3-Clause
# copyright-holders:MAMEUI Netplay
#
# The netplay front end for builds without MAMEUI (OSD=sdl on Linux): the
# lobby, game picker, savestate picker, media picker and direct connect of
# src/osd/winui/netplayui.cpp, netplaypick.cpp and netplaystate.cpp.
# Behaviour and its reasons are notes/netplay-ui.txt; this file follows it.
#
#     python3 tools/netplay_lobby.py [--mame PATH]
#     python3 tools/netplay_lobby.py --selftest
#
# RUN IT FROM THE DIRECTORY MAME SHOULD RUN IN. netplay.ini, netplaypick.ini,
# mame.ini, rompath and everything MAME writes resolve against the working
# directory, exactly as they do beside MAMEUI.exe.
#
# The emulator is a CHILD PROCESS here, not in-process as under MAMEUI, so
# the NETPLAY_* variables go into the child's environment only and need no
# clearing afterwards (contrast NetplayClear).
#
# The MAME binary: --mame, else $NETPLAY_MAME, else the SDL build in this
# tree, else netplay64 / mame on PATH.

import argparse
import hashlib
import json
import os
import select
import shutil
import socket
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

UI_INI = "netplay.ini"
PICK_INI = "netplaypick.ini"
LOG = "netplay-last.log"

PLAYERS_HI = 8
DELAY_LO, DELAY_HI = 1, 20
BATCH_LO, BATCH_HI = 1, 6
AUTOSAVE_SECS = (0, 10, 30, 60, 120, 240)
TICK_MS = 100
PING_WAIT_S = 0.6


# --------------------------------------------------------------------------
#  wire helpers - must match netplayui.cpp byte for byte
# --------------------------------------------------------------------------

def hex_of(s):
    # WIRE FORMAT: lower-case, two digits per byte, no prefix.
    return s.encode("latin-1").hex()


def unhex(s):
    # Empty on anything malformed: "no media", never a partial name.
    try:
        return bytes.fromhex(s).decode("latin-1") if len(s) % 2 == 0 else ""
    except ValueError:
        return ""


def kvs(line, key):
    # A key only matches at the start of a word, so "hash=" is not found
    # inside "mediahash=".
    for word in line.split(" "):
        if word.startswith(key + "="):
            return word[len(key) + 1:]
    return ""


def kv(line, key, fallback):
    v = kvs(line, key)
    digits = ""
    for c in v:
        if not c.isdigit():
            break
        digits += c
    return int(digits) if digits else (0 if v else fallback)


def media_of(line):
    # Found by the "media." prefix, not by knowing the slot name.
    for word in line.split(" "):
        if word.startswith("media.") and "=" in word:
            return unhex(word.split("=", 1)[1])
    return ""


def name_token(raw):
    # THE NAME IS ONE WHITESPACE-DELIMITED TOKEN; the protocol splits on
    # spaces. Same reduction as netplayui.cpp name_read, same 31-byte cap.
    parts = raw.split()
    return parts[0][:31] if parts else "player"


def csv_names(s):
    return [n for n in s.split(",") if n]


def media_hash_of(path):
    # Matches emu_file::hashes("S").attribute_string(): the whole file.
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return 'sha1="%s"' % h.hexdigest()


# --------------------------------------------------------------------------
#  netplay.ini - the same file and keys MAMEUI writes
# --------------------------------------------------------------------------

def cfg_defaults():
    return dict(host="127.0.0.1", host2="", name="player", port=7000,
                players=2, slot=1, delay=8, batch=1, savestates=True,
                sendconfig=True, netstats=False, cart="", debug=False)


def cfg_load(path=UI_INI):
    c = cfg_defaults()
    try:
        with open(path, "r", encoding="latin-1") as f:
            for line in f:
                p = line.split()
                if len(p) < 2:
                    continue
                k, v = p[0], p[1]
                if k in ("host", "host2", "name"):
                    c[k] = v
                elif k in ("port", "players", "slot", "delay", "batch"):
                    c[k] = kv(k + "=" + v, k, c[k])
                elif k in ("savestates", "sendconfig", "netstats", "debug"):
                    c[k] = kv(k + "=" + v, k, 0) != 0
                elif k == "cart":
                    c["cart"] = unhex(v)
    except OSError:
        pass
    return c


def cfg_save(c, path=UI_INI):
    out = ["host %s" % c["host"]]
    if c["host2"]:
        out.append("host2 %s" % c["host2"])
    out += ["name %s" % c["name"], "port %u" % c["port"],
            "players %u" % c["players"], "slot %u" % c["slot"],
            "delay %u" % c["delay"], "batch %u" % c["batch"],
            "savestates %d" % c["savestates"],
            "sendconfig %d" % c["sendconfig"],
            "netstats %d" % c["netstats"]]
    if c["cart"]:
        out.append("cart %s" % hex_of(c["cart"]))
    if c["debug"]:
        out.append("debug 1")
    try:
        with open(path, "w", encoding="latin-1") as f:
            f.write("\n".join(out) + "\n")
    except OSError:
        pass


def netplay_env(c, base):
    # The same variables, the same values, as netplayui.cpp NetplayApply.
    env = dict(base)
    env.update(NETPLAY_MODE="udp", NETPLAY_HOST=c["host"],
               NETPLAY_PORT=str(c["port"]), NETPLAY_PLAYERS=str(c["players"]),
               NETPLAY_SLOT=str(c["slot"]), NETPLAY_DELAY=str(c["delay"]),
               NETPLAY_BATCH=str(c["batch"]),
               NETPLAY_SAVESTATES="1" if c["savestates"] else "0",
               NETPLAY_SNAPSHOT_SECS=str(c.get("autosave", 0)),
               NETPLAY_SEND_CONFIG="1" if c["sendconfig"] else "0",
               NETPLAY_STATS="1" if c["netstats"] else "0",
               NETPLAY_REDUNDANCY="99",
               NETPLAY_TAG="player %u" % c["slot"])
    return env


def direct_check(c):
    # netplayui.cpp from_dialog: an error string, or None.
    lo = 1 if c["debug"] else 2
    if not c["host"]:
        return "Enter the server address."
    if not 1 <= c["port"] <= 65535:
        return "Port must be 1 to 65535."
    if not lo <= c["players"] <= 10:
        return "Players must be %d to 10." % lo
    if not 1 <= c["slot"] <= c["players"]:
        return "Your player number must be within the player count."
    if not 1 <= c["delay"] <= 60:
        return "Delay must be 1 to 60 frames."
    if not 1 <= c["batch"] <= 8:
        return "Frames per packet must be 1 to 8."
    # Delay 1 leaves no room for redundancy. protocol.txt 1.7.1
    if c["delay"] < 2 and c["host"] != "127.0.0.1":
        return "Delay 1 has no loss tolerance. Use 2 or more off this machine."
    return None


# --------------------------------------------------------------------------
#  the emulator, asked through its command line
# --------------------------------------------------------------------------

def find_mame(arg):
    here = os.path.dirname(os.path.abspath(__file__))
    tree = os.path.join(here, "..", "build", "linux_gcc", "bin", "x64",
                        "Release")
    # The full build (mame64) before the narrow SOURCES= one (netplay64).
    for c in (arg, os.environ.get("NETPLAY_MAME"),
              os.path.join(tree, "mame64"), os.path.join(tree, "netplay64")):
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return os.path.abspath(c)
    return shutil.which("netplay64") or shutil.which("mame")


class Mame:
    def __init__(self, exe):
        self.exe = exe
        self.opts = {}
        self.games = {}
        out = self.run("-showconfig")[1]
        for line in out.splitlines():
            p = line.split(None, 1)
            if len(p) == 2 and not line.startswith("#"):
                self.opts[p[0]] = p[1].strip()

    def run(self, *args):
        r = subprocess.run([self.exe] + list(args), stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        return r.returncode, r.stdout.decode("utf-8", "replace")

    def load_games(self):
        # -listxml costs seconds on a full build, so the parse is cached
        # against the binary's identity.
        st = os.stat(self.exe)
        key = hashlib.sha1(("%s|%d|%d" % (self.exe, st.st_size,
                                          st.st_mtime_ns)).encode()).hexdigest()
        cdir = os.path.join(os.environ.get("XDG_CACHE_HOME") or
                            os.path.expanduser("~/.cache"), "mameui-netplay")
        cpath = os.path.join(cdir, key + ".json")
        try:
            with open(cpath) as f:
                self.games = json.load(f)
            return
        except (OSError, ValueError):
            pass
        p = subprocess.Popen([self.exe, "-listxml"], stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
        games = {}
        for _, el in ET.iterparse(p.stdout):
            if el.tag != "machine":
                continue
            if el.get("isdevice") != "yes" and el.get("runnable") != "no":
                slot, exts = "", []
                for d in el.findall("device"):
                    inst = d.find("instance")
                    if inst is not None:
                        slot = inst.get("briefname", "")
                        exts = [e.get("name") for e in d.findall("extension")]
                        break
                games[el.get("name")] = dict(
                    desc=el.findtext("description", ""),
                    manuf=el.findtext("manufacturer", ""),
                    year=el.findtext("year", ""),
                    clone=el.get("cloneof", ""), slot=slot, exts=exts)
            el.clear()
        p.wait()
        self.games = games
        try:
            os.makedirs(cdir, exist_ok=True)
            with open(cpath, "w") as f:
                json.dump(games, f)
        except OSError:
            pass

    def verify(self, name):
        # -verifyroms prints the per-file detail of media_auditor::summarize
        # and exits 0 exactly when the set is CORRECT, BEST_AVAILABLE or
        # NONE_NEEDED - netplayui.cpp audit_ok. clifront.cpp verifyroms
        rc, out = self.run("-verifyroms", name)
        text = out.strip()
        if rc == 0 and not text:
            text = "ROM set is good."
        return rc == 0, text

    def rom_dirs(self):
        return [d for d in self.opts.get("rompath", "roms").split(";") if d]

    def media_find(self, name):
        # The name can come from the server, so a bare file name only - no
        # path that could reach outside the ROM directories.
        if not name or os.path.basename(name) != name or name in (".", ".."):
            return ""
        for d in self.rom_dirs():
            full = os.path.join(os.path.expanduser(d), name)
            if os.path.isfile(full):
                return os.path.abspath(full)
        return ""

    def media_hash(self, name):
        full = self.media_find(name)
        try:
            return media_hash_of(full) if full else ""
        except OSError:
            return ""

    def state_dir(self, game):
        # <state_directory>/<statename>/, statename with %g for the system.
        # Same reduction as netplaystate.cpp state_folder.
        tpl = self.opts.get("statename", "%g") or "%g"
        if "." in tpl:
            tpl = tpl[:tpl.rfind(".")]
        tpl = tpl.replace("%g", game)
        if "%" in tpl:
            tpl = game
        base = os.path.expanduser(self.opts.get("state_directory", "sta"))
        return os.path.abspath(os.path.join(base, tpl))

    def state_exists(self, game, name):
        return os.path.isfile(os.path.join(self.state_dir(game), name + ".sta"))

    def launch(self, c, game, log):
        args = [self.exe, game]
        if c.get("state"):
            args += ["-state", c["state"]]
        g = self.games.get(game, {})
        if c["cart"] and g.get("slot"):
            # The image loader does not search, so the full path.
            full = self.media_find(c["cart"])
            if full:
                args += ["-" + g["slot"], full]
        return subprocess.Popen(args, env=netplay_env(c, os.environ),
                                stdout=log, stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL)


# --------------------------------------------------------------------------
#  the GUI
# --------------------------------------------------------------------------

def desktop_scale():
    # Tk ignores the desktop's scale factor and draws at 96 dpi, which on a
    # HiDPI screen under GNOME's xwayland-native-scaling is half size. The
    # desktop publishes its scale as Xft.dpi, which GTK and Qt honour.
    # shortcut: read through xrdb only; without xrdb the window is unscaled.
    try:
        out = subprocess.run(["xrdb", "-query"], stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, timeout=2).stdout
    except (OSError, subprocess.SubprocessError):
        return 1.0
    for line in out.decode("latin-1").splitlines():
        p = line.split()
        if len(p) == 2 and p[0] == "Xft.dpi:":
            try:
                return max(1.0, float(p[1]) / 96.0)
            except ValueError:
                pass
    return 1.0


def gui(mame):
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox, font

    root = tk.Tk()
    root.title("Netplay")
    scale = desktop_scale()
    root.tk.call("tk", "scaling", scale * 96.0 / 72.0)

    def px(n):
        return int(n * scale)

    style = ttk.Style()
    style.theme_use("clam")
    # Row height is in pixels and does not follow the font.
    line = font.nametofont("TkDefaultFont").metrics("linespace")
    style.configure("Treeview", rowheight=int(line * 1.35))
    cfg = cfg_load()
    S = dict(sock=None, state="idle", rx=b"", name="", ping=-1,
             g_active=False, g_driver="", g_players=2, g_count=0,
             g_mine=False, g_amhost=False, g_cart="", g_carthash="",
             proc=None, log=None)

    def players_lo():
        return 1 if cfg["debug"] else 2

    # ---- layout -----------------------------------------------------------

    top = ttk.Frame(root, padding=6)
    top.pack(fill="x")
    ttk.Label(top, text="Server").pack(side="left")
    host_box = ttk.Combobox(top, width=24)
    host_box.pack(side="left", padx=4)
    ttk.Label(top, text="Port").pack(side="left")
    port_box = ttk.Entry(top, width=6)
    port_box.pack(side="left", padx=4)
    ttk.Label(top, text="Name").pack(side="left")
    name_box = ttk.Entry(top, width=14)
    name_box.pack(side="left", padx=4)
    connect_btn = ttk.Button(top, text="Connect")
    connect_btn.pack(side="left", padx=4)
    status = ttk.Label(root, text="", padding=(6, 0))
    status.pack(fill="x")

    mid = ttk.Frame(root, padding=6)
    mid.pack(fill="both", expand=True)
    left = ttk.Frame(mid)
    left.pack(side="left", fill="y")
    ttk.Label(left, text="Players").pack(anchor="w")
    players_lb = tk.Listbox(left, width=16, height=12)
    players_lb.pack(fill="y", expand=True)
    count_lbl = ttk.Label(left, text="")
    count_lbl.pack(anchor="w")

    chatf = ttk.Frame(mid)
    chatf.pack(side="left", fill="both", expand=True, padx=(6, 0))
    chat = tk.Text(chatf, width=48, height=12, state="disabled", wrap="word")
    chat.pack(fill="both", expand=True)
    sayf = ttk.Frame(chatf)
    sayf.pack(fill="x", pady=(4, 0))
    say_box = ttk.Entry(sayf)
    say_box.pack(side="left", fill="x", expand=True)
    send_btn = ttk.Button(sayf, text="Send")
    send_btn.pack(side="left", padx=(4, 0))

    gf = ttk.LabelFrame(root, text="Game", padding=6)
    gf.pack(fill="x", padx=6)
    row1 = ttk.Frame(gf)
    row1.pack(fill="x")
    ttk.Label(row1, text="System").pack(side="left")
    driver_box = ttk.Entry(row1, width=14)
    driver_box.pack(side="left", padx=4)
    pick_btn = ttk.Button(row1, text="Select game...")
    pick_btn.pack(side="left")
    media_btn = ttk.Button(row1, text="Cart / media...")
    media_btn.pack(side="left", padx=4)
    fullname = ttk.Label(row1, text="")
    fullname.pack(side="left", padx=4)

    row2 = ttk.Frame(gf)
    row2.pack(fill="x", pady=4)

    def combo(parent, label, values, width=4):
        ttk.Label(parent, text=label).pack(side="left")
        cb = ttk.Combobox(parent, values=values, width=width, state="readonly")
        cb.pack(side="left", padx=(2, 8))
        return cb

    players_cb = combo(row2, "Players", [str(n) for n in range(players_lo(), PLAYERS_HI + 1)])
    delay_cb = combo(row2, "Delay", [str(n) for n in range(DELAY_LO, DELAY_HI + 1)])
    batch_cb = combo(row2, "Frames per packet", [str(n) for n in range(BATCH_LO, BATCH_HI + 1)])
    saves_var = tk.BooleanVar(value=cfg["savestates"])
    saves_chk = ttk.Checkbutton(row2, text="Savestates", variable=saves_var)
    saves_chk.pack(side="left")
    config_var = tk.BooleanVar(value=cfg["sendconfig"])
    config_chk = ttk.Checkbutton(row2, text="Send my settings", variable=config_var)
    config_chk.pack(side="left", padx=8)

    row3 = ttk.Frame(gf)
    row3.pack(fill="x")
    # AUTO-SAVE IS OFF EVERY TIME and is not saved. netplay-ui.txt 5.3
    autosave_cb = combo(row3, "Auto-save (s)",
                        ["Off" if s == 0 else str(s) for s in AUTOSAVE_SECS], 5)
    autosave_cb.current(0)
    ttk.Label(row3, text="Start from state").pack(side="left")
    state_box = ttk.Entry(row3, width=10)
    state_box.pack(side="left", padx=4)
    saves_btn = ttk.Button(row3, text="Saves...")
    saves_btn.pack(side="left")

    row4 = ttk.Frame(gf)
    row4.pack(fill="x", pady=4)
    host_btn = ttk.Button(row4, text="Host game")
    join_btn = ttk.Button(row4, text="Join")
    leave_btn = ttk.Button(row4, text="Leave")
    start_btn = ttk.Button(row4, text="Start")
    for b in (host_btn, join_btn, leave_btn, start_btn):
        b.pack(side="left", padx=(0, 4))

    row5 = ttk.Frame(gf)
    row5.pack(fill="x")
    roster = ttk.Treeview(row5, columns=("slot", "player"), show="headings", height=4)
    roster.heading("slot", text="Slot")
    roster.heading("player", text="Player")
    roster.column("slot", width=px(40), stretch=False)
    roster.column("player", width=px(160))
    roster.pack(side="left", fill="y")
    result = tk.Text(row5, height=6, width=50, state="disabled", wrap="word")
    result.pack(side="left", fill="both", expand=True, padx=(6, 0))

    bot = ttk.Frame(root, padding=6)
    bot.pack(fill="x")
    stats_var = tk.BooleanVar(value=cfg["netstats"])
    ttk.Checkbutton(bot, text="Network stats", variable=stats_var).pack(side="left")
    direct_btn = ttk.Button(bot, text="Direct connect...")
    if cfg["debug"]:
        direct_btn.pack(side="left", padx=8)
    close_btn = ttk.Button(bot, text="Close")
    close_btn.pack(side="right")

    # ---- small helpers ----------------------------------------------------

    def text_set(w, s, add=False):
        w.configure(state="normal")
        if not add:
            w.delete("1.0", "end")
        elif w.get("1.0", "end").strip():
            w.insert("end", "\n")
        w.insert("end", s)
        w.see("end")
        w.configure(state="disabled")

    def say(line):
        text_set(chat, line, add=True)

    def res(s, add=False):
        text_set(result, s, add)

    def set_status(s):
        status.configure(text=s)

    def combo_set(cb, lo, hi, v):
        cb.current(max(lo, min(hi, v)) - lo)

    def combo_get(cb, lo):
        i = cb.current()
        return lo if i < 0 else lo + i

    def enable(w, on):
        w.state(["!disabled"] if on else ["disabled"])

    def show_fullname(drv):
        g = mame.games.get(drv)
        fullname.configure(text=g["desc"] if g else ("unknown system" if drv else ""))

    def host_fill():
        vals = [cfg["host"]]
        if cfg["host2"] and cfg["host2"] != cfg["host"]:
            vals.append(cfg["host2"])
        host_box.configure(values=vals)
        host_box.set(cfg["host"])

    def game_enable():
        # Driven by server state only, so a stale local idea of the game
        # cannot enable a button the server will refuse.
        up = S["state"] == "up"
        mine = up and S["g_active"] and S["g_mine"]
        enable(host_btn, up and not S["g_active"])
        enable(join_btn, up and S["g_active"] and not S["g_mine"]
               and S["g_count"] < S["g_players"])
        enable(leave_btn, mine)
        enable(start_btn, mine and S["g_amhost"] and S["g_count"] >= S["g_players"])
        editable = not S["g_active"] or S["g_amhost"]
        enable(pick_btn, not S["g_active"])
        for w in (players_cb, delay_cb, batch_cb):
            w.configure(state="readonly" if editable else "disabled")
        enable(saves_chk, editable)
        enable(config_chk, editable)
        # Snapshots are the host's alone, and off with savestates off.
        on = editable and saves_var.get()
        autosave_cb.configure(state="readonly" if on else "disabled")
        enable(state_box, on)
        enable(saves_btn, on)

    # ---- the ROM, state and media gates ------------------------------------

    def rom_gate(drv):
        # The audit IS the join button. notes/netplay-ui.txt 4
        if drv not in mame.games:
            res("No such system: " + drv)
            return False
        res("Checking ROMs...")
        root.update_idletasks()
        ok, text = mame.verify(drv)
        res(text)
        return ok

    def state_gate(drv):
        name = state_box.get().strip()
        if not name:
            return True
        if not saves_var.get():
            res("Starting from a state needs savestates enabled.\n"
                "The guests are given that state, and with savestates off\n"
                "they would cold-boot and desync from the first frame.")
            return False
        if not mame.state_exists(drv, name):
            res("No state %s for %s\n\nThe name is the bare one -state takes, "
                "like np01 -\nno .sta and no path." % (name, drv))
            return False
        return True

    def roots_text():
        return "\n".join(os.path.abspath(os.path.expanduser(d)) for d in mame.rom_dirs())

    def media_gate(drv):
        # WARN, DO NOT REFUSE: an empty slot is a legal boot.
        if not mame.games[drv]["slot"]:
            return True
        if cfg["cart"]:
            full = mame.media_find(cfg["cart"])
            if full:
                res("Media: %s\n%s" % (cfg["cart"], full), add=True)
            else:
                res("Media: %s\n\nWARNING: not in any of your ROM directories, "
                    "so it will not load - and the other players look it up the "
                    "same way.\n\nCopy it into one of these, or add its folder "
                    "to rompath:\n\n%s" % (cfg["cart"], roots_text()), add=True)
            return True
        res("Warning: no cart file selected.\n\nThis system takes media. Hosting "
            "now boots it with an empty slot.\n\nTo play a game: Leave, choose "
            "one with Cart / media, then Host game again.", add=True)
        return True

    def media_join_gate():
        # THE NAME FINDS THE FILE, THE HASH DECIDES.
        if not S["g_cart"]:
            return True
        mine = mame.media_hash(S["g_cart"])
        if not mine:
            res("You do not have %s.\n\nCopy it under that exact name into one "
                "of these, or add its folder to rompath:\n\n%s"
                % (S["g_cart"], roots_text()))
            return False
        if S["g_carthash"] not in ("", "none") and mine != S["g_carthash"]:
            res("Your copy of %s differs from the host's.\n\nSame name, different "
                "bytes - a different dump or region. Both players need the same "
                "file." % S["g_cart"])
            return False
        res("Media: " + S["g_cart"], add=True)
        return True

    # ---- the lobby socket --------------------------------------------------

    def send_line(line):
        if S["state"] != "up":
            return
        try:
            S["sock"].sendall((line + "\n").encode("latin-1", "replace"))
        except OSError:
            lobby_close("Connection lost.")

    def lobby_close(why=None):
        if S["sock"] is not None:
            S["sock"].close()
            S["sock"] = None
        S.update(state="idle", rx=b"", g_active=False, g_mine=False,
                 g_amhost=False, g_count=0)
        players_lb.delete(0, "end")
        roster.delete(*roster.get_children())
        count_lbl.configure(text="")
        connect_btn.configure(text="Connect")
        game_enable()
        if why:
            set_status(why)

    def lobby_open():
        host = host_box.get().strip()
        try:
            port = int(port_box.get())
        except ValueError:
            port = 0
        S["name"] = name_token(name_box.get())
        if not host or not 1 <= port <= 65535:
            set_status("Enter a server address and port.")
            return
        try:
            addr = socket.getaddrinfo(host, port, socket.AF_INET,
                                      socket.SOCK_STREAM)[0][4]
        except OSError:
            set_status("Could not resolve that address.")
            return
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setblocking(False)
        S["sock"] = s
        S["ping"] = -1
        t0 = time.perf_counter()
        s.connect_ex(addr)
        # THE HANDSHAKE IS THE MEASUREMENT: writable means the SYN-ACK came
        # back, one round trip. Timed here, not on the 100 ms tick, which
        # would swamp it. netplayui.cpp measure_ping
        _, w, _ = select.select([], [s], [], PING_WAIT_S)
        if w and s.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR) == 0:
            S["ping"] = int((time.perf_counter() - t0) * 1000 + 0.5)
        S["state"] = "connecting"
        connect_btn.configure(text="Disconnect")
        set_status("Connecting...")

    def lobby_poll():
        s = S["sock"]
        if S["state"] == "connecting":
            _, w, _ = select.select([], [s], [], 0)
            if not w:
                return
            if s.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR) != 0:
                lobby_close("Could not connect.")
                return
            S["state"] = "up"
            set_status("Connected.")
            if S["ping"] >= 0:
                say("* your ping to the server: %d ms" % S["ping"])
            send_line("HELLO " + S["name"])
            game_enable()
            return
        if S["state"] != "up":
            return
        while True:
            r, _, _ = select.select([s], [], [], 0)
            if not r:
                break
            try:
                data = s.recv(4096)
            except BlockingIOError:
                break
            except OSError:
                lobby_close("Connection lost.")
                return
            if not data:
                lobby_close("The server closed the connection.")
                return
            S["rx"] += data
        while b"\n" in S["rx"] and S["state"] == "up":
            line, S["rx"] = S["rx"].split(b"\n", 1)
            line = line.rstrip(b"\r").decode("latin-1")
            if line:
                lobby_handle(line)

    def roster_set(csv):
        roster.delete(*roster.get_children())
        S.update(g_mine=False, g_amhost=False, g_count=0)
        for i, n in enumerate(csv_names(csv)):
            roster.insert("", "end", values=(i + 1, n))
            if n == S["name"]:
                S["g_mine"] = True
                S["g_amhost"] = (i == 0)
            S["g_count"] = i + 1

    def lobby_handle(line):
        cmd, _, arg = line.partition(" ")
        if cmd == "YOUARE":
            # The server may have suffixed it; its name is the truth.
            S["name"] = arg
            name_box.delete(0, "end")
            name_box.insert(0, arg)
            set_status("Connected as " + arg)
        elif cmd == "PLAYERS":
            names = csv_names(arg)
            players_lb.delete(0, "end")
            for n in names:
                players_lb.insert("end", n)
            count_lbl.configure(text="%d here" % len(names))
        elif cmd == "CHAT":
            who, _, text = arg.partition(" ")
            if text:
                say("<%s> %s" % (who, text))
        elif cmd == "GAME":
            S.update(g_active=True, g_driver=arg.split(" ")[0],
                     g_players=kv(arg, "players", 2), g_cart=media_of(arg),
                     g_carthash=kvs(arg, "mediahash"))
            driver_box.delete(0, "end")
            driver_box.insert(0, S["g_driver"])
            show_fullname(S["g_driver"])
            combo_set(players_cb, players_lo(), PLAYERS_HI, S["g_players"])
            combo_set(delay_cb, DELAY_LO, DELAY_HI, kv(arg, "delay", 8))
            combo_set(batch_cb, BATCH_LO, BATCH_HI, kv(arg, "batch", 1))
            saves_var.set(kv(arg, "savestates", 1) != 0)
            config_var.set(kv(arg, "config", 1) != 0)
            game_enable()
        elif cmd == "ROSTER":
            roster_set(arg)
            game_enable()
        elif cmd == "NOGAME":
            S.update(g_active=False, g_mine=False, g_amhost=False, g_count=0,
                     g_driver="", g_cart="", g_carthash="")
            roster.delete(*roster.get_children())
            game_enable()
        elif cmd == "LAUNCH":
            game_launch(arg)
        elif cmd == "ERROR":
            say("* " + arg)
            res(arg)

    # ---- host, join, start, launch ----------------------------------------

    def on_host():
        drv = driver_box.get().strip()
        if not rom_gate(drv) or not state_gate(drv) or not media_gate(drv):
            return
        media = ""
        slot = mame.games[drv]["slot"]
        if slot and cfg["cart"]:
            # MEDIA RIDES THE LOBBY, keyed by the slot. console-roadmap.txt 3.1
            h = mame.media_hash(cfg["cart"])
            media = " media.%s=%s mediahash=%s" % (slot, hex_of(cfg["cart"]), h or "none")
        send_line("CREATE %s players=%u delay=%u batch=%u savestates=%u config=%u%s" % (
            drv, combo_get(players_cb, players_lo()), combo_get(delay_cb, DELAY_LO),
            combo_get(batch_cb, BATCH_LO), saves_var.get(), config_var.get(), media))

    def on_join():
        if rom_gate(S["g_driver"]) and media_join_gate():
            send_line("JOIN")

    def on_start():
        drv = S["g_driver"]
        if drv in mame.games and not state_gate(drv):
            return
        send_line("START")

    def game_launch(arg):
        # Built from the server's LAUNCH, not this window: the host's
        # settings win.
        drv = kvs(arg, "game")
        if not rom_gate(drv):
            # Leaving a slot held by someone who cannot run the game
            # strands the host.
            send_line("LEAVE")
            return
        c = dict(cfg)
        slot = kv(arg, "slot", 1)
        c.update(players=kv(arg, "players", 2), slot=slot,
                 delay=kv(arg, "delay", 8), batch=kv(arg, "batch", 1),
                 savestates=kv(arg, "savestates", 1) != 0,
                 sendconfig=kv(arg, "config", 1) != 0, cart=media_of(arg),
                 netstats=stats_var.get(), host=host_box.get().strip(),
                 port=int(port_box.get()))
        # Host-local and never on the wire: only slot 1 writes snapshots and
        # loads a start state; guests get it through the state push.
        c["autosave"] = AUTOSAVE_SECS[max(0, autosave_cb.current())] if slot == 1 else 0
        c["state"] = state_box.get().strip() if slot == 1 else ""
        for k in ("players", "slot", "delay", "batch", "savestates",
                  "sendconfig", "netstats"):
            cfg[k] = c[k]
        cfg_save(cfg)
        send_line("BYE")
        lobby_close(None)
        run_game(c, drv)

    def run_game(c, drv):
        S["log"] = open(LOG, "w")
        try:
            S["proc"] = mame.launch(c, drv, S["log"])
        except OSError as e:
            S["log"].close()
            res("Could not start the emulator: %s" % e)
            return
        set_status("Playing %s as player %u..." % (drv, c["slot"]))
        root.withdraw()

    def proc_poll():
        p = S["proc"]
        if p is None or p.poll() is None:
            return
        S["proc"] = None
        S["log"].close()
        root.deiconify()
        if p.returncode == 0:
            set_status("Game over. Connect to play again.")
            return
        set_status("The emulator exited with code %d." % p.returncode)
        try:
            with open(LOG, encoding="utf-8", errors="replace") as f:
                tail = f.read().strip().splitlines()[-12:]
        except OSError:
            tail = []
        res("Emulator exited with code %d. Last output (%s):\n\n%s"
            % (p.returncode, os.path.abspath(LOG), "\n".join(tail)))

    def tick():
        try:
            lobby_poll()
            proc_poll()
        finally:
            root.after(TICK_MS, tick)

    # ---- dialogs -----------------------------------------------------------

    def modal(title):
        d = tk.Toplevel(root)
        d.title(title)
        d.transient(root)
        return d

    def run_modal(d):
        d.wait_visibility()
        d.grab_set()
        d.wait_window()

    def pick_game():
        # netplaypick.cpp: four columns, header sort, clone grouping, a
        # filter OR'd across the ticked fields, three settings remembered.
        pc = dict(grouped=1, sort=0, reverse=0)
        try:
            with open(PICK_INI) as f:
                for line in f:
                    p = line.split()
                    if len(p) == 2 and p[0] in pc:
                        pc[p[0]] = int(p[1])
        except (OSError, ValueError):
            pass
        cols = ("desc", "name", "manuf", "year")
        d = modal("Select Game")
        bar = ttk.Frame(d, padding=6)
        bar.pack(fill="x")
        filt = ttk.Entry(bar, width=24)
        filt.pack(side="left")
        fv = [tk.BooleanVar(value=True), tk.BooleanVar(), tk.BooleanVar()]
        for v, t in zip(fv, ("Machine", "Manufacturer", "Directory")):
            ttk.Checkbutton(bar, text=t, variable=v).pack(side="left", padx=4)
        gv = tk.BooleanVar(value=bool(pc["grouped"]))
        ttk.Checkbutton(bar, text="Group clones", variable=gv).pack(side="left", padx=8)
        n_lbl = ttk.Label(bar, text="")
        n_lbl.pack(side="right")
        tf = ttk.Frame(d)
        tf.pack(fill="both", expand=True, padx=6)
        tv = ttk.Treeview(tf, columns=cols[1:], height=20)
        sb = ttk.Scrollbar(tf, command=tv.yview)
        tv.configure(yscrollcommand=sb.set)
        tv.pack(side="left", fill="both", expand=True)
        sb.pack(side="left", fill="y")
        for c, t, w in zip(("#0",) + cols[1:], ("Machine", "Directory", "Manufacturer", "Year"),
                           (300, 90, 180, 50)):
            tv.column(c, width=px(w), stretch=(c == "#0"))
        chosen = [None]

        def match(name, g, text):
            if not text:
                return True
            fields = (g["desc"], g["manuf"], name)
            ticked = [f for f, v in zip(fields, fv) if v.get()]
            return not ticked or any(text in f.lower() for f in ticked)

        def fill(*_):
            tv.delete(*tv.get_children())
            text = filt.get().strip().lower()
            key = cols[pc["sort"]]
            items = [(n, g) for n, g in mame.games.items() if match(n, g, text)]
            items.sort(key=lambda it: (it[0] if key == "name" else it[1][key]).lower(),
                       reverse=bool(pc["reverse"]))
            shown = {n for n, _ in items}
            for n, g in items:
                # A clone whose parent the filter removed is a top-level row.
                parent = g["clone"] if gv.get() and g["clone"] in shown else ""
                if parent and not tv.exists(parent):
                    pg = mame.games[parent]
                    tv.insert("", "end", iid=parent, text=pg["desc"],
                              values=(parent, pg["manuf"], pg["year"]), open=True)
                if not tv.exists(n):
                    tv.insert(parent, "end", iid=n, text=g["desc"],
                              values=(n, g["manuf"], g["year"]), open=True)
            n_lbl.configure(text="%d of %d" % (len(items), len(mame.games)))
            cur = driver_box.get().strip()
            if cur and tv.exists(cur):
                tv.selection_set(cur)
                tv.see(cur)

        def sort_by(i):
            pc["reverse"] = int(not pc["reverse"]) if pc["sort"] == i else 0
            pc["sort"] = i
            fill()

        for i, (c, t) in enumerate(zip(("#0",) + cols[1:],
                                       ("Machine", "Directory", "Manufacturer", "Year"))):
            tv.heading(c, text=t, command=lambda i=i: sort_by(i))

        def ok(*_):
            sel = tv.selection()
            if sel:
                chosen[0] = sel[0]
                d.destroy()

        btns = ttk.Frame(d, padding=6)
        btns.pack(fill="x")
        ttk.Button(btns, text="Cancel", command=d.destroy).pack(side="right")
        ttk.Button(btns, text="OK", command=ok).pack(side="right", padx=4)
        filt.bind("<KeyRelease>", fill)
        for v in fv + [gv]:
            v.trace_add("write", fill)
        tv.bind("<Double-1>", ok)
        tv.bind("<Return>", ok)
        fill()
        filt.focus_set()
        run_modal(d)
        pc["grouped"] = int(gv.get())
        try:
            with open(PICK_INI, "w") as f:
                f.write("".join("%s %d\n" % kvp for kvp in pc.items()))
        except OSError:
            pass
        if chosen[0]:
            driver_box.delete(0, "end")
            driver_box.insert(0, chosen[0])
            show_fullname(chosen[0])

    def pick_state():
        drv = driver_box.get().strip()
        if drv not in mame.games:
            res("Pick a system first - the states listed are that system's own.")
            return
        sdir = mame.state_dir(drv)
        try:
            names = [f[:-4] for f in os.listdir(sdir) if f.endswith(".sta")]
        except OSError:
            names = []
        # Newest first, not by name: the rotation reuses np01 once it wraps.
        names.sort(key=lambda n: os.path.getmtime(os.path.join(sdir, n + ".sta")),
                   reverse=True)
        d = modal("Savestates - " + drv)
        ttk.Label(d, text=sdir, padding=6).pack(anchor="w")
        body = ttk.Frame(d, padding=6)
        body.pack(fill="both", expand=True)
        tv = ttk.Treeview(body, columns=("when",), height=12)
        tv.heading("#0", text="State")
        tv.heading("when", text="Saved")
        tv.column("#0", width=px(100))
        tv.column("when", width=px(150))
        tv.pack(side="left", fill="y")
        preview = ttk.Label(body, text="", width=40, anchor="center")
        preview.pack(side="left", fill="both", expand=True, padx=6)
        for n in names:
            t = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(
                os.path.getmtime(os.path.join(sdir, n + ".sta"))))
            tv.insert("", "end", iid=n, text=n, values=(t,))
        if not names:
            preview.configure(text="No savestates for this system yet.")
        chosen = [None]

        def show(*_):
            sel = tv.selection()
            if not sel:
                return
            png = os.path.join(sdir, sel[0] + ".png")
            # ONLY OUR SNAPSHOTS HAVE A THUMBNAIL. netplay-ui.txt 5.3
            try:
                img = tk.PhotoImage(file=png)
                k = max(1, -(-img.width() // px(320)))
                img = img.subsample(k, k)
                preview.configure(image=img, text="")
                preview.image = img
            except (tk.TclError, OSError):
                preview.configure(image="", text="No thumbnail - saved by hand.")
                preview.image = None

        def ok(*_):
            sel = tv.selection()
            if sel:
                chosen[0] = sel[0]
                d.destroy()

        tv.bind("<<TreeviewSelect>>", show)
        tv.bind("<Double-1>", ok)
        btns = ttk.Frame(d, padding=6)
        btns.pack(fill="x")
        ttk.Button(btns, text="Cancel", command=d.destroy).pack(side="right")
        ttk.Button(btns, text="OK", command=ok).pack(side="right", padx=4)
        if names:
            tv.selection_set(names[0])
        run_modal(d)
        if chosen[0]:
            state_box.delete(0, "end")
            state_box.insert(0, chosen[0])

    def pick_media():
        drv = driver_box.get().strip()
        if drv not in mame.games:
            res("Pick a system first.")
            return
        g = mame.games[drv]
        if not g["slot"]:
            res(drv + " takes no media - it is not a console.")
            return
        d = modal("Media - " + drv)
        f = ttk.Frame(d, padding=6)
        f.pack(fill="both", expand=True)
        ttk.Label(f, text=g["slot"] + ":").grid(row=0, column=0, sticky="w")
        box = ttk.Entry(f, width=40)
        box.grid(row=0, column=1, padx=4)
        box.insert(0, cfg["cart"])
        note = ttk.Label(f, text="", wraplength=px(420), justify="left")
        note.grid(row=1, column=0, columnspan=3, sticky="w", pady=6)

        def bare():
            return os.path.basename(box.get().strip())

        def update(*_):
            # SAYS WHETHER THE PICK WILL ACTUALLY LOAD, as it is picked.
            n = bare()
            if not n:
                note.configure(text="No media: the system will boot with an empty slot.")
            elif mame.media_find(n):
                note.configure(text="Found: %s\n\nEvery player needs this file under "
                               "the same name in one of their own ROM directories."
                               % mame.media_find(n))
            else:
                note.configure(text="NOT in any of your ROM directories, so it will "
                               "not load.\nCopy it into one of these, or add its folder "
                               "to rompath:\n\n" + roots_text())

        def browse():
            pat = " ".join("*." + e for e in g["exts"]) or "*"
            p = filedialog.askopenfilename(parent=d, filetypes=[
                (g["slot"] + " files", pat), ("All files", "*")])
            if p:
                # The lobby only ever carries a BARE NAME.
                box.delete(0, "end")
                box.insert(0, os.path.basename(p))
                update()

        def ok():
            n = bare()
            if n and not mame.media_find(n) and not messagebox.askyesno(
                    "Netplay", "%s is not in any of your ROM directories, so it "
                    "will not load - and the other players look it up the same "
                    "way.\n\nKeep it anyway?" % n, parent=d):
                return
            cfg["cart"] = n
            cfg_save(cfg)
            d.destroy()

        ttk.Button(f, text="Browse...", command=browse).grid(row=0, column=2)
        btns = ttk.Frame(d, padding=6)
        btns.pack(fill="x")
        ttk.Button(btns, text="Cancel", command=d.destroy).pack(side="right")
        ttk.Button(btns, text="OK", command=ok).pack(side="right", padx=4)
        box.bind("<KeyRelease>", update)
        update()
        run_modal(d)
        res("Media: " + cfg["cart"] if cfg["cart"] else
            "Media: none - the system will boot empty.")

    def direct_connect():
        # The expert path: validates rather than constrains. netplay-ui.txt 5.1
        drv = driver_box.get().strip()
        if drv not in mame.games:
            set_status("Pick a system first.")
            return
        d = modal("Netplay - %s [%s]" % (mame.games[drv]["desc"], drv))
        f = ttk.Frame(d, padding=6)
        f.pack(fill="both", expand=True)
        boxes = {}
        for i, (k, t) in enumerate((("host", "Server"), ("port", "Port"),
                                    ("players", "Players"), ("slot", "You are player"),
                                    ("delay", "Delay"), ("batch", "Frames per packet"))):
            ttk.Label(f, text=t).grid(row=i, column=0, sticky="w")
            e = ttk.Entry(f, width=20)
            e.insert(0, str(cfg[k]))
            e.grid(row=i, column=1, sticky="w", pady=1)
            boxes[k] = e
        sv = tk.BooleanVar(value=cfg["savestates"])
        cv = tk.BooleanVar(value=cfg["sendconfig"])
        ttk.Checkbutton(f, text="Savestates", variable=sv).grid(row=6, column=0, sticky="w")
        ttk.Checkbutton(f, text="Send my settings", variable=cv).grid(row=6, column=1, sticky="w")
        out = tk.Text(f, height=6, width=50, state="disabled", wrap="word")
        out.grid(row=7, column=0, columnspan=2, pady=4)
        go = [None]

        def verify():
            text_set(out, "Checking...")
            d.update_idletasks()
            text_set(out, mame.verify(drv)[1])

        def ok():
            c = dict(cfg)
            c["host"] = boxes["host"].get().strip()
            for k in ("port", "players", "slot", "delay", "batch"):
                try:
                    c[k] = int(boxes[k].get())
                except ValueError:
                    c[k] = 0
            c.update(savestates=sv.get(), sendconfig=cv.get(),
                     netstats=stats_var.get(), autosave=0, state="")
            err = direct_check(c)
            if err:
                messagebox.showwarning("Netplay", err, parent=d)
                return
            go[0] = c
            d.destroy()

        btns = ttk.Frame(d, padding=6)
        btns.pack(fill="x")
        ttk.Button(btns, text="Verify ROMs", command=verify).pack(side="left")
        ttk.Button(btns, text="Cancel", command=d.destroy).pack(side="right")
        ttk.Button(btns, text="Launch", command=ok).pack(side="right", padx=4)
        run_modal(d)
        if go[0]:
            c = go[0]
            for k in ("host", "port", "players", "slot", "delay", "batch",
                      "savestates", "sendconfig"):
                cfg[k] = c[k]
            cfg_save(cfg)
            if S["state"] != "idle":
                send_line("BYE")
            lobby_close(None)
            run_game(c, drv)

    # ---- wiring ------------------------------------------------------------

    def on_connect():
        if S["state"] != "idle":
            send_line("BYE")
            lobby_close("Disconnected.")
            return
        # MRU of two: connecting promotes the address to slot 1.
        typed = host_box.get().strip()
        if typed != cfg["host"]:
            cfg["host2"], cfg["host"] = cfg["host"], typed
        cfg["name"] = name_token(name_box.get())
        name_box.delete(0, "end")
        name_box.insert(0, cfg["name"])
        try:
            cfg["port"] = int(port_box.get())
        except ValueError:
            pass
        cfg_save(cfg)
        host_fill()
        lobby_open()

    def on_chat(*_):
        text = say_box.get()
        if not text:
            return
        if S["state"] != "up":
            set_status("Not connected.")
            return
        send_line("CHAT " + text[:200])
        say_box.delete(0, "end")

    def on_close():
        if S["proc"] is not None:
            return
        if S["state"] != "idle":
            send_line("BYE")
        lobby_close(None)
        root.destroy()

    connect_btn.configure(command=on_connect)
    send_btn.configure(command=on_chat)
    say_box.bind("<Return>", on_chat)
    pick_btn.configure(command=pick_game)
    media_btn.configure(command=pick_media)
    saves_btn.configure(command=pick_state)
    host_btn.configure(command=on_host)
    join_btn.configure(command=on_join)
    leave_btn.configure(command=lambda: send_line("LEAVE"))
    start_btn.configure(command=on_start)
    direct_btn.configure(command=direct_connect)
    saves_chk.configure(command=game_enable)
    driver_box.bind("<KeyRelease>", lambda *_: show_fullname(driver_box.get().strip()))
    close_btn.configure(command=on_close)
    root.protocol("WM_DELETE_WINDOW", on_close)

    host_fill()
    port_box.insert(0, str(cfg["port"]))
    name_box.insert(0, cfg["name"])
    combo_set(players_cb, players_lo(), PLAYERS_HI, cfg["players"])
    combo_set(delay_cb, DELAY_LO, DELAY_HI, cfg["delay"])
    combo_set(batch_cb, BATCH_LO, BATCH_HI, cfg["batch"])
    game_enable()
    set_status("Loading the system list...")
    root.update()
    mame.load_games()
    set_status("%d systems. Not connected." % len(mame.games))
    root.after(TICK_MS, tick)
    root.mainloop()


# --------------------------------------------------------------------------

def selftest():
    assert hex_of("dino.zip") == "64696e6f2e7a6970"
    assert unhex(hex_of("Sonic (USA).md")) == "Sonic (USA).md"
    assert unhex("abc") == "" and unhex("zz") == ""
    line = "dino host=a players=3 delay=6 batch=2 media.cart=6162 mediahash=sha1=\"x\""
    assert kv(line, "players", 2) == 3 and kv(line, "delay", 8) == 6
    assert kv(line, "missing", 7) == 7
    assert kvs(line, "hash") == "" and kvs(line, "mediahash") == 'sha1="x"'
    assert media_of(line) == "ab" and media_of("dino players=2") == ""
    assert name_token("  dave  smith ") == "dave" and name_token(" ") == "player"
    assert csv_names("a,,b,") == ["a", "b"]
    c = cfg_defaults()
    c.update(players=1, debug=False)
    assert direct_check(c) == "Players must be 2 to 10."
    c.update(players=2, delay=1, host="10.0.0.1")
    assert "loss tolerance" in direct_check(c)
    c.update(delay=6)
    assert direct_check(c) is None
    env = netplay_env(c, {})
    assert env["NETPLAY_MODE"] == "udp" and env["NETPLAY_TAG"] == "player 1"
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, UI_INI)
        c.update(cart="a b.zip", host2="h2", netstats=True)
        cfg_save(c, p)
        assert cfg_load(p) == c
    print("selftest ok")


def main():
    ap = argparse.ArgumentParser(description="Netplay lobby for the SDL build.")
    ap.add_argument("--mame", help="path to the emulator binary")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        selftest()
        return 0
    exe = find_mame(a.mame)
    if not exe:
        sys.stderr.write("No emulator found. Build it (CLAUDE.md, Build "
                         "configuration) or pass --mame PATH.\n")
        return 1
    gui(Mame(exe))
    return 0


if __name__ == "__main__":
    sys.exit(main())
