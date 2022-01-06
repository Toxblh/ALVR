FROM ubuntu:latest

ENV DEBIAN_FRONTEND="noninteractive" TZ="Europe/London"

RUN apt update -y
RUN apt install curl -y

RUN apt install build-essential pkg-config nasm libva-dev libdrm-dev libvulkan-dev libx264-dev libx265-dev -y
RUN apt install git cmake libasound2-dev libgtk-3-dev libunwind-dev -y

# Get Rust
RUN curl https://sh.rustup.rs -sSf | bash -s -- -y
ENV PATH="/root/.cargo/bin:${PATH}"

RUN git clone https://github.com/Toxblh/ALVR.git
RUN ls
WORKDIR /ALVR
RUN ls
RUN git checkout nvidia-linux

RUN cargo update

RUN mkdir -p /ALVR/deps/linux
RUN curl -o /ALVR/deps/temp_download.zip --url https://codeload.github.com/FFmpeg/FFmpeg/zip/n4.4
RUN apt install unzip -y
RUN unzip /ALVR/deps/temp_download.zip -d /ALVR/deps/linux
RUN ls
# openssl don't need for nvenc
RUN apt install -y libffmpeg-nvenc-dev nvidia-cuda-toolkit
RUN apt install -y openssl libssl-dev libclang-dev

RUN cargo xtask build-ffmpeg-linux
RUN cd deps/linux/FFmpeg-n4.4 && make install && cd ../../..
RUN cargo build -p alvr_xtask -p alvr_launcher -p alvr_server -p alvr_vulkan-layer -p vrcompositor-wrapper --verbose