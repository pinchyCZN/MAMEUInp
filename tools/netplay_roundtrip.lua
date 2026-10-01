-- license:BSD-3-Clause
-- copyright-holders:MAMEUI Netplay
--
-- TEST 2: does a savestate round-trip bit-identically?
--
-- The gate for rollback and for resync. We have measured savestate SIZES but
-- never that loading one reproduces the state that was saved. If it does not,
-- rollback and resync are both impossible and the design is lockstep-only.
--
-- Method. Run deterministically under frame-keyed input. Save at frame A,
-- keep running to frame B, then load. If save/load is exact, the hash log
-- from B onward must REPLAY the hashes from A onward:
--
--     hash[B + k] == hash[A + k]    for all k > 0
--
-- The input stream has to replay too, or the comparison is meaningless, so
-- the Lua frame counter is rewound to A at the load. Our hash's own frame
-- counter is not registered with save_manager, so it keeps counting up and
-- the log stays monotonic - that is what makes the two regions comparable.
--
-- Ordering, read out of machine.cpp running_machine::run(): handle_saveload()
-- runs after m_scheduler.timeslice(), and our hash is taken inside
-- ioport_manager::frame_update which is driven from within the timeslice. So
-- a save scheduled on frame A commits after frame A has been hashed.
--
-- LANDMINE: (LOAD_FRAME - SAVE_FRAME) MUST be a multiple of INPUT_PERIOD.
-- ioport live state - m_digital_value - is NOT registered with save_manager,
-- so a load does not restore the held input. If the two points sit at
-- different phases of the input pattern, the frame after the load runs with
-- the WRONG buttons held, writes them into work RAM, and the state diverges
-- for two frames before the game re-reads input and converges.
--
-- That looks exactly like an imperfect savestate and is not one. Measured on
-- dino: a matched phase matches every frame of the window (320/320,
-- 512/512), a mismatched phase loses exactly 3 (627/630, 485/488), and the
-- only differing entry is memory/:maincpu/0/00ff0000-00ffffff. The check
-- below enforces the multiple so the artifact cannot recur silently.
--
-- The offset is still DISCOVERED by netplay_rtcheck.py rather than assumed:
-- handle_saveload() returns without cancelling when m_scheduler.can_save()
-- is false, so the commit can in principle slip.
--
--     mamecoinopui.exe dino -autoboot_script tools/netplay_roundtrip.lua \
--         -netplay_hashlog rt.csv -netplay_hashinterval 1 \
--         -nothrottle -frameskip 0 -seconds_to_run 0
--
--     python tools/netplay_rtcheck.py rt.csv --save 600 --load 1200
--
-- hashinterval MUST be 1: the check is frame-by-frame.
-- frameskip MUST be 0 and identical to any run compared against - see
-- notes/determinism.txt 3.

-- Overridable so a sweep can vary the save/load point without editing this
-- file; netplay_rtcheck.py needs the same two numbers passed to --save/--load.
local SAVE_FRAME = tonumber(os.getenv("NETPLAY_RT_SAVE")) or 600
local LOAD_FRAME = tonumber(os.getenv("NETPLAY_RT_LOAD")) or 1200
local STOP_FRAME = tonumber(os.getenv("NETPLAY_RT_STOP")) or 1800
local SLOT       = os.getenv("NETPLAY_RT_SLOT") or "netplay_rt"

-- the period of drive() below. The gap between save and load must be a
-- whole number of these or the comparison is invalid; see LANDMINE above.
local INPUT_PERIOD = 64

local machine = manager:machine()

-- The exe links /SUBSYSTEM:WINDOWS (scripts/src/osd/winui.lua sets
-- /ENTRY:wWinMainCRTStartup), so print() output is discarded when MAME is
-- launched from a shell. Markers go to a file or they are not observable.
local EVENTS = os.getenv("NETPLAY_RT_EVENTS") or "rt_events.txt"
local evf = io.open(EVENTS, "w")

local function say(msg)
	print(msg)
	if evf then
		evf:write(msg, "\n")
		evf:flush()
	end
end

local function find_field(name)
	for _, port in pairs(machine:ioport().ports) do
		for fname, field in pairs(port.fields) do
			if fname == name then
				return field
			end
		end
	end
	return nil
end

local COIN  = find_field("Coin 1")
local START = find_field("1 Player Start")
local B1    = find_field("P1 Button 1")
local B2    = find_field("P1 Button 2")
local LEFT  = find_field("P1 Left")
local RIGHT = find_field("P1 Right")

local function press(field, down)
	if field then field:set_value(down and 1 or 0) end
end

-- Frame-keyed, never time-keyed: a wall-clock-driven script would make the
-- comparison meaningless. Must be a pure function of the frame number so
-- that rewinding the counter rewinds the input stream with it.
local function drive(frame)
	press(COIN,  frame >= 30 and frame < 36)
	press(START, frame >= 90 and frame < 96)
	if frame >= 180 then
		local phase = frame % INPUT_PERIOD
		press(B1,    phase >= 0  and phase < 6)
		press(B2,    phase >= 16 and phase < 22)
		press(LEFT,  phase >= 32 and phase < 44)
		press(RIGHT, phase >= 48 and phase < 60)
	end
end

local gap = LOAD_FRAME - SAVE_FRAME
if (gap % INPUT_PERIOD) ~= 0 then
	say(string.format(
			"roundtrip: REFUSING - gap %d is not a multiple of "
			.. "INPUT_PERIOD %d. The input phase would differ at the load "
			.. "and the result would be wrong by a few frames. Try a gap "
			.. "of %d.",
			gap, INPUT_PERIOD,
			INPUT_PERIOD * math.floor(gap / INPUT_PERIOD)))
	machine:exit()
	return
end

local FRAME = 0
local saved, loaded = false, false

emu.register_frame(function()
	FRAME = FRAME + 1
	drive(FRAME)

	if (FRAME == SAVE_FRAME) and not saved then
		saved = true
		machine:save(SLOT)
		say(string.format("roundtrip: SAVE scheduled, lua frame %d", FRAME))
	elseif (FRAME == LOAD_FRAME) and not loaded then
		loaded = true
		machine:load(SLOT)
		say(string.format("roundtrip: LOAD scheduled, lua frame %d", FRAME))
		-- rewind the input stream with the machine state
		FRAME = SAVE_FRAME
	elseif FRAME >= STOP_FRAME then
		say(string.format("roundtrip: stopping, lua frame %d", FRAME))
		machine:exit()
	end
end)

say(string.format(
		"roundtrip: driver=%s save=%d load=%d stop=%d slot=%s " ..
		"bindings coin=%s start=%s b1=%s b2=%s left=%s right=%s",
		machine:system().name, SAVE_FRAME, LOAD_FRAME, STOP_FRAME, SLOT,
		tostring(COIN ~= nil), tostring(START ~= nil), tostring(B1 ~= nil),
		tostring(B2 ~= nil), tostring(LEFT ~= nil), tostring(RIGHT ~= nil)))
