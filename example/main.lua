-- example/main.lua
-- Minimal reference for streaming a .rwlpv. Copy StreamVideo.lua (from ../lua) into
-- your source, add the C from ../src to your build, host a .rwlpv somewhere, and set
-- HOST/CLIP below. This isn't a turn-key project (it needs your own content + build)
-- — it's the smallest honest example of the integration.

import "CoreLibs/graphics"
import "StreamVideo"

local gfx = playdate.graphics

local HOST = "your-host.example.com"   -- where your .rwlpv is served from (HTTPS)
local CLIP = "clips/demo.rwlpv"        -- path under that host
local CACHE = "/demo.rwlpv"            -- local Data path to Stream+Save into (optional)

StreamVideo.configure(HOST, 443, true)
-- StreamVideo.debug = true            -- uncomment for console logs

local started = false

function playdate.update()
	if not StreamVideo.available() then
		gfx.clear(); gfx.drawTextAligned("native engine missing", 200, 116, kTextAlignment.center)
		return
	end

	-- Prefer a saved copy if we already have one; otherwise stream (and save).
	if not started then
		started = true
		if playdate.file.exists(CACHE) then
			StreamVideo.playFile(CACHE)
		else
			StreamVideo.play(CLIP, CACHE, function(p) print("cached -> " .. p) end)
		end
	end

	StreamVideo.tick()

	gfx.clear(gfx.kColorBlack)
	local img = StreamVideo.image()
	if StreamVideo.isReady() and img then
		img:draw(0, 0)
	else
		gfx.setImageDrawMode(gfx.kDrawModeFillWhite)
		gfx.drawTextAligned("Buffering…", 200, 116, kTextAlignment.center)
		gfx.setImageDrawMode(gfx.kDrawModeCopy)
	end

	if StreamVideo.finished() then
		gfx.drawTextAligned("done", 200, 220, kTextAlignment.center)
	end
end

-- Tidy up if the player leaves this scene / the app loses focus.
function playdate.gameWillTerminate() StreamVideo.stop() end
function playdate.deviceWillSleep()   StreamVideo.stop() end
