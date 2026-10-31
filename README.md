# OpenXR Simulator

Run and test OpenXR applications on your desktop without a VR headset. Control a simulated headset and controllers with a keyboard, mouse or gamepad, watch the stereo preview, and inspect what the application renders.

## What you can do

- Launch applications with their own runtime settings and keep several applications open in separate tabs.
- Move simulated devices, adjust headset settings, and try different poses and inputs.
- Pause and step frames, browse recent frame history, and export images for inspection.
- Save input sequences as takes, replay them, and turn them into editable scripts for repeatable tests.
- Control applications from an MCP client or automate them with Luau and Python.

## Getting started

Follow the [build instructions](docs/BUILDING.md) to build this branch from source. After a release build, launch:

- **Windows:** `out/build/release/bin/simxr_companion.exe`
- **Linux:** `out/build/release/bin/simxr_companion`

Both platforms require a Vulkan driver and loader for the desktop app.

1. Choose **Add application** and select the OpenXR application's executable.
2. Choose **Launch**. New application profiles use the bundled simulator runtime without changing your system's default OpenXR runtime.
3. Open **Pilot keys** in the preview to see the keyboard, mouse and gamepad controls. Use the device inspector to adjust poses and settings precisely.

Use **Connect** to attach to an application already running with the simulator runtime.

## Inspect and replay

The timeline lets you look back through recent frames while the application keeps running. Pause and step when you need to inspect individual frames.

To repeat an input sequence, select a timeline range and choose **Save as take**. In the Library, **Export editable rig** turns a take into a Luau script you can edit and run again.

Replay restores simulator input, not game state. Start the application from the same point for each run, or use the optional game bridge to reset a cooperating application.

## Automation

Luau scripts can schedule input, capture images and check results. The Python SDK provides access from external tools. To use an AI assistant, open **Preferences → MCP** and copy the configuration into your MCP client.

See [automation and debugging](docs/AUTOMATION.md) for setup and examples, or browse the [example scripts](examples/rigs).

## Compatibility

This is a development preview implementing OpenXR 1.1. Application compatibility depends on the graphics binding and extensions the application requires.

| Platform | Application graphics bindings |
| --- | --- |
| Windows | Direct3D 11, Direct3D 12, OpenGL, Vulkan |
| Linux | Vulkan |

Physical headset support is experimental and requires a compatible OpenXR runtime for the headset.

## Contributing

See the [build instructions](docs/BUILDING.md) and [contributor guide](docs/CODING_STYLE.md).

Based on [elliotttate/OpenXR-Simulator](https://github.com/elliotttate/OpenXR-Simulator). Released under the [MIT License](LICENSE); bundled dependencies retain their own licenses.
