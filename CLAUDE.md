# MAMEUI 0.210 - Netplay

## Mission

Add a deterministic, server-mediated netplay layer to MAMEUI 0.210, and a relay
server to go with it. Conceptually Kaillera - every client sends its local input
for frame N to a server, the server redistributes the complete input set for
frame N, and every instance steps in lockstep - but built on MAME's own
determinism guarantees rather than against them, with rollback layered on later.

This is **not** a reverse-engineering project. MAME 0.210 already works. The job
is to add one subsystem to a large, healthy, third-party codebase without
breaking it and without making it painful to rebase.

---

## Hard rules

### 1. Git is read-only

Enforced in `.claude/settings.json`. Read commands (`status`, `log`, `diff`,
`show`, `blame`, `ls-files`, ...) are allowed. Everything that mutates a repo,
an index, a working tree, or a remote is denied - including `add`, `commit`,
`push`, `pull`, `fetch`, `checkout`, `reset`, `rebase`, `merge`, `stash`, `tag`,
`remote`, `config`, `init`.

**Commits, branches, and pushes are the user's job, never the agent's.** If a
change is ready to commit, say so and stop.

> This is a guardrail against accident, not a security sandbox. Don't route
> around it - no `sh -c`, no aliases, no scripts that shell out to git. If a
> git write is genuinely needed, ask for it.

### 2. Upstream code is upstream - keep the rebase surface small

This working tree is a checkout of `Robbbert/mameui` at tag `tag210`, on branch
`netplay`. Roughly 30,000 files belong to somebody else. Every line we change in
one of them is a line we re-apply by hand if we ever move to another tag.

Therefore:

- **Prefer a new file to an edit.** New files never conflict.
- **When an edit is unavoidable, make it one contiguous block** with a
  `NETPLAY:` marker comment, so it is findable and re-appliable.
- **Every upstream file we touch is listed in `notes/upstream-edits.txt`**, with
  what changed and why. That list is the rebase checklist. If it is not on the
  list, we did not change it.
- **Never reformat, retab, or tidy upstream code.** Not the file you are
  editing, not the function you are editing. A whitespace change in
  `ioport.cpp` is a permanent merge conflict for zero benefit.
- **Do not sweep upstream comments.** Rule 7 governs code we write. MAME's own
  comments are not ours to curate.

### 3. Determinism is the product - never guess about it

The entire design rests on two instances of the same binary producing
bit-identical machine state from the same input stream. Any claim about what
does or does not perturb emulated state gets **read out of the source**, never
recalled.

If you cannot point at the code, say so and stop. A wrong determinism claim does
not produce a compile error - it produces a desync three months later that
nobody can reproduce.

Known divergence sources and their evidence live in `notes/determinism.txt`.
Add to it; do not re-derive it in chat.

### 4. Separate fact from inference

Every claim in `notes/` carries a tag:

- **`[verified]`** - read directly out of this source tree, or observed at
  runtime in our own build. Cite the file and line.
- **`[inferred]`** - a reasoned conclusion from verified evidence. State the
  evidence.
- **`[upstream]`** - documented by MAME or MAMEdev (`docs/`, the wiki, a commit
  message). True of MAME in general, but check it holds at 0.210.
- **`[guess]`** - a hypothesis worth testing. Never build on one without saying
  so.

A measurement made by a script we wrote is `[verified]` only if the script is in
`tools/` and can be re-run. Otherwise it is a number in a transcript, which is
to say it is gone.

### 5. Plain ASCII only

Every byte in every document, script, and source comment must be `<= 0x7F`.
No em dashes, no curly quotes, no arrows, no box-drawing characters, no accented
letters, no ellipsis glyph. Write `->` not an arrow, `-` not an em dash, `...`
not an ellipsis, `x` not a multiplication sign, `+`/`-`/`|` for diagrams.

```
python tools/asciify.py --check    ASCII, line width, table alignment
python tools/asciify.py --fix      transliterate and rewrap in place
```

With no PATH it walks **our** paths only - `notes/`, `tools/`, `server/`,
`src/emu/netplay/`, `CLAUDE.md`. Upstream MAME contains plenty of non-ASCII
(`language/`, `hash/`, driver comments) and is none of our business.

### 6. Documents are RFC-style plain text, not markdown

