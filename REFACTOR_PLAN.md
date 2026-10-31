# Architecture

The runtime and companion share one compositor, shader source set and submission policy. Native Vulkan, D3D11, D3D12 and OpenGL adapters own device operations and synchronization; the companion uses Vulkan. Normal preview and local inspection remain GPU-resident. Explicit exports perform readback.

Applications can continue independently of the companion. Runtime selection is per application and must not change the system default.

UI, MCP and scripts use shared typed application operations. MCP translates at the external boundary. Automation uses canonical scheduled commands; scripts define scenarios and policy while native code owns exact-frame application, capture and resource lifetimes.

Automatic monitoring uses Luau orchestration outside XR/render threads. Native code performs bounded sampling, image reduction and analysis; scripts consume compact results rather than processing pixels. Configured monitors have a background owner independent of UI/MCP connections. Slow monitoring must never stall rendering: bound CPU work, queues and retained images, report gaps, and cancel cleanly. Observation does not implicitly pause or step the application.

The existing native headset host connects to a physical OpenXR runtime and exchanges input, timing, haptics and GPU output with the simulator. The application continues to use the simulator; the companion owns the host and selects its physical runtime without changing system registration. This bridge is the device-driven capture path. Record the inputs actually applied, physical view/calibration context and correlated game-SDK observations for editable, headset-free replay. Game-state restoration requires explicit game support. Physical gameplay and provider qualification remain separate from software implementation.
