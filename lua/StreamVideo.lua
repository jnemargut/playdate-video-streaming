-- StreamVideo.lua
-- A small Lua front-end for the native 1-bit video+audio streaming engine
-- (src/streamvideo.c). It does the HTTP GET, then feeds the .rwlpv bytes to C as
-- they arrive — strictly in order, and only while the engine has room (roomC) so a
-- fast network can't overrun the rings. The C side decodes the audio, syncs the
-- video to the audio clock, and blits each frame into an image you draw yourself.
--
-- This file is dependency-free: call StreamVideo.configure() once, give play() an
-- image to draw into, and call tick() every frame. See README for a full example.
--
--   Stream:        StreamVideo.play("path/clip.rwlpv")
--   Stream + save: StreamVideo.play("path/clip.rwlpv", "/Data/clip.rwlpv")
--   Replay cached: StreamVideo.playFile("/Data/clip.rwlpv")

StreamVideo = {}
StreamVideo.debug = false

local C = (streamvideo ~= nil and streamvideo.startC ~= nil) and streamvideo or nil
local HOST, PORT, SSL = nil, 443, true

local conn, fileReader, active, leftover, headersOk, mode = nil, nil, false, nil, false, nil
local outFile, cacheDest, doCache, cacheOnly, cacheCb = nil, nil, false, false, nil
local img = nil
local bytesDown, bytesTotal = 0, 0
local READ_CAP = 16384

local function log(s) if StreamVideo.debug then print("SV: " .. s) end end

-- true only if the native engine is fully present (guards against a stale build).
function StreamVideo.available()
	return C ~= nil and C.roomC ~= nil and C.feedC ~= nil and C.finalizeC ~= nil
end

-- Point the streamer at your server. Call once at startup.
function StreamVideo.configure(host, port, useSSL)
	HOST, PORT, SSL = host, port or 443, useSSL ~= false
end

