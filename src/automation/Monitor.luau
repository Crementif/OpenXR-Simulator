-- The host supplies target, interval and timeout. Only native code reads pixels.
local nextSample = simxr.monotonic_time()
while true do
    local deadline = simxr.monotonic_time() + timeout
    local sample = simxr.analyze_image_history(8, {target = target})
    while sample.frames_analyzed + sample.frames_skipped == 0 do
        assert(simxr.monotonic_time() < deadline, "Monitoring history acquisition timed out")
        simxr.wait(10)
        sample = simxr.analyze_image_history(8, {target = target})
    end
    nextSample += interval
    local now = simxr.monotonic_time()
    if nextSample < now then
        local missed = math.floor((now - nextSample) / interval) + 1
        simxr.monitor_cadence(missed)
        nextSample += missed * interval
    end
    simxr.wait(math.ceil(math.max(0, nextSample - simxr.monotonic_time()) * 1000))
end
