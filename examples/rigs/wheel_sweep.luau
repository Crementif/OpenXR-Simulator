-- Positions are simulation-space metres; angles are radians. Speed follows application frames.
local config = {
    center = {0, 1.35, -0.45}, radius = 0.18,
    tilt = math.rad(20), amplitude = math.rad(80), spread = math.rad(90), grip = 1,
    framesPerCycle = 120, cycles = 1,
}

simxr.pause()
for sample = 0, config.framesPerCycle * config.cycles do
    local angle = config.amplitude * math.sin(2 * math.pi * sample / config.framesPerCycle)
    local sx, cx = math.sin(-config.tilt / 2), math.cos(-config.tilt / 2)
    local sz, cz = math.sin(angle / 2), math.cos(angle / 2)
    local commands = {}
    for hand = 0, 1 do
        local side = if hand == 0 then "left" else "right"
        local bearing = (if hand == 0 then -config.spread else config.spread) + angle
        table.insert(commands, {kind="take_control", channel=side .. "_pose"})
        table.insert(commands, {kind="take_control", channel=side .. "_controls"})
        table.insert(commands, {kind="controller_pose", hand=hand, position={
            config.center[1] + config.radius * math.sin(bearing),
            config.center[2] + config.radius * math.cos(bearing) * math.cos(config.tilt),
            config.center[3] - config.radius * math.cos(bearing) * math.sin(config.tilt),
        }, orientation={sx*cz, -sx*sz, cx*sz, cx*cz}})
        table.insert(commands, {kind="controller_controls", hand=hand, grip=config.grip})
    end
    local frame = simxr.frame_add(simxr.state().frame_id or "0", "1")
    local transaction = simxr.schedule({frame=frame, commands=commands})
    simxr.advance_to_frame(frame)
    assert(simxr.await_transaction(transaction).applied_frame == frame)
end
simxr.release_inputs()
simxr.resume()
