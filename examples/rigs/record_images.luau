-- Explicit real-time recording. Synchronous persistence may overrun a fast application.
-- For controlled recording/backpressure use examples/record-frame-images.py --mode controlled.
assert(simxr.image_recording().available, "Image recording is unavailable on this backend")
simxr.start_image_recording({capacity=2, frames="2"})
local persisted = 0
local previous = nil
while persisted < 2 do
    local recording = simxr.image_recording()
    assert(recording.phase ~= "overrun", "Recording overrun at frame " .. (recording.missed_frame or "unknown"))
    for _, identity in ipairs(recording.captures) do
        local capture = simxr.capture(identity)
        if capture.phase == "ready" then
            assert(capture.image_available and capture.frame_id, "Recorded frame has no retained image")
            if previous then assert(capture.frame_id == simxr.frame_add(previous, "1"), "Recording has a gap") end
            simxr.export(identity, "frame-" .. capture.frame_id .. ".rgba")
            local frame, inspection = simxr.inspect(identity)
            simxr.checkpoint("saved frame " .. capture.frame_id, frame.frame_id == capture.frame_id)
            simxr.release_inspection(inspection)
            simxr.release_capture(identity)
            persisted += 1
            previous = capture.frame_id
        else
            assert(capture.phase == "issued" or capture.phase == "reserved" or
                capture.phase == "begun" or capture.phase == "gpu_pending",
                "Recording capture failed: " .. capture.phase)
            break
        end
    end
end
local recording = simxr.image_recording()
simxr.checkpoint("finite recording stopped without overrun", recording.phase == "stopped" and recording.captured == "2")
simxr.stop_image_recording()
