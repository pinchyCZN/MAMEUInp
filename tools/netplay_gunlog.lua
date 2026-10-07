-- license:BSD-3-Clause
-- copyright-holders:MAMEUI Netplay
--
-- What value is an analog port actually delivering to the driver?
--
-- Reads every analog port once per frame and logs the value the game sees,
-- which is the output of analog_field::read() - so it reflects netplay
-- injection (analog_field::netplay_force) as well as local input. Running
-- this on both a single-player session and a netplay one answers "does
-- netplay deliver a different value than playing alone" without having to
-- guess from the screen.
--
-- Also tracks the min and max each port reaches, which is the number to
-- compare against the field's declared PORT_MINMAX.
--
--     mamecoinopui.exe opwolf3 -autoboot_script tools/netplay_gunlog.lua
--
-- NETPLAY_GUNLOG   output path, default gunlog.txt
-- NETPLAY_GUNSNAP  if set, save a screen snapshot every this many frames
-- NETPLAY_GUNEXIT  exit after this many frames (default: run until closed)
--
-- LIMITATION: this observes the PORT, not the cursor the game draws from
-- it. A restricted cursor with a full-range port is the game's own mapping;
-- a restricted port is ours or the field's declaration.

local outpath  = os.getenv("NETPLAY_GUNLOG") or "gunlog.txt"
local snapevery = tonumber(os.getenv("NETPLAY_GUNSNAP") or "0") or 0
local exitat   = tonumber(os.getenv("NETPLAY_GUNEXIT") or "0") or 0

local machine = manager:machine()
local driver = machine:system().name

-- Collect the analog ports once. A port holding an analog field is what we
-- want; its whole read() value is logged because an analog field is
-- normally the only thing in such a port.
local watch = {}
for tag, port in pairs(machine:ioport().ports) do
	local analog = false
	local mask, defval = 0, 0
	for name, field in pairs(port.fields) do
		if field.is_analog then
			analog = true
			mask = field.mask
			defval = field.defvalue
		end
	end
	if analog then
		watch[#watch + 1] = {
			tag = tag, port = port, mask = mask, defval = defval,
			lo = nil, hi = nil
		}
	end
end

table.sort(watch, function(a, b) return a.tag < b.tag end)

local f = io.open(outpath, "w")
if f then
	f:write("# driver=", driver, "\n")
	f:write("# analog ports=", #watch, "\n")
	for _, w in ipairs(watch) do
		f:write(string.format("# %s mask=%04x defvalue=%04x\n",
				w.tag, w.mask, w.defval))
	end
	f:write("frame")
	for _, w in ipairs(watch) do
		f:write(",", w.tag)
	end
	f:write("\n")
end

local frame = 0
local screens = {}
for _, s in pairs(machine.screens) do
	screens[#screens + 1] = s
end

emu.register_frame_done(function()
	frame = frame + 1

	local row = {}
	for _, w in ipairs(watch) do
		local v = w.port:read() & w.mask
		row[#row + 1] = v
		if (w.lo == nil) or (v < w.lo) then w.lo = v end
		if (w.hi == nil) or (v > w.hi) then w.hi = v end
	end

	-- Every frame would be 3600 lines a minute for nothing; the interesting
	-- thing is change, so log a row only when one appears, plus a heartbeat.
	if f then
		local changed = false
		if frame == 1 then changed = true end
		for i, w in ipairs(watch) do
			if w.last ~= row[i] then changed = true end
		end
		if changed or ((frame % 300) == 0) then
			f:write(tostring(frame))
			for _, v in ipairs(row) do
				f:write(string.format(",%02x", v))
			end
			f:write("\n")
			f:flush()
		end
	end
	for i, w in ipairs(watch) do
		w.last = row[i]
	end

	if (snapevery > 0) and ((frame % snapevery) == 0) and screens[1] then
		screens[1]:snapshot(string.format("%s_f%06d.png", driver, frame))
	end

	if (exitat > 0) and (frame >= exitat) then
		if f then
			f:write("# range reached\n")
			for _, w in ipairs(watch) do
				f:write(string.format("# %s lo=%02x hi=%02x span=%d\n",
						w.tag, w.lo or 0, w.hi or 0,
						(w.hi or 0) - (w.lo or 0)))
			end
			f:close()
			f = nil
		end
		machine:exit()
	end
end)
