# OpenXR SDK provenance

All runtime and probe targets use the official Khronos OpenXR SDK release 1.1.65 declarations.

- Repository: https://github.com/KhronosGroup/OpenXR-SDK
- Tag: `release-1.1.65`
- Commit: `24a1f2b92d212501a2b1b8d30ffd53abf55fcfa8`
- Registry: `registry/xr.xml`, from `specification/registry/xr.xml` at the same commit
- Runtime API remains explicitly capped at OpenXR 1.0.27. Header declarations do not enable or advertise a runtime feature.

The registry records official extension revisions, dependencies, aliases, joint schemas and structure contexts. Do not recreate numeric declarations. Upgrade headers, reflection files and registry together. Files are exact upstream bytes, including original license notices.

| File | SHA-256 |
| --- | --- |
| `openxr.h` | `183fce845edd297790082fd23b49f78f023be6438eb529ae59e2af5fbc6d8ee4` |
| `openxr_loader_negotiation.h` | `ebcca0946b4b738ebee41943b821abe4df518b3a37766e0ce6883bcefce65124` |
| `openxr_platform.h` | `f17e6d752e0065ede9f2a4c8932155acff96462306a2468a67a6ad77484641d8` |
| `openxr_platform_defines.h` | `a458ba5777415f2de518fdcdcc87fcf8651ba0468c9f69a101378672aad80946` |
| `openxr_reflection.h` | `32c16139808f29751786361d748cbb4f169180ec8a89afd2d73d2568a45ba0f1` |
| `openxr_reflection_parent_structs.h` | `d600e3b8107f16671bd8d0d7a0fd5fb8e9e25f2f3f047cc73abef59995d7e600` |
| `openxr_reflection_structs.h` | `baaeaa330f1b0e5b58fbb09b6995058db18e3ac05dc22033de7d9182cb0dafce` |
| `registry/xr.xml` | `4b9c2d5b1c7e19f2be1e03c07d0c92c86986b5022d90270d0fdfa75308a697fd` |
