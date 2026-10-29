-- Run with an active Touch-controller action set and neutral baseline inputs.
local poses = {{-.35, 1.55, -.5}, {.4, 1.25, -.6}}
local triggers, sticks = {.25, .75}, {{.2, -.3}, {-.4, .5}}
for _, owner in pairs(simxr.state().owners or {}) do
    assert(not owner, "Release existing input owners before running this rig")
end
local function close(a, b) return math.abs(a - b) < .0001 end
local function observe()
    local capture = simxr.step()
    assert(capture, "Application did not submit a captured frame")
    local frame, inspection = simxr.inspect(capture)
    simxr.release_inspection(inspection)
    simxr.release_capture(capture)
    repeat
        local displayed, _, source = simxr.step_display()
        if displayed then simxr.release_capture(displayed) end
        if source and not simxr.frame_before(source, frame.frame_id) then break end
    until false
    return frame
end
local function matches(frame, released)
    if not frame.actions.available or not frame.actions.focused
        or tonumber(frame.actions.input_revision) == 0 then return false end
    for hand, side in ipairs({"left", "right"}) do
        local position = frame.tracking.controllers[hand].sample.pose.position
        if not released and not (close(position.x, poses[hand][1])
            and close(position.y, poses[hand][2]) and close(position.z, poses[hand][3])) then return false end
        local path = "/user/hand/" .. side
        for _, check in ipairs({
            {"/input/trigger/value", "scalar", if released then 0 else triggers[hand]},
            {"/input/thumbstick", "vector", if released then {0, 0} else sticks[hand]},
            {"/input/" .. (if hand == 1 then "x" else "a") .. "/click", "boolean", not released},
            {"/input/grip/pose", "active", true},
        }) do
            local found = false
            for _, action in ipairs(frame.actions.actions) do
                local bound = false
                for _, source in ipairs(action.sources) do
                    bound = bound or source.binding.source == path .. check[1]
                end
                if bound then
                    for _, value in ipairs(action.values) do
                        if value.subaction_path == path and value.active then
                            local actual, expected = value[check[2]], check[3]
                            local correct
                            if check[2] == "vector" then
                                correct = close(actual.x, expected[1]) and close(actual.y, expected[2])
                            elseif check[2] == "scalar" then
                                correct = close(actual, expected)
                            else
                                correct = actual == expected
                            end
                            if released and not correct then return false end
                            found = found or correct
                        end
                    end
                end
            end
            if not found then return false end
        end
        if released then
            local controls = frame.input.controllers[hand]
            if controls.trigger ~= 0 or controls.primary or controls.stick.x ~= 0 or controls.stick.y ~= 0 then
                return false
            end
        end
    end
    return true
end
local function checkFrames(name, target, released)
    for _ = 1, 16 do
        local frame = observe()
        if not simxr.frame_before(frame.frame_id, target) and matches(frame, released) then
            simxr.checkpoint(name, true, "frame " .. frame.frame_id)
            return
        end
    end
    simxr.checkpoint(name, false, "No matching synchronized actions within 16 frames")
end
simxr.pause()
local commands = {}
for hand, side in ipairs({"left", "right"}) do
    table.insert(commands, {kind="take_control", channel=side .. "_pose"})
    table.insert(commands, {kind="take_control", channel=side .. "_controls"})
    table.insert(commands, {kind="controller_pose", hand=hand-1, position=poses[hand]})
    table.insert(commands, {kind="controller_controls", hand=hand-1, trigger=triggers[hand],
        stick_x=sticks[hand][1], stick_y=sticks[hand][2], primary=true})
end
local target = simxr.frame_add(simxr.state().frame_id or "0", "1")
local transaction = simxr.schedule({frame=target, commands=commands})
checkFrames("Both hand poses and synchronized controller bindings", target, false)
simxr.await_transaction(transaction)
simxr.release_inputs()
target = simxr.frame_add(simxr.state().frame_id, "1")
checkFrames("Released controls and synchronized actions", target, true)
for _, owner in pairs(simxr.state().owners or {}) do
    assert(not owner, "Input ownership remained after release")
end
simxr.resume()
