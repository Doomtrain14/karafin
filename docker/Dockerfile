# karafin-builder: devkitA64 + libnx + official switch portlibs (from the pinned base image)
# overlaid with Switchfin's nvtegra-enabled ffmpeg/mpv and newer curl/mbedtls.
#
# Build context is build/pkgs (staged by build.ps1): *.pkg.tar.zst, packages.lock, install-packages.sh.

ARG BASE_IMAGE=docker.io/devkitpro/devkita64@sha256:82575ea78651b530b2e232bb3799cfd1fe331514e053d5f724bb4b28191fb79d
FROM ${BASE_IMAGE}

COPY packages.lock install-packages.sh /tmp/karafin-pkgs/
COPY *.pkg.tar.zst /tmp/karafin-pkgs/
RUN sh /tmp/karafin-pkgs/install-packages.sh /tmp/karafin-pkgs && rm -rf /tmp/karafin-pkgs

ENV DEVKITPRO=/opt/devkitpro \
    DEVKITA64=/opt/devkitpro/devkitA64 \
    PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/devkitA64/bin:/opt/devkitpro/portlibs/switch/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

LABEL org.opencontainers.image.title="karafin-builder"
WORKDIR /src
