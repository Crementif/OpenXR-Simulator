# OpenXR Simulator

Launch and inspect OpenXR applications without a headset. The desktop app provides a stereo preview, simulated devices, frame inspection, input recording and replay. Native MCP and Luau provide automation.

## Install and launch

Download a package from [releases](https://github.com/Crementif/OpenXR-Simulator/releases). Windows users can run the setup executable or extract the portable archive and launch `bin/simxr_companion.exe`. On Linux, extract the archive and run `install.sh` or `bin/simxr_companion`. A Vulkan driver and loader are required on both platforms.

Open the app, choose **Add application**, select its executable, then choose **Launch**. The bundled runtime is selected for new profiles; launching does not change the system's default OpenXR runtime. Use **Connect** for an already-running simulator application. Input replay does not restore game state.

For source builds, see [BUILDING.md](docs/BUILDING.md). Manual testing, Luau rigs, the Python SDK and MCP are described in [AUTOMATION.md](docs/AUTOMATION.md).

## Automation

Under **Preferences → MCP**, copy a Generic JSON or Codex configuration into your client. The native connector starts or connects to the app. Automation clients can also use the [Python SDK](sdk/python).

## Development

The [work queue](EXECUTION_PLAN.md) tracks remaining work; [architecture](REFACTOR_PLAN.md), [UI guide](docs/UI_IMPLEMENTATION_GUIDE.md) and [contributor guide](docs/CODING_STYLE.md) describe project conventions. This is a development preview.

[MIT License](LICENSE). Bundled dependencies retain their own licenses.
