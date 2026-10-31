# Building

Install a C++20 compiler, CMake 3.24 or newer, Ninja and the system libraries below.

Ubuntu or Debian:

```sh
sudo apt install build-essential cmake ninja-build git curl zip unzip tar pkg-config libvulkan-dev \
  libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxrender-dev libxtst-dev \
  libxss-dev libxkbcommon-dev libwayland-dev wayland-protocols libegl-dev libgles-dev libudev-dev \
  libdbus-1-dev libusb-1.0-0-dev libdecor-0-dev libdrm-dev libgbm-dev libibus-1.0-dev libthai-dev libfribidi-dev
```

Fedora:

```sh
sudo dnf install gcc-c++ cmake ninja-build git curl zip unzip tar pkgconf vulkan-loader-devel \
  libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXi-devel libXfixes-devel libXrender-devel \
  libXtst-devel libXScrnSaver-devel libxkbcommon-devel wayland-devel wayland-protocols-devel libglvnd-devel \
  systemd-devel dbus-devel libusb1-devel libdecor-devel libdrm-devel mesa-libgbm-devel ibus-devel \
  libthai-devel fribidi-devel
```

Windows needs Visual Studio 2022 with the C++ workload. Then clone and build:

```sh
git clone --recursive --branch rework https://github.com/Crementif/OpenXR-Simulator
cd OpenXR-Simulator
cmake --preset release
cmake --build --preset release
```

The release source tarball already includes the submodule; extract it and run the same two `cmake` commands. Binaries and the runtime manifest land in `out/build/release/bin`. Set `XR_RUNTIME_JSON` to that manifest to use the runtime from a process.
