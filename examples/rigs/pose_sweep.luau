-- Run against a connected simulator: simxr_runner --socket PATH --rig pose_sweep.luau --seed 0
-- This schedules simulator input; deterministic game simulation needs a cooperating game.
simxr.pause()
for sample = 0, 15 do
    local frame = tonumber(simxr.state().frame_id or "0") + 1
    local transaction = simxr.schedule({frame=frame, commands={
        {kind="take_control", channel="head"},
        {kind="head_pose", position={math.sin(sample * math.pi / 8) * 0.2, 1.7, 0}},
    }})
    while tonumber(simxr.state().frame_id or "0") < frame do
        local capture = simxr.step()
        if capture then simxr.release_capture(capture) end
    end
    local applied = simxr.await_transaction(transaction)
    simxr.checkpoint("head sample " .. sample, applied.applied_frame == tostring(frame))
end
simxr.release_inputs()
simxr.resume()
