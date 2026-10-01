-- license:BSD-3-Clause
-- copyright-holders:MAMEUI Netplay
--
-- Deterministic synthetic input, for testing that identical input from an
-- identical starting state produces identical emulated state.
--
--     mamecoinopui.exe <driver> -autoboot_script tools/netplay_inputgen.lua \
--         -netplay_hashlog h1.csv -str 40 ...
--
-- Everything here is keyed off a FRAME COUNTER, never off wall-clock time
-- or machine().time(), so two runs produce byte-identical input streams.
-- That is the whole point: a time-based script would make the test
-- meaningless in exactly the way a cold-boot attract-mode run already is.
--
-- Input is pushed with field:set_value(), which lands in m_digital_value
-- and is OR'd into the port during ioport_field::frame_update - before
-- record_port runs, so -record captures it and the resulting .inp can be
-- replayed to exercise the external-input path as well.
--
-- NOTE on set_value: a pushed value bypasses the 4-way/8-way joystick
-- restriction that a polled value obeys (notes/netplay-hooks.txt 1.2).
-- Harmless here because both runs push identically, but it means this
-- script's input is not identical to what a human at the keyboard
-- produces.

local FRAME = 0

-- Fields are located by NAME rather than by port tag and mask, because
-- tags differ per driver and we want this to work unchanged on anything.
local function find_field(name)
	for _, port in pairs(manager:machine():ioport().ports) do
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

-- A fixed script. Coin and start first so the machine leaves attract mode,
-- then a repeating pattern that keeps the game logic busy rather than
-- idling - the point is to exercise state, not to play well.
emu.register_frame(function()
	FRAME = FRAME + 1

	press(COIN,  FRAME >= 30  and FRAME < 36)
	press(START, FRAME >= 90  and FRAME < 96)

	if FRAME >= 180 then
		local phase = FRAME % 64
		press(B1,    phase >= 0  and phase < 6)
		press(B2,    phase >= 16 and phase < 22)
		press(LEFT,  phase >= 32 and phase < 44)
		press(RIGHT, phase >= 48 and phase < 60)
	end
end)

-- Report what was actually bound, so a silent "field not found" cannot
-- masquerade as a passing test.
print(string.format(
	"inputgen: coin=%s start=%s b1=%s b2=%s left=%s right=%s",
	tostring(COIN ~= nil), tostring(START ~= nil), tostring(B1 ~= nil),
	tostring(B2 ~= nil), tostring(LEFT ~= nil), tostring(RIGHT ~= nil)))