Our documents live in `notes/` as `.txt`. This file is the one exception -
Claude Code requires the name `CLAUDE.md` and the harness renders it - but
everything else is laid out like an old RFC:

```
MAMEUI Netplay                                             Design Note


                        FRAME SEQUENCING


Abstract

   Body text indented three spaces, wrapped at 72 columns.

Table of Contents

   1. First Section
      1.1. A Subsection

1.  First Section

   +--------------+-------+
   | Column       | Count |
   +==============+=======+
   | aligned with | 2542  |
   +--------------+-------+
```

Rules that follow:

- **No markdown syntax in a `.txt` file.** No `#` headings, no `**bold**`, no
  backticks, no `*emphasis*`, no `[text](link)`, no `|`-delimited markdown
  tables. If it needs a renderer to look right, it is wrong.
- **Tables drawn with `+---+` borders and space-aligned columns.** `+===+` under
  the header row. Every `|` row exactly as wide as the border above it.
- **Numbered sections** (`1.`, `1.1.`) with a Table of Contents.
- **Body text indented three spaces, wrapped at 72 columns.** Tables may run to
  120 including their indent.
- Blocks indented four or more spaces are preformatted and never rewrapped.

`tools/rfcfmt.py --fix P` converts a markdown-ish draft to RFC form. It is
one-way, not a round-trip formatter, and refuses a file already in RFC form.
Author new documents in RFC form directly.

**`notes/` is ours. `docs/` is MAME's Sphinx tree - do not write there.**

### 7. Record facts, not the journey - and record each one ONCE

Documentation says **what the code does and why**. It does not narrate the
search that found it.

**Do not write:**

- "we first thought X, then found Y" - just write Y.
- "this took three sessions", "the first sweep was wrong", "an earlier note
  said" - the reader does not care how the fact was obtained.
- restating what a previous section already established.
- congratulation, summary-of-the-summary, or which tool was used.

**The one exception - a trap worth marking.** If an approach looks obviously
right and is actually wrong, say so in a sentence or two and say why. "Both
peers must use the same OPTIMIZE level; the FP codegen differs" is a fact about
the problem. "I built one peer at -O0 and it desynced" is a diary entry. Write
the first.

#### 7.1. One fact, one home

The same fact in two files means one of them goes stale, and only the other gets
updated.

| What | Where it lives |
|---|---|
| where we hook MAME, and what upstream guarantees | `notes/netplay-hooks.txt` |
| what perturbs emulated state | `notes/determinism.txt` |
| the wire format | `notes/protocol.txt` |
| how the trimmed driver set is derived | `notes/coinop-subtarget.txt` |
| every upstream file we changed | `notes/upstream-edits.txt` |
| why our C++ is shaped this way | the `src/emu/netplay/` comment |
| the build commands you need to run anything | this file, **Build configuration** |
| toolchain quirks and their evidence | `notes/build-config.txt` |
| project rules | this file |

Everything else **points at** that home - `see determinism.txt 3.4` - and does
not restate its contents.

#### 7.2. Correct in place; never append

When a fact changes, **edit the sentence that is now wrong.** Do not leave it
standing and add a correction beside it - that produces two statements where one
is false, and the reader cannot tell which.

A correction earns its own paragraph only when the WRONG reading is one a future
reader would independently arrive at.

#### 7.3. Proportion

A note should be shorter than the code it describes. Prefer a table to a
paragraph, a formula to a description of a formula, and silence to a
restatement.

### 8. Comments must earn their place

This governs **code we write** - `src/emu/netplay/`, `server/`, `tools/`, and
the lines we add inside upstream files. It does not govern MAME's own comments
(rule 2), and it changes no behaviour.

**A comment stays only if it is one of these seven:**

| Keep | Why |
|---|---|
| **an upstream anchor** | `ioport.cpp frame_update()` - name the function, never the line; lines drift across tags |
| **a determinism constraint** | why this must not change. The product depends on it |
| **a landmine** | looks obviously right, is wrong. State the PROBLEM as a fact, never the story |
| **a deliberate divergence from upstream** | so nobody "fixes" it back during a rebase |
| **a wire-format invariant** | byte order, field width, version gate |
| **`[open]` / not implemented** | what is missing and what it would take |
| **a non-obvious WHY** | something reading the code cannot give you |

**Everything else goes:**

