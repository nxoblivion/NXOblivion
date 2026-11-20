# reOblivion web (2026-11-17): Tint, Dawn's shader compiler, for SPIR-V -> WGSL (reNut's shaders for WebGPU).
# Built in a container so nothing of it lands on the PC; nx/tools/webgpu_shaders.py runs it with docker run.
#
#   docker build -t reoblivion-tint nx/web/tint
#   docker run --rm -v <dir>:/work reoblivion-tint tint --format wgsl -o /work/x.wgsl /work/x.spv
#
# Source: Google's own repository (dawn.googlesource.com), its dependencies by its own fetch script. Only the tint
# command-line tool is built, with the SPIR-V reader and the WGSL writer (no GPU backends, samples or tests).
FROM ubuntu:24.04

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates git python3 cmake ninja-build clang lld \
    && rm -rf /var/lib/apt/lists/*

ARG DAWN_REF=main
RUN git clone --depth 1 --branch ${DAWN_REF} https://dawn.googlesource.com/dawn /src/dawn \
    && git -C /src/dawn rev-parse HEAD > /dawn-commit.txt

WORKDIR /src/dawn
RUN CC=clang CXX=clang++ cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DDAWN_FETCH_DEPENDENCIES=ON \
      -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF -DTINT_BUILD_TESTS=OFF \
      -DDAWN_ENABLE_VULKAN=OFF -DDAWN_ENABLE_DESKTOP_GL=OFF -DDAWN_ENABLE_OPENGLES=OFF -DDAWN_ENABLE_NULL=OFF \
      -DDAWN_USE_X11=OFF -DDAWN_USE_WAYLAND=OFF -DDAWN_USE_GLFW=OFF \
      -DTINT_BUILD_CMD_TOOLS=ON -DTINT_BUILD_SPV_READER=ON -DTINT_BUILD_WGSL_READER=ON -DTINT_BUILD_WGSL_WRITER=ON \
      -DTINT_BUILD_SPV_WRITER=OFF -DTINT_BUILD_MSL_WRITER=OFF -DTINT_BUILD_HLSL_WRITER=OFF -DTINT_BUILD_GLSL_WRITER=OFF \
      -DTINT_BUILD_GLSL_VALIDATOR=OFF \
    && ninja -C out tint \
    && install -m 755 out/tint /usr/local/bin/tint \
    && rm -rf out

# glslang: the web shaders are SPIRV-Cross's GLSL with their texture tables made bound (tools/webgpu_shaders.py), back
# to SPIR-V for tint. Ubuntu's package; after tint so its layer stays cached.
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends glslang-tools     && rm -rf /var/lib/apt/lists/*

WORKDIR /work
CMD ["tint", "--help"]
