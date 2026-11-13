# Debian bullseye arm64: glibc 2.31 / libstdc++ 6.0.28 / OpenSSL 1.1, none newer than the TrimUI Brick's.
FROM --platform=linux/arm64 debian:bullseye
RUN printf '%s\n' "deb http://archive.debian.org/debian bullseye main" \
      "deb http://archive.debian.org/debian bullseye-updates main" \
      "deb http://archive.debian.org/debian-security bullseye-security main" > /etc/apt/sources.list \
 && apt-get -o Acquire::Check-Valid-Until=false update \
 && apt-get install -y --no-install-recommends gcc g++ make pkg-config git ca-certificates file xz-utils \
      libssl-dev zlib1g-dev libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev \
 && git config --global --add safe.directory '*'
