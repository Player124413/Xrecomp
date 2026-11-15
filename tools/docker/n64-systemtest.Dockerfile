# n64-systemtest (lemmy-64, MIT: https://github.com/lemmy-64/n64-systemtest), the N64 hardware test ROM, built from
# source at a pinned commit for the N64 core's tests (consoles/n64/tests: n64_systemtest). It is written in Rust
# (nightly-2022-07-10, build-std) and packed into a ROM with libdragon's open-source IPL3 by nust64 (MIT). It prints
# its results through the IS-Viewer debug buffer, which n64_run --tty shows. No Nintendo code is involved.
# Used by tools/consoles/build_n64_systemtest.sh; the image is a build tool only, never published.
#
# Licences: n64-systemtest MIT, nust64 MIT / Apache-2.0, Rust toolchain MIT / Apache-2.0, crates per Cargo.lock
# (linked_list_allocator, spinning_top, arrayref, oorandom, arbitrary-int, bitbybit: MIT / Apache-2.0).
FROM ubuntu:24.04@sha256:2198e4c65076e9cb0b3d695219df667c0e3b0072da64ca3c2648aca17dcecaf5

ARG SYSTEMTEST_COMMIT=196f5421173220eb2f63a7a99c64795dc0ea0698
ARG NUST64_VERSION=0.4.1
ARG JOBS=4

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates curl git gcc libc6-dev \
    && rm -rf /var/lib/apt/lists/*

ENV RUSTUP_HOME=/opt/rustup CARGO_HOME=/opt/cargo PATH=/opt/cargo/bin:/usr/local/bin:/usr/bin:/bin
RUN curl -fsSL https://sh.rustup.rs -o /tmp/rustup.sh \
    && sh /tmp/rustup.sh -y --profile minimal --default-toolchain stable --no-modify-path \
    && rm /tmp/rustup.sh \
    && rustup toolchain install nightly-2022-07-10 --profile minimal --component rust-src

RUN cargo +stable install nust64 --version "${NUST64_VERSION}" --locked -j "${JOBS}"

RUN git init -q /src \
    && cd /src \
    && git fetch -q --depth 1 https://github.com/lemmy-64/n64-systemtest.git "${SYSTEMTEST_COMMIT}" \
    && git checkout -q FETCH_HEAD \
    && test "$(git rev-parse HEAD)" = "${SYSTEMTEST_COMMIT}"

# The 2022 nightly's cargo reads crates.io through the sparse protocol only behind -Z sparse-registry.
RUN cd /src \
    && cargo -Z sparse-registry build --release -j "${JOBS}" \
    && nust64 --libdragon release --elf target/mips-nintendo64-none/release/n64-systemtest \
    && mkdir -p /out \
    && cp "$(find target -name '*.z64' | head -n 1)" /out/n64-systemtest.z64 \
    && cp LICENSE /out/LICENSE \
    && echo "lemmy-64/n64-systemtest ${SYSTEMTEST_COMMIT}, nust64 ${NUST64_VERSION} (libdragon release IPL3)" >/out/VERSION

# The same ROM with every test group n64-systemtest has (its default build is "base" only): timing (a NOP is half a
# cycle, Random counts instructions), cycle, cop0hazard and poorly_understood_quirk. Groups the core does not aim at:
# measured, not gated (run_systemtest.sh full).
RUN cd /src \
    && cargo -Z sparse-registry build --release -j "${JOBS}" --target-dir target-full \
        --features base,timing,cycle,cop0hazard,poorly_understood_quirk \
    && nust64 --libdragon release --elf target-full/mips-nintendo64-none/release/n64-systemtest \
    && cp "$(find target-full -name '*.z64' | head -n 1)" /out/n64-systemtest-full.z64

WORKDIR /out