- **The dev journal.** "we first thought X", "this took three sessions", "an
  earlier version said". The highest-value deletions. The one exception is rule
  7's: an approach that looks right and is wrong keeps a sentence, as a fact.
- **Restatement of the code.** If the line below says it, the comment goes. Do
  not explain C++ to the reader.
- **Our own keybindings and menu paths.** Ours change; the comment is stale the
  same week.
- **Measured numbers a `notes/` file already holds.** Replace with a pointer.
  The exception: a constant the code actually computes with keeps its value
  inline.
- **Anything stale.** Highest priority, because a stale comment actively
  misleads where a missing one merely omits.

**Long explanations move to `notes/` and leave a one-line pointer.** The two are
married; expect to edit both in the same session.

> This rule exists because the failure mode is well known: comments accrete into
> a development diary, useful facts get braided into the cruft, and separating
> them later costs more than writing them correctly cost. Write the fact; skip
> the journey.

### 9. Match MAME's house style in MAME's tree

Measured in this tree, not assumed:

- **Tabs for indentation, 4 wide.** `src/emu/ioport.cpp` is 2130 tab-indented
  lines against 75 space-indented. `[verified]`
- **Two-line licence header on every source file** - `// license:BSD-3-Clause`
  then `// copyright-holders:<name>`. `[verified]`
- There is no `.editorconfig` and `docs/CONTRIBUTING.md` states no style rules.
  The conventions above are observed, not documented. `[verified]`
- MAME types: `u8`/`u16`/`u32`/`u64`, `s8`..`s64`, `attotime`, `std::string`.
  Use them rather than `uint32_t` or `DWORD`.

Our own standalone code (`server/`, `tools/`) is not bound by MAME style, but is
bound by rules 5 through 8.

### 10. Never commit ROMs, binaries, or build output

`roms/`, `build/`, and any `.exe`/`.pdb`/`.chd` stay out of version control.
Upstream's `.gitignore` already covers `build/`. ROM images are copyrighted and
are not ours to redistribute.

### 11. Never run MAME from the repo root

MAME writes its configuration and its output beside wherever it is started.
Run it from the root and it scatters `cfg/`, `nvram/`, `sta/`, `snap/`,
`folders/`, `history/`, `MAMEUI.ini`, `ui.ini`, `MAME_g.ini` and a per-game
`.ini` across a tree of 30,000 files you are trying to keep legible. It also
overwrites `netplay.ini`, which holds the address of the server the user is
testing against.

Two working directories, and neither is the root:

| Who | Where | Contains |
|---|---|---|
| the user | `run/` | their own GUI testing - **off limits** |
| an agent | `run/agent/` | its own everything - wipe freely |

**An agent runs MAME with the working directory at `run/agent/`.** Every
config file MAME and MAMEUI touch - `netplay.ini`, `netplaypick.ini`,
`MAMEUI.ini`, `ini/ui.ini`, the per-game inis - resolves against the working
directory, so moving it is the whole isolation mechanism. No code change and
no command-line options are needed.

#### 11.1. The sandbox needs two seeded files, and they must use
absolute read paths

`run/agent/mame.ini` serves a direct emulator launch; `run/agent/ini/mame.ini`
serves a GUI launch, because MAMEUI takes its directories from its own ini and
forces them at `OPTION_PRIORITY_CMDLINE` (`mui_opts.cpp` `SetDirectories`), so
the command line cannot override them. Both are data, not code - recreate them
if they are missing.

THE READ PATHS MUST BE ABSOLUTE. `hashpath`, `pluginspath`, `artpath`,
`languagepath`, `samplepath`, `ctrlrpath`, `cheatpath` and `rompath` default to
bare names that resolve against the working directory. Left relative they point
at `run/agent/hash`, `run/agent/plugins` and so on, which do not exist - and
nothing says so. The symptom is a driver that exits during startup with no
diagnostic.

Writable paths stay relative, so everything lands inside the sandbox.

#### 11.2. The user's files are not ours to touch

Never read, write, move or delete `run/` (except `run/agent/`), `ini/`,
`roms/`, `netplay.ini` or `MAMEUI.ini`. They hold server addresses, ROM paths,
input remaps and save data that no rebuild reproduces.

**Never delete configuration to tidy up.** Say what is stale and ask. A
missing `ini/mame.ini` costs the user their ROM path, and nothing in the UI
explains why every audit suddenly fails.

