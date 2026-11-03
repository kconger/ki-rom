FROM ubuntu:24.04

RUN apt update && \
    apt install -y g++-14 gcc-14 gcc-14-mipsel-linux-gnu make libpng-dev libgif-dev python3

WORKDIR /opt/src
ENTRYPOINT ["/usr/bin/make"]
