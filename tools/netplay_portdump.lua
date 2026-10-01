-- license:BSD-3-Clause
-- copyright-holders:MAMEUI Netplay
--
-- TEST 1: the real per-frame input payload size, per driver.
--
-- Dumps every ioport and field, and computes the raw bytes that MAME's own
-- -record writes per frame. That figure is the authoritative wire-payload
-- baseline, because -record/-playback demonstrably reproduces a session, so
-- whatever record_port captures is provably sufficient.
--
-- Payload arithmetic, read out of ioport.cpp record_port / record_frame:
--
--     per port           defvalue + digital, ioport_value = u32    8 bytes
--     per analog field   m_accum + m_previous + m_sensitivity,
--                        s32 each, plus m_reverse as u8           13 bytes
--     per frame          curtime.seconds() s32 + attoseconds s64  12 bytes
--
-- record_frame also writes a u32 speed_percent. playback_frame accumulates
-- it for statistics only and never feeds it back into machine state, so it
-- is NOT part of the payload and must never go on the wire.
--
-- The recorded curtime is not payload either - playback_frame compares it
-- against the live attotime and calls playback_end("Out of sync"). It is a
-- desync check, the same role our state hash plays. Counted separately.
--
-- frame_update applies record_port to every port in m_portlist with no
-- active/player filter, so nports below is the whole list including DSW and
-- config ports. Trimming to player-relevant ports is an optimisation to be
-- validated against this baseline, not assumed.
--
--     mamecoinopui.exe <driver> -autoboot_script tools/netplay_portdump.lua
--
-- Writes a CSV to the path in NETPLAY_PORTDUMP, default portdump.csv, and
-- prints the totals. Exits as soon as it has dumped, so it is cheap to run
-- across many drivers.
--
-- LIMITATION: ioport_field::type_class() is not bound in luaengine.cpp, and
-- field.player reads 0 for both player 1 and non-player fields, so this
-- cannot separate player inputs from coin/DSW/service with certainty. The
-- raw size does not depend on that classification. A (player, logical
-- input) -> (port, mask) table that does needs a C++ dump.

local BYTES_PER_PORT   = 8
local BYTES_PER_ANALOG = 13
local BYTES_PER_FRAME  = 12   -- the attotime sync check, not payload

local outpath = os.getenv("NETPLAY_PORTDUMP") or "portdump.csv"

local machine = manager:machine()
local driver = machine:system().name

local rows = {}
local nports, nfields, nanalog = 0, 0, 0

for tag, port in pairs(machine:ioport().ports) do
	nports = nports + 1
	local port_fields, port_analog = 0, 0
	for name, field in pairs(port.fields) do
		nfields = nfields + 1
		port_fields = port_fields + 1
		if field.is_analog then
			nanalog = nanalog + 1
			port_analog = port_analog + 1
		end
		-- field.type is an ioport_type enum; tonumber() rather than %d so a
		-- binding that hands it over as userdata cannot abort the dump
		rows[#rows + 1] = string.format("%s,%s,%d,%d,0x%08x,%s,%s,%d",
				tag, string.gsub(name, ",", ";"), field.player,
				tonumber(field.type) or -1,
				field.mask, tostring(field.is_analog),
				tostring(field.is_digital_joystick), field.way)
	end
	rows[#rows + 1] = string.format("%s,<PORT TOTAL>,0,0,0x00000000,,,%d",
			tag, port_fields)
end

table.sort(rows)

local payload = (nports * BYTES_PER_PORT) + (nanalog * BYTES_PER_ANALOG)

local f = io.open(outpath, "w")
if f then
	f:write("# driver=", driver, "\n")
	f:write("# ports=", nports, " fields=", nfields, " analog=", nanalog, "\n")
	f:write("# payload_bytes_per_frame=", payload, "\n")
	f:write("# synccheck_bytes_per_frame=", BYTES_PER_FRAME, "\n")
	f:write("port,field,player,type,mask,is_analog,is_digital_joystick,way\n")
	for _, r in ipairs(rows) do
		f:write(r, "\n")
	end
	f:close()
else
	print("portdump: CANNOT WRITE " .. outpath)
end

-- One line per driver, so a sweep can be grepped out of stdout alone.
print(string.format(
		"portdump: driver=%s ports=%d fields=%d analog=%d " ..
		"payload=%d B/frame synccheck=%d B/frame at60=%d B/s csv=%s",
		driver, nports, nfields, nanalog, payload, BYTES_PER_FRAME,
		payload * 60, outpath))

machine:exit()
