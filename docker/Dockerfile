# Build-ready OpenHarmony container for the Oniro x86_general emulator.
#
# The official OpenHarmony "standard" build image ships without several host
# build tools that a *cold* build of this tree needs. Third-party components
# run autotools/cmake shims at gn-gen / ninja time, and the stock image makes
# them fail:
#   * third_party/libnl   -> autogen.sh needs `autoreconf`  (autoconf/automake/m4)
#   * third_party/libtiff -> install.sh needs `cmake`
#   * libtool-based builds -> need the `libtool` binary (libtool-bin) + libltdl
#
# This image is the stock base plus exactly those tools, so the emulator can be
# built from a freshly repo-synced tree without hand-patching the container.
#
# Build:
#   sudo docker build -t oniro-oh-standard:3.2 device/board/oniro/docker
#
# Run (adjust the source path as needed). No --privileged and no --network are
# needed: the build compiles and packs ext4 images with mke2fs/e2fsdroid, which
# do not mount anything or use loop devices.
#   sudo docker run -d -it --name oniro-build \
#       -w /home/openharmony \
#       -v "$PWD":/home/openharmony/workdir \
#       -v "$HOME/.ccache":/root/.ccache \
#       oniro-oh-standard:3.2 /bin/bash
FROM swr.cn-south-1.myhuaweicloud.com/openharmony-docker/docker_oh_standard:3.2

# Non-interactive apt so the image builds unattended.
ARG DEBIAN_FRONTEND=noninteractive

# Host build tools the stock image lacks. `libtool` ships the macros/libtoolize;
# the actual `libtool` executable lives in `libtool-bin`, so both are needed.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        autoconf \
        automake \
        autotools-dev \
        m4 \
        libtool \
        libtool-bin \
        libltdl-dev \
        cmake \
    && rm -rf /var/lib/apt/lists/*

# Fail the build early if any tool is missing / not on PATH.
RUN set -eux; \
    autoreconf --version | head -1; \
    aclocal    --version | head -1; \
    automake   --version | head -1; \
    autoconf   --version | head -1; \
    m4         --version | head -1; \
    libtool    --version | head -1; \
    libtoolize --version | head -1; \
    cmake      --version | head -1
