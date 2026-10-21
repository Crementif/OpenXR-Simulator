-- Run against an application that keeps pumping frames during tracking/focus loss.
-- This changes simulator inputs and session visibility; it does not reset a game.
simxr.pause()
local function apply(commands)
    local target = simxr.frame_add(simxr.state().frame_id or "0", "1")
    local transaction = simxr.schedule({frame=target, commands=commands})
    while simxr.frame_before(simxr.state().frame_id or "0", target) do
        local capture = simxr.step()
        local applied = simxr.state().frame_id
        if capture then simxr.release_capture(capture) end
        repeat
            local displayCapture, progress, source = simxr.step_display()
            if displayCapture then simxr.release_capture(displayCapture) end
        until source and not simxr.frame_before(source, applied)
    end
    simxr.await_transaction(transaction)
    return simxr.state().configuration
end
local function scenario(name, enabled)
    return apply({{kind="scenario",scenario=name,enabled=enabled}})
end
local state = apply({
    {kind="take_control",channel="head"},
    {kind="take_control",channel="left_pose"},
    {kind="take_control",channel="body"},
    {kind="take_control",channel="gaze"},
    {kind="take_control",channel="trackers"},
    {kind="take_control",channel="session"},
    {kind="head_pose",position={0.4,1.7,-0.3}},
    {kind="body",preset=2,confidence=0.9},
    {kind="gaze",yaw=0.2,pitch=-0.1,available=true,tracked=true},
    {kind="tracker",index=0,identity="1",role=0,connected=true,tracked=true,position={1,1,-1}},
})
simxr.checkpoint("body gaze tracker applied", state.body.preset == "2"
    and math.abs(state.gaze.yaw - 0.2) < 0.0001
    and state.trackers[1].pose_override.position[1] == 1)
state = scenario("head_untracked", true)
simxr.checkpoint("head tracking lost with valid pose", state.head.flags == "3")
state = scenario("head_untracked", false)
simxr.checkpoint("head tracking recovered", state.head.flags == "15")
state = scenario("gaze_unavailable", true)
simxr.checkpoint("gaze unavailable", not state.gaze.available)
state = scenario("gaze_unavailable", false)
simxr.checkpoint("gaze recovered", state.gaze.available and state.gaze.tracked)
state = scenario("left_disconnected", true)
simxr.checkpoint("controller disconnected", not state.controllers[1].connected)
state = scenario("left_disconnected", false)
simxr.checkpoint("controller reconnected", state.controllers[1].connected)
state = scenario("unfocused", true)
simxr.checkpoint("focus lost", not state.focused)
state = scenario("unfocused", false)
simxr.checkpoint("focus recovered", state.focused)
state = scenario("recenter", true)
simxr.checkpoint("recenter applied", math.abs(state.reference_spaces["local"].position[1] - 0.4) < 0.0001)
scenario("recenter", false)
state = scenario("hidden", true)
simxr.checkpoint("hidden session loses focus", not state.focused)
state = scenario("hidden", false)
simxr.checkpoint("visible session restores focus", state.focused)
simxr.release_inputs()
simxr.resume()
