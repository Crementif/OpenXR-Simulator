simxr.pause()
for _, unavailable in {true, false} do
    local frame = simxr.frame_add(simxr.state().frame_id or "0", "1")
    local transaction = simxr.schedule({frame=frame, commands={
        {kind="take_control",channel="gaze"},
        {kind="scenario",scenario="gaze_unavailable",enabled=unavailable},
    }})
    while simxr.frame_before(simxr.state().frame_id or "0", frame) do
        local capture = simxr.step()
        if capture then simxr.release_capture(capture) end
        local applied = simxr.state().frame_id
        repeat
            local displayCapture, progress, source = simxr.step_display()
            if displayCapture then simxr.release_capture(displayCapture) end
        until source and not simxr.frame_before(source, applied)
    end
    simxr.await_transaction(transaction)
    simxr.checkpoint("gaze availability transition", simxr.state().configuration.gaze.available == not unavailable)
end
simxr.release_inputs()
simxr.resume()
