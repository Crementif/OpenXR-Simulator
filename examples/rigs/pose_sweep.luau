-- Positions are simulation-space metres; angles are radians. Speed follows application frames.
local config = {
    position = {0, 1.7, 0},
    yaw = math.rad(30), pitch = math.rad(15), roll = math.rad(15),
    framesPerCycle = 120, cycles = 1,
}

simxr.pause()
for sample = 0, config.framesPerCycle * config.cycles do
    local phase = 2 * math.pi * sample / config.framesPerCycle
    local yaw = config.yaw * math.sin(phase) / 2
    local pitch = config.pitch * math.sin(2 * phase) / 2
    local roll = config.roll * math.sin(3 * phase) / 2
    local sy, cy = math.sin(yaw), math.cos(yaw)
    local sx, cx = math.sin(pitch), math.cos(pitch)
    local sz, cz = math.sin(roll), math.cos(roll)
    local frame = simxr.frame_add(simxr.state().frame_id or "0", "1")
    local transaction = simxr.schedule({frame=frame, commands={
        {kind="take_control", channel="head"},
        {kind="head_pose", position=config.position, orientation={
            cy*sx*cz + sy*cx*sz, sy*cx*cz - cy*sx*sz,
            cy*cx*sz - sy*sx*cz, cy*cx*cz + sy*sx*sz,
        }},
    }})
    simxr.advance_to_frame(frame)
    assert(simxr.await_transaction(transaction).applied_frame == frame)
end
simxr.release_inputs()
simxr.resume()