`rm -rf run/agent` is always safe and is the reset.

#### 11.3. What the sandbox does not isolate

The binary. An agent rebuilding to test its own change rebuilds the exe the
user runs. Close any running instance first - the link fails with LNK1104 and
leaves the PREVIOUS exe in place, so the next run silently tests old code.

### 12. Never run an unbounded build, and never a concurrent one

**An agent builds with `tools\build.bat` and nothing else.** Two projects
at a time, multiprocessor compilation off. That matches the user's own
Options -> Projects and Solutions -> Build and Run setting, so an agent
build never loads the machine harder than their own does.

```
tools\build.bat                  Release Win32, capped
tools\build.bat Release emu      one project
```

**Do not raise the cap.** Not for a "quick" full rebuild, not to save
wall-clock time. The user works on this machine while a build runs.

#### 12.1. Capping needs two knobs, and one of them is not a property

A bare `msbuild /m` on this solution spawned **46 `cl.exe`** and made the
machine unusable. There are two multipliers:

| Knob | What it does | How to cap it |
|---|---|---|
| `/m` | runs that many of the 131 **projects** at once; unbounded with no number | `/m:2` |
| `/MP` | each project's `cl.exe` forks per core as well | `tools/cap_mp.props` |

`/p:MultiProcessorCompilation=false` ON THE COMMAND LINE DOES NOTHING.
genie writes that setting into an `ItemDefinitionGroup`, which makes it
`ClCompile` **item metadata**, and a global property does not override item
metadata. `tools/cap_mp.props` sets it from inside a target instead, where
the items already exist, and is injected with
`/p:ForceImportAfterCppTargets`. `[verified]` Release|Win32 carries
`MultiProcessorCompilation=true` in all 131 generated projects.

**Setting it in the MSVC GUI does not hold.** The `.vcxproj` files under
`build/projects/` are generated; `make vs2017 ...` overwrites them and the
setting with them. Only the IDE's own "maximum number of parallel project
builds" is a user setting that survives, and it caps projects, not `/MP`.

#### 12.2. One build at a time, always

Two builds over one obj tree kill each other: the loser dies with
`Cannot open compiler generated file: Permission denied`. The lasting
damage is worse than the failure - **a `cl.exe` killed mid-write leaves a
truncated `.obj` that the next build treats as up to date**, so a later
link fails or, worse, succeeds against stale code.

`tools\build.bat` refuses to start if **`cl.exe`** is running, which also
covers the user building in the IDE.

**It deliberately does NOT test `msbuild.exe`.** Visual Studio keeps MSBuild
node processes alive and idle long after a build finishes (node reuse), so
that test means "the IDE is open", not "a build is running" - it refused
every build while VS was merely open. A live `cl.exe` is what actually says
somebody is compiling.

If a build was ever killed part-way, say so and recommend a `Rebuild` of the
affected projects rather than trusting the obj tree.

**Before starting any build, check whether the user is already building.**
A rebuild triggered by an `ioport.h` change touches every driver and runs
for a long time; starting one on a machine somebody is working on is not
a neutral act.

---

## Repository layout

Everything of ours is in a path upstream does not use, except for the enumerated
edits in `notes/upstream-edits.txt`.

