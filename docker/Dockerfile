# AWvoice build environment: the pinned host tools around the pinned SDK.
#
# The SDK, not this image, carries the toolchain; the image only has to keep
# the host tools that the SDK expects. The base image is pinned by digest, the
# same digest the firmware repository uses (one base image to audit). To move
# it: docker pull debian:bookworm-slim; docker images --digests debian.
#
# Build:   docker build -t awvoice-builder docker/
# Run:     scripts/build.sh (the host entry)
FROM debian@sha256:7ff6ba31527b9e336162fa061746c366c4b2032f4d2cff2078bea43f78efe3f8

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
      --no-install-recommends \
      build-essential clang flex bison g++ gawk gcc-multilib g++-multilib \
      gettext git libncurses-dev libssl-dev python3 python3-setuptools \
      python3-distutils rsync swig unzip zlib1g-dev file wget zstd texinfo \
      curl ca-certificates jq xz-utils cpio kmod && \
    rm -rf /var/lib/apt/lists/*

# some OpenWrt host tools refuse to configure as root
ENV FORCE_UNSAFE_CONFIGURE=1

# the repository and the SDK cache are bind-mounted from the host; git runs in
# the extracted SDK and must trust what it finds there
RUN git config --system --add safe.directory '*'

# the build runs as the host user (docker run --user), so the work areas are
# world-writable inside the container only
RUN mkdir -p /work /out && chmod 1777 /work /out
ENV HOME=/work
WORKDIR /work
