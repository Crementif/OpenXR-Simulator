-- Watch recent frames for flicker and print each detection. Copy and tune the thresholds for your application.
-- TARGET "composed" checks the final eye images; "quad_layers" checks only the UI quad regions.
local TARGET = "composed"
local DURATION = 60 -- seconds; keep below the run deadline
local INTERVAL = 0.25 -- seconds between checks
local HISTORY = 8 -- recent frames read per check

local function luma(pixels, offset)
    local red, green, blue = buffer.readu8(pixels, offset), buffer.readu8(pixels, offset + 1), buffer.readu8(pixels, offset + 2)
    return (54 * red + 183 * green + 19 * blue) // 256
end

-- Reduce one sample to per-eye luma and brightness metrics.
local function measure(sample)
    local eyes = {}
    for index, eye in sample.eyes do
        local count = eye.width * eye.height
        if count == 0 then
            return nil
        end
        local values, sum, bright = buffer.create(count), 0, 0
        local position = 0
        for y = eye.y, eye.y + eye.height - 1 do
            local row = y * sample.width
            for x = eye.x, eye.x + eye.width - 1 do
                local value = luma(sample.pixels, (row + x) * 4)
                buffer.writeu8(values, position, value)
                position += 1
                sum += value
                if value >= 245 then
                    bright += 1
                end
            end
        end
        eyes[index] = {values = values, count = count, mean = sum / count / 255, bright = bright / count}
    end
    return {eyes = eyes, mean = (eyes[1].mean + eyes[2].mean) / 2}
end

local function sameShape(first, second)
    return first.eyes[1].count == second.eyes[1].count and first.eyes[2].count == second.eyes[2].count
end

-- Mean absolute luma change per eye, in [0, 1].
local function difference(first, second)
    local deltas = {}
    for index = 1, 2 do
        local a, b, total = first.eyes[index].values, second.eyes[index].values, 0
        for position = 0, first.eyes[index].count - 1 do
            total += math.abs(buffer.readu8(a, position) - buffer.readu8(b, position))
        end
        deltas[index] = total / first.eyes[index].count / 255
    end
    local count = first.eyes[1].count + first.eyes[2].count
    local both = (deltas[1] * first.eyes[1].count + deltas[2] * first.eyes[2].count) / count
    return deltas[1], deltas[2], both
end

local previous, older -- the last two consecutive frames
local lastFrame, visible, brightRun = nil, 0, 0
local detections, printed = 0, 0

local function reset()
    previous, older, visible, brightRun = nil, nil, 0, 0
end

local function report(frame, reason)
    detections += 1
    if printed < 200 then
        printed += 1
        print(reason .. " at frame " .. frame)
    end
end

local function observe(frame, current)
    local reason
    if TARGET == "composed" then
        local high, low = math.max(current.eyes[1].bright, current.eyes[2].bright), math.min(current.eyes[1].bright, current.eyes[2].bright)
        if high >= 0.001 and high - low >= 0.0009 and low <= high * 0.35 then
            brightRun += 1
            if brightRun == 2 then
                reason = "ASYMMETRIC_BRIGHT_BREAKTHROUGH"
            end
        else
            brightRun = 0
        end
    end
    if previous and sameShape(current, previous) then
        local left, right, both = difference(current, previous)
        local flash = math.max(left, right) > 0.12 and math.abs(left - right) > 0.08
        local function alternating(threshold, settled)
            if not (older and sameShape(current, older) and both > threshold) then
                return false
            end
            local _, _, back = difference(current, older)
            return back < settled
        end
        if reason then
            -- The bright breakthrough already explains this frame.
        elseif TARGET == "composed" then
            local isBlank = current.mean < 0.015 or current.mean > 0.985
            local wasBlank = previous.mean < 0.015 or previous.mean > 0.985
            if isBlank and not wasBlank and visible >= 5 then
                reason = "VISIBLE_TO_BLANK_FRAME"
            elseif flash then
                reason = "ASYMMETRIC_EYE_FLASH"
            elseif alternating(0.08, 0.03) then
                reason = "ALTERNATING_VISIBLE_FRAMES"
            end
        elseif alternating(0.055, 0.025) then
            reason = "ALTERNATING_UI_REGION"
        elseif flash then
            reason = "ASYMMETRIC_UI_EYE_FLASH"
        end
    end
    if current.mean >= 0.015 and current.mean <= 0.985 then
        visible += 1
    end
    if reason then
        report(frame, reason)
    end
    previous, older = current, previous
end

local options = {target = TARGET}
local stop = simxr.monotonic_time() + DURATION
while simxr.monotonic_time() < stop do
    local history = simxr.image_history(HISTORY, options)
    for _, entry in history.entries do
        local frame = entry.frame_id
        if lastFrame and not simxr.frame_before(lastFrame, frame) then
            continue
        end
        if lastFrame and simxr.frame_add(lastFrame, 1) ~= frame then
            reset() -- unsampled frames break the temporal comparison
        end
        lastFrame = frame
        local current
        if entry.phase == "ready" then
            -- UI quads are cropped to their own rectangles so small panels keep native detail.
            local sample = simxr.sample_history_image(frame, {target = TARGET, rectangles = entry.quad_rectangles})
            current = sample and measure(sample)
        end
        if current then
            observe(frame, current)
        else
            reset() -- evicted, pending, failed or missing an eye
        end
    end
    simxr.wait(math.ceil(INTERVAL * 1000))
end
simxr.checkpoint("No flicker detected", detections == 0, detections .. " detections")