```
MAMEUI/                        upstream Robbbert/mameui @ tag210, branch netplay
  CLAUDE.md                    this file                            [ours]
  .claude/settings.json        permissions - read-only git          [ours]

  notes/                       our documents, RFC-style plain .txt  [ours]
    README.txt                 doc conventions and evidence tags
    upstream-edits.txt         every upstream file we changed - the rebase list
    determinism.txt            what perturbs emulated state, and the evidence
    netplay-hooks.txt          where we hook MAME and what it guarantees
    protocol.txt               the wire format
    build-config.txt           toolchain, flags, and the MSVC 2017 quirks
    coinop-subtarget.txt       how the trimmed driver set is generated

  tools/                       our scripts - python 3.10            [ours]
    asciify.py                 plain-ASCII rule: --check / --fix
    rfcfmt.py                  markdown-ish draft -> RFC text (one way)
    retab.py                   tab/space rule for our own sources
    mech_scan.py               resolve MACHINE_MECHANICAL through the macros
    gen_coinop.py              regenerate coinop.flt + coinop.lua from arcade.*
    netplay_analog.py          analog axis widths and counts, from -listxml
    netplay_wordtest.cpp       wire-format round trip, standalone, no ROM
    build.bat                  THE build command - rule 12
    cap_mp.props               turns /MP off; injected by build.bat

  server/                      the relay server - standalone        [ours]

  src/emu/netplay/             the netplay core, compiled into emu  [ours]
  src/mame/coinop.flt          trimmed driver filter                [ours]
  scripts/target/mame/coinop.lua   its target script                [ours]

  src/                         MAME source                      [upstream]
    emu/ioport.cpp               the input hook lives here
    emu/save.h                   ram_state + rewinder - the rollback primitives
    osd/winui/                   the MAMEUI frontend
  scripts/                     genie build scripts              [upstream]
  docs/                        MAME's Sphinx docs - NOT ours     [upstream]
  3rdparty/                    bundled libs, incl. asio          [upstream]
  projects/                    MAME's out-of-tree slot - gitignored, unused
  build/                       generated projects and objects   [not committed]
  roms/                        ROM images                       [not committed]
  run/                         the user's working directory - rule 11
  run/agent/                   an agent's sandbox - rule 11, wipe freely
```

Three things worth knowing about that layout:

**`notes/`, not `docs/`.** `docs/` is MAME's Sphinx documentation tree. Writing
there collides with upstream and pollutes their build.

**`tools/` is free.** MAME has `src/tools/` but no top-level `tools/`, so the
name is ours without conflict.

**`projects/` is the wrong hook, despite the name.** It exists for standalone
out-of-tree targets: `PROJECT=x` sets `TARGET := x` (`makefile:237-240`),
replacing the mame target entirely rather than adding to it. Its `.gitignore` is
`*`, so nothing there is version controlled either. `[verified]`

### Where netplay code has to live, and why

`src/emu/netplay/` rather than a separate library, because the two things the
core must reach are both in `src/emu`: the input chokepoint in `ioport.cpp` and
the savestate machinery in `save.h`. A separate static lib would need a new
genie project, a new link edge in `main.lua`, and an include path into
`src/emu` anyway - three upstream edits instead of one.

The one unavoidable cost: `scripts/src/emu.lua` lists its 227 source files
explicitly and does not glob `[verified]`, so new files must be added there.
Keep them in one contiguous `NETPLAY:` block.

The relay server is a separate program with separate dependencies and goes in
`server/`. It is in this repo rather than a sibling so that one `CLAUDE.md`
governs both ends and `notes/protocol.txt` sits beside both implementations.

---

## Build configuration

Facts that govern every build command. Detail in `notes/build-config.txt`.

| | |
|---|---|
| Version | MAMEUI 0.210, upstream commit `83deae72` "0.210 Release files", 2019-05-29 |
| Toolchain | MSVC 2017 `v141`. `14.13.26128` (15.6) installed and works - `build-config.txt` 1 |
| Python | 3.10.5 on PATH. `makedep.py` and `makelist.py` are both py3-clean |
| OSD | `OSD=winui` (MAMEUI frontend). `OSD=messui` is the MESS variant |
| Driver set | `SUBTARGET=coinop` - arcade minus mechanical. `SUBTARGET=arcade` for stock |

```
make vs2017 OSD=winui TARGET=mame SUBTARGET=coinop PTR64=1 NOWERROR=1 \
     DEBUG_DIR=n:\DEV\MAMEUI DEBUG_ARGS=-window
```

Solution lands in `build/projects/winui/mamecoinop/vs2017/`. That command only
GENERATES the projects; it compiles nothing.

**To compile, use `tools\build.bat` - never a bare `msbuild`.** Rule 12: two
projects at a time, `/MP` off, and it refuses to start while another build is
running. A bare `msbuild /m` here spawns 46 `cl.exe`.

For netplay iteration use a `SOURCES=` build instead - it derives the needed
CPUs, sound and video devices from the named drivers, so it compiles in minutes
rather than hours:

```
make vs2017 OSD=winui TARGET=mame PTR64=1 NOWERROR=1 OPTIMIZE=0 \
     SOURCES=src/mame/drivers/cps1.cpp SUBTARGET=netplay
```

### Four toolchain quirks that will otherwise cost you a day

Named here so you know they exist; the evidence is in
`notes/build-config.txt` 3.

