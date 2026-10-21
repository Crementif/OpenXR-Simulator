-- Read already-retained GPU history while the application continues rendering.
local history = simxr.image_history()
local selected = nil
for index = #history.entries, 1, -1 do
    if history.entries[index].phase == "ready" then
        selected = history.entries[index]
        break
    end
end
assert(selected, "No ready historical image; render frames before running this rig")
simxr.export_history(selected.frame_id, "history-left.rgba", 0)
simxr.checkpoint("Historical image exported", true)