-- Supply the image frames are blitted into (created 400x240 if you don't). Draw it
-- yourself every frame, e.g. StreamVideo.image():draw(0, 0).
function StreamVideo.setImage(image) img = image end
function StreamVideo.image() return img end

function StreamVideo.isCaching() return active and outFile ~= nil and doCache end
function StreamVideo.progress() return bytesDown, bytesTotal end

local function feed(buf)
	local accepted = C.feedC(buf)
	if accepted < #buf then leftover = string.sub(buf, accepted + 1) end
end

local function closeCache(keep)
	if not outFile then return end
	pcall(function() outFile:close() end); outFile = nil
	if keep and doCache and cacheDest then
		playdate.file.delete(cacheDest); playdate.file.rename(cacheDest .. ".part", cacheDest)
		if cacheCb then cacheCb(cacheDest) end       -- tell the app a cached file is ready
	elseif cacheDest then
		playdate.file.delete(cacheDest .. ".part")
	end
end

local function pumpHttp(c)
	if not C or not c then return end
	if cacheOnly then                      -- detached: drain to cache at full speed, no decode
		while true do
			local avail = c:getBytesAvailable() or 0
			if avail <= 0 then break end
			local buf = c:read(avail < 32768 and avail or 32768)
			if not buf or #buf == 0 then break end
			if outFile then outFile:write(buf) end
			bytesDown = bytesDown + #buf
		end
		return
	end
	if not headersOk then return end
	if leftover then local p = leftover; leftover = nil; feed(p); if leftover then return end end
	while C.roomC() do
		local avail = c:getBytesAvailable() or 0
		if avail <= 0 then break end
		local buf = c:read(avail < READ_CAP and avail or READ_CAP)
		if not buf or #buf == 0 then break end
		if outFile then outFile:write(buf) end
		feed(buf)
		bytesDown = bytesDown + #buf
		if bytesTotal == 0 then local _, t = c:getProgress(); if t and t > 0 then bytesTotal = t end end
		if leftover then break end          -- ring full mid-chunk: resume next tick
	end
end

local function pumpFile()
	if not C or not fileReader then return end
	if leftover then local p = leftover; leftover = nil; feed(p); if leftover then return end end
	while C.roomC() do
		local buf = fileReader:read(READ_CAP)
		if not buf or #buf == 0 then
			pcall(function() fileReader:close() end); fileReader = nil
			C.finalizeC(); break                -- EOF: let it play out, then report finished
		end
		feed(buf)
		if leftover then break end
	end
end

-- Stream /path over HTTP(S). If cachePath is given, the bytes are also written to
-- disk (Stream+Save); onCached(path) fires when the cache completes.
function StreamVideo.play(path, cachePath, onCached)
	if not StreamVideo.available() or not HOST then return false end
	StreamVideo.stop()
	active, leftover, headersOk, mode, cacheOnly = true, nil, false, "http", false
	doCache, cacheDest, cacheCb, outFile = (cachePath ~= nil), cachePath, onCached, nil
	bytesDown, bytesTotal = 0, 0
	if not img then img = playdate.graphics.image.new(400, 240, playdate.graphics.kColorBlack) end
	C.startC(); C.setTargetC(img)

	local c = playdate.network.http.new(HOST, PORT, SSL, "stream video")
	conn = c
	if not c then StreamVideo.stop(); return false end
	c:setConnectTimeout(10)
	c:setHeadersReadCallback(function()
		if not active then return end
		local status = c:getResponseStatus() or 0
		log("headers status=" .. status)
		if status >= 200 and status < 300 then
			headersOk = true
			if doCache and cacheDest then outFile = playdate.file.open(cacheDest .. ".part", playdate.file.kFileWrite) end
		end
	end)
	c:setRequestCallback(function() if active then pumpHttp(c) end end)
	c:setRequestCompleteCallback(function()
		if not active then return end
		local err = c:getError()
		if err and err ~= "Connection closed" then log("error " .. tostring(err)); closeCache(false); return end
		local status = c:getResponseStatus() or 0
		closeCache(status >= 200 and status < 300)
		if cacheOnly then conn = nil; active = false; cacheOnly = false
		elseif status >= 200 and status < 300 then C.finalizeC() end
	end)
	c:setConnectionClosedCallback(function() end)
	local ok = c:get("/" .. path)
	log("get('/" .. path .. "') ok=" .. tostring(ok))
	if not ok then StreamVideo.stop(); return false end
	return true
end

-- Replay a cached .rwlpv straight from disk (no network).
function StreamVideo.playFile(localPath)
	if not StreamVideo.available() then return false end
	StreamVideo.stop()
	active, leftover, mode, cacheOnly = true, nil, "file", false
	doCache, outFile, cacheDest = false, nil, nil
	if not img then img = playdate.graphics.image.new(400, 240, playdate.graphics.kColorBlack) end
	C.startC(); C.setTargetC(img)
	fileReader = playdate.file.open(localPath, playdate.file.kFileRead)
	if not fileReader then StreamVideo.stop(); return false end
	return true
end

-- Call once per frame from playdate.update().
function StreamVideo.tick()
	if not C or not active then return end
	if mode == "http" then if conn then pumpHttp(conn) end
	elseif mode == "file" then pumpFile() end
	C.tickC()
end

-- Leave the player but finish writing the cache in the background (stop decoding).
function StreamVideo.detach()
	if not active or not (outFile and doCache) then StreamVideo.stop(); return end
	cacheOnly, leftover = true, nil
	if C then pcall(function() C.stopC() end) end
end

function StreamVideo.finished()  return (C and C.isFinishedC and C.isFinishedC()) == true end
function StreamVideo.isReady()   return (C and active and C.isReadyC()) == true end
function StreamVideo.isPlaying() return (C and active and not cacheOnly and C.isPlayingC()) == true end
function StreamVideo.isActive()  return active end
function StreamVideo.posSeconds() return (C and C.posSecondsC and C.posSecondsC()) or 0 end

function StreamVideo.stop()
	local c = conn; conn = nil
	active, leftover, headersOk, mode, cacheOnly = false, nil, false, nil, false
	if c then pcall(function() c:close() end) end
	if fileReader then pcall(function() fileReader:close() end); fileReader = nil end
	closeCache(false)
	if C then pcall(function() C.stopC() end) end
end

return StreamVideo
