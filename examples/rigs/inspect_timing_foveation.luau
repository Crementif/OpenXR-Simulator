-- Inspect actual query evidence and submitted timing without claiming which query the game used.
simxr.pause()
local capture = simxr.step()
assert(capture, "No submitted frame was captured")
local frame, inspection = simxr.inspect(capture)
local views = simxr.revision(frame.view_settings)
simxr.checkpoint("prediction identity", frame.prediction.frame_id == frame.frame_id)
simxr.checkpoint("positive predicted display period", tonumber(frame.prediction.period) > 0)
print("frame " .. frame.frame_id .. " display time " .. frame.submission.display_time)
print("view layout " .. views.view_layout .. "; FB foveation: " .. frame.availability.fb_foveation)
if frame.view_query then
    local query = frame.view_query
    simxr.checkpoint("query time matches submitted frame", query.display_time == frame.submission.display_time)
    simxr.checkpoint("query correlation is explicit", query.correlation == "last_successful_query_at_submission_time")
    print("foveated query " .. tostring(query.foveated_requested) .. "; gaze active " .. tostring(query.gaze_active))
else
    print("No matching view query was retained for this submitted frame")
end
simxr.release_inspection(inspection)
simxr.release_capture(capture)
simxr.resume()
