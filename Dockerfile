# dmf-mf-mxl-writer: producer-side image for the mxl-k8s demo pipeline.
#
# Builds mxl-gst-testsrc (and supporting binaries) from the libmxl source
# the go-mxl-builder/runtime pair at GO_MXL_TAG was built from, so the
# writer links the same libmxl ABI and writes the same flow header layout
# as the mxl-k8s gateway that reads its flows. The README has the bump
# policy and the patch series applied on top.

# renovate: datasource=docker depName=ghcr.io/qvest-digital/go-mxl-builder
ARG GO_MXL_TAG=1.1.0-rc.7

# MXL_SRC and MXL_REF are the repository and commit named in go-mxl's
# .github/libmxl.version at GO_MXL_TAG.
ARG MXL_SRC=https://github.com/qvest-digital/mxl.git
ARG MXL_REF=c259243931333497e61faabeedac8048915dcb2b

# libmxl-monitoring counts what mxl-gst-testsrc's writer calls return (patch
# 0011); built into the same tree and linked statically. Checked out by
# commit, which a moved tag cannot change; the tag names the release it is.
# renovate: datasource=github-tags depName=marquies/libmxl-monitoring
ARG MONITORING_TAG=v0.4.0
ARG MONITORING_REF=8a3ec560da3773319c788fe2ecd6d838784d1557

# NvNmos, NVIDIA's C API over nmos-cpp, provides the NMOS Node mxl-writer-nmos
# runs: IS-04 registration, the IS-05 Connection API and BCP-007-03's MXL
# transport. Its dependencies take the better part of an hour to compile, so it
# is not built here: dmf-mf-mxl-compositor publishes it as an image tagged by
# the NvNmos commit and the hash of the patches applied to it. Pinned by digest
# as well, so a re-pushed tag cannot change the library under the build. libnvnmos.so links its own
# dependencies statically and is built on the Debian release the go-mxl images
# are; it does not link libmxl.
ARG NVNMOS_IMAGE=ghcr.io/qvest-digital/dmf-mf-mxl-compositor/nvnmos:36fb7db9b5616a26f77164de735c15171e01aaed-59013af0@sha256:298270d13fb0f75a984c656bfdd469834c68a58a0749f2f5d5f68277258123b1

FROM ${NVNMOS_IMAGE} AS nvnmos

# Stage 1: build mxl-gst-testsrc against the canonical libmxl
FROM ghcr.io/qvest-digital/go-mxl-builder:${GO_MXL_TAG} AS build
ARG MXL_REF
ARG MXL_SRC
ARG MONITORING_REF

# opentelemetry-cpp with its OTLP/HTTP exporter, for libmxl-monitoring's OTLP
# exporter (patch 0012). go-mxl-builder does not carry it; installed from the
# image's vcpkg ports in classic mode, in a layer of its own that only a new
# GO_MXL_TAG rebuilds.
RUN /root/vcpkg/vcpkg install "opentelemetry-cpp[otlp-http]" --x-install-root=/opt/otel

