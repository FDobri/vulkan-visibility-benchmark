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

VMA owns the mapped buffers. The application waits for each submission before reusing those buffers. This makes frame samples easy to compare and excludes overlap between frames. CPU timing captures culling and command recording for the visibility phase. Vulkan timestamps report GPU work through the visibility phase and through the draw, including clears and graphics pipeline work ahead of the timestamp. The visibility phase in CPU mode includes the indirect-command upload. It excludes CPU frustum testing, which appears in the CPU column.

## Validation and capture

The graphics smoke test compares both paths against a CPU count at 4,096, 65,536 and 262,144 objects. The benchmark takes 12 warm-up frames and 120 measured frames per path/count and writes median microseconds at a fixed camera angle and a nominal 1024×640 framebuffer. It requests immediate presentation when supported. Record the actual framebuffer size, CPU, GPU, driver and build configuration alongside results.

In RenderDoc, launch the built executable with the interactive scene and capture a frame with compute culling enabled. Inspect the compute dispatch, the barrier and the indirect command under the scene draw. Compare against a frame with compute culling disabled and the orbit paused.

## Measured medians

Ryzen 7 3700X, GeForce RTX 3070 Ti driver 596.49, Windows 10 22H2, MSVC 19.44 x64 Release, 1024×640, fixed camera and 120 samples after warm-up.

| Objects | Path | Visible | CPU work (µs) | GPU visibility phase (µs) | GPU through scene draw (µs) |
| ---: | :--- | ---: | ---: | ---: | ---: |
| 4,096 | CPU | 1,685 | 51.30 | 3.97 | 7.55 |
| 4,096 | Compute | 1,685 | 7.60 | 13.78 | 18.24 |
| 65,536 | CPU | 27,239 | 936.75 | 4.00 | 27.33 |
| 65,536 | Compute | 27,239 | 7.80 | 109.63 | 133.55 |
| 262,144 | CPU | 109,330 | 3820.00 | 4.35 | 91.78 |
| 262,144 | Compute | 109,330 | 7.90 | 419.10 | 507.34 |
