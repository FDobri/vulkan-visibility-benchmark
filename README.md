# Vulkan Renderer and Visibility Benchmark

A deterministic 3D field with CPU and GPU sphere-frustum culling paths, a shared indirect draw and Dear ImGui controls for the camera and workload.

## Build and run

Use CMake 3.24+, a C++20 compiler, a Vulkan 1.2 driver and the glslang compiler from the Vulkan SDK or your package manager. Linux builds require GLFW's X11 development packages. Pass `-DGLSLANG=/path/to/glslang` when the compiler is outside PATH and VULKAN_SDK.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVIS_GPU_TEST=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
./build/visibility
./build/visibility --benchmark > timings.csv
```

Use `build/Release/` with multi-config generators. The window lets you toggle culling paths, vary object counts and pause the orbit. Both `--smoke` and `--benchmark` still require a graphics session and create a hidden window.

## Design

GLM builds a Vulkan clip-space camera and six normalized frustum planes. Both paths test the same bounding spheres. The CPU writes visible IDs to mapped storage. The GPU path compacts surviving indices through an atomic counter directly into the shared instance list and indirect command, with a barrier from compute writes to indirect reads, vertex reads and host readback. Both draw the same additive billboards in one indirect instanced draw.
