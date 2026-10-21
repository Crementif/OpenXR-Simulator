simxr.pause()
local frame = simxr.frame_add(simxr.state().frame_id or "0", "1")
local transaction = simxr.schedule({frame=frame, commands={
    {kind="take_control",channel="body"},
    {kind="body",preset=2,confidence=0.9},
    {kind="take_control",channel="gaze"},
    {kind="gaze",yaw=0.2,pitch=-0.1,available=true,tracked=true},
    {kind="take_control",channel="trackers"},
    {kind="tracker",index=0,identity="1",role=0,connected=true,tracked=true},
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
simxr.checkpoint("seated body and gaze", simxr.state().configuration.body.preset == "2")
simxr.release_inputs()
simxr.resume()
