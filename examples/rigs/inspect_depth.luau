-- Run with simxr_runner --socket PATH --rig inspect_depth.luau --json report.json --junit report.xml
-- Requires a submitted, sampleable D32_SFLOAT projection depth view at layer 0/view 1.
-- Writes explicit normalized grayscale RGBA bytes to depth.rgba in the runner's directory.
simxr.pause()
local target = {target="layer_depth", layer=0, view=1, array_slice=0}
local capture = simxr.step(target)
assert(capture, "The admitted frame has no retained depth image")
local frame, inspection = simxr.inspect(capture)
local views = simxr.revision(frame.view_settings)
simxr.checkpoint("exact retained depth", frame.layers.layers[1].views[2].depth ~= nil)
simxr.checkpoint("immutable view configuration", views.ipd > 0)
simxr.export(capture, "depth.rgba", target)
simxr.release_capture(capture)
-- CPU inspection ownership is independent of the GPU capture.
assert(simxr.revision(frame.view_settings).ipd == views.ipd)
simxr.release_inspection(inspection)
simxr.resume()