WORKDIR /src
COPY patches/ /patches/
# Full clone, then checkout the pinned ref. A shallow `git fetch --depth=1
# origin <SHA>` fails (exit 128) when the server doesn't allow fetching an
# arbitrary commit by SHA (uploadpack.allowReachableSHA1InWant off) -- which is
# the GitHub default. A full clone has the commit in history, so checkout works
# whether MXL_REF is a SHA, branch, or tag.
RUN git clone "${MXL_SRC}" . \
 && git checkout "${MXL_REF}" \
 && git apply /patches/*.patch
RUN git clone https://github.com/marquies/libmxl-monitoring.git libmxl-monitoring \
 && git -C libmxl-monitoring checkout "${MONITORING_REF}" \
 && echo 'add_subdirectory(libmxl-monitoring)' >> CMakeLists.txt

# go-mxl-builder ships vcpkg with stduuid + spdlog + fmt + libfabric
# already provisioned; reuse its toolchain rather than re-fetching.
RUN cmake -S . -B build \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DMXL_ENABLE_FABRICS_OFI=ON \
        -DMXL_MONITORING_OTLP=ON \
        -DCMAKE_PREFIX_PATH=/opt/otel/x64-linux \
        -DCMAKE_TOOLCHAIN_FILE=/root/vcpkg/scripts/buildsystems/vcpkg.cmake \
    && cmake --build build --target mxl-gst-testsrc mxl-info -j

# The flow definition mxl-gst-testsrc hands to libmxl, which stores it as
# flow_def.json. The build runs the test and fails with it.
COPY tools/mxl-gst-flowdef-test/ /src/tools/mxl-gst-flowdef-test/
RUN echo 'add_subdirectory(mxl-gst-flowdef-test)' >> tools/CMakeLists.txt \
    && cmake -S . -B build \
    && cmake --build build --target mxl-gst-flowdef-test -j \
    && build/tools/mxl-gst-flowdef-test/mxl-gst-flowdef-test

# The faults mxl-gst-testsrc writes on command (patch 0013). The build runs the
# test and fails with it.
COPY tools/mxl-gst-faults-test/ /src/tools/mxl-gst-faults-test/
RUN echo 'add_subdirectory(mxl-gst-faults-test)' >> tools/CMakeLists.txt \
    && cmake -S . -B build \
    && cmake --build build --target mxl-gst-faults-test -j \
    && build/tools/mxl-gst-faults-test/mxl-gst-faults-test

# mxl-writer-status, which serves the ring state of the writer's flows, builds
# in the same tree against the same libmxl target. It comes after the other
# tools, so a change to it, or to the version it reports, leaves their layers
# cached; cmake then rebuilds only its own targets. The build runs its tests
# and fails with them.
COPY tools/mxl-writer-status/ /src/tools/mxl-writer-status/
RUN echo 'add_subdirectory(mxl-writer-status)' >> tools/CMakeLists.txt
# What the tool reports as its version: the release, or the commit.
ARG VERSION=dev
RUN cmake -S . -B build -DMXL_WRITER_STATUS_VERSION="${VERSION}" \
    && cmake --build build --target mxl-writer-status mxl-writer-status-tests -j \
    && build/tools/mxl-writer-status/mxl-writer-status-tests

# mxl-writer-nmos, the NMOS Node that runs mxl-gst-testsrc once per flow, in
# the same tree against NvNmos. The build runs its tests and fails with them.
COPY --from=nvnmos /opt/nvnmos /opt/nvnmos
COPY tools/mxl-writer-nmos/ /src/tools/mxl-writer-nmos/
COPY test/nmos/phase-probe.cpp /src/tools/mxl-writer-nmos/
RUN echo 'add_subdirectory(mxl-writer-nmos)' >> tools/CMakeLists.txt \
    && cmake -S . -B build -DNVNMOS_ROOT=/opt/nvnmos \
    && cmake --build build --target mxl-writer-nmos mxl-writer-nmos-tests mxl-phase-probe -j \
    && build/tools/mxl-writer-nmos/mxl-writer-nmos-tests

# Stage 2: runtime
FROM ghcr.io/qvest-digital/go-mxl-runtime:${GO_MXL_TAG}

# GStreamer plugin set the testsrc pipeline needs. Kept minimal -- no
# x11 / playbin chain here; the writer is headless and pushes v210 into
# libmxl, not a display sink.
RUN apt-get update && apt-get install -y --no-install-recommends \
        gstreamer1.0-plugins-base \
        gstreamer1.0-plugins-good \
        gstreamer1.0-plugins-bad \
        gstreamer1.0-x \
        libpango-1.0-0 \
        libpangocairo-1.0-0 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/tools/mxl-gst/mxl-gst-testsrc /usr/bin/mxl-gst-testsrc
COPY --from=build /src/build/tools/mxl-info/mxl-info       /usr/bin/mxl-info
COPY --from=build /src/build/tools/mxl-writer-status/mxl-writer-status /usr/bin/mxl-writer-status
COPY --from=build /src/build/tools/mxl-writer-nmos/mxl-writer-nmos     /usr/bin/mxl-writer-nmos
COPY --from=nvnmos /opt/nvnmos/lib/libnvnmos.so /usr/local/lib/
RUN ldconfig

# Flow definition template the producer script substitutes per-flow UUIDs
# into before handing the rendered JSON to mxl-gst-testsrc -v. Path
# matches the upstream jonasohland/mxl image so existing producer scripts
# keep working without modification.
RUN mkdir -p /home/mxl
COPY --from=build /src/lib/tests/data/v210_flow.json  /home/mxl/v210_flow.json
COPY --from=build /src/lib/tests/data/audio_flow.json /home/mxl/audio_flow.json

# Default to 720p (1284x720, chroma 642x720) instead of the upstream 1080p:
# ~44% the pixels -> the v210<->I420 overlay conversions (single-thread bound)
# run well under one core, with zero grain backfill. Width is 1284, not 1280:
# the v210 unpacker requires width divisible by 6 (1280 is not), and 1284 is
# a 16:9-ish width. 1296 (not 1284) is also divisible by 48, so the v210
# row stride has no padding (packed == padded) -- the GStreamer videoconvert in
# the compositor rejects the padded case, showing empty (green) tiles.
RUN sed -i 's/1920/1296/g; s/1080/720/g; s/960/648/g' /home/mxl/v210_flow.json

ENTRYPOINT ["/usr/bin/mxl-gst-testsrc"]