- **Precompiled headers are forcibly off for MSVC** and `PRECOMPILE` cannot
  override it. Every driver re-parses `emu.h`; this is why a full build takes
  hours and why the `SOURCES=` build above exists.
- **`NOWERROR=1` does not disable warnings-as-errors under MSVC.** Hence the
  project-scoped `/WX-` in `winui_cfg.lua`.
- **The winui entry point resolves to the wrong `main` under MSVC.** It links
  clean and runs as plain MAME with no frontend. There is no diagnostic.
- **Filter (`.flt`) comments must use `//`** - a leading `#` is an import
  directive.

## Netplay ground facts

Established by reading this tree. They are NOT repeated here - one fact, one
home (rule 7.1):

| | |
|---|---|
| where we hook MAME, and what upstream guarantees at each point | `notes/netplay-hooks.txt` |
| what perturbs emulated state, and the evidence | `notes/determinism.txt` |
| toolchain quirks with their evidence | `notes/build-config.txt` |
| how the trimmed driver set is derived | `notes/coinop-subtarget.txt` |
| the wire format | `notes/protocol.txt` - not written yet |

Three that change how you work and are worth knowing before you read any of
them:

- **MAME is deterministic for a fixed binary, config and ROM set.** The proof
  is that `-record`/`-playback` works. "Modern MAME is not deterministic" is
  false as stated. The hook that inherits this guarantee is
  `ioport_manager::frame_update`.
- **Frameskip is not presentation-only.** Several drivers derive save-stated
  state from it, so peers running different frameskip hold different machine
  state. Force it off and identical on all peers - `determinism.txt` 3.
- **Rollback primitives already exist** in `src/emu/save.h`: `ram_state` and
  `rewinder`. We are wiring a network source into machinery that is already
  there.

## The journal - deferred, deliberately

MOTORACER uses a gitignored SQLite journal (`tools/journal.py`, `.journal.db`)
because a 2300-function reverse-engineering backlog does not fit in a text file
that has to be read-modify-written on every append.

**We are not adopting it yet.** This project has one subsystem, not a backlog,
and `notes/` plus the rules above carry the load.

The trigger to adopt it is a **driver compatibility matrix**: once we are
recording per-driver whether savestates round-trip, whether rollback holds, and
which games desync and at what frame, that is exactly the per-row state a
journal is for and exactly the thing a text file corrupts. When that day comes,
`tools/journal.py` drops in from MOTORACER and `.journal.db` goes in the
gitignore.

Until then the rule is rule 7.1: **`notes/` is for facts, not for progress.**
If a sentence is only about the state of the work, it does not belong in a
document. Say it in chat and let it go.

---

## Working method

**Read before claiming.** This tree is 30,000 files and the answer is in it.
A grep costs seconds; a wrong recollection about MAME internals costs a desync
hunt.

**Anything run more than twice becomes a script in `tools/`,** with a header
comment saying what it does and how to run it. A measurement that cannot be
re-run is not a fact (rule 4).

**Prefer the narrow build.** Use `SOURCES=src/mame/drivers/cps1.cpp` for netplay
work. CPS1 is integer-only in the emulated path, two-player, save-state clean,
and it is the hardware already understood from the CDINO work - so nothing
extraneous muddies a desync.

**Smooth playback beats low input latency.** A stutter is worse than 200 ms of
lag. Where the two conflict, spend latency - this is a design principle, not a
default, and it settles arguments. Modern emulator netplay is tuned for
fighting games, where a lost input costs one move against an opponent who also
missed it. In a co-op game the shared state is a world full of AI reacting to
player input, so a lost input compounds instead of cancelling. The measured
arithmetic is `notes/protocol.txt` 1.6.3; the rationale is 1.6.4.

**Lockstep before rollback.** Get two instances bit-identical on a fixed input
delay first. Rollback reuses the same wire protocol and only changes client
speculation; building it first means debugging two unknowns at once.

**Desync detection comes before rollback, not after.** Without a per-frame state
hash exchanged and compared, every later bug is invisible.

**Keep an Open questions section in every note.** An honest gap is more useful
than a confident guess.

---

## Legal note

A personal interoperability project on legally-owned hardware and software. MAME
is BSD-3-Clause; the WINUI portion has its own licence in
`docs/winui_license.txt`. ROM images are not redistributed and are excluded
from version control. Our netplay code is our own work product.
