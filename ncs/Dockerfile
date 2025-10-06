# Dockerfile for MouthPad USB - Simple NCS Build Environment
# Based on the Interrupt article approach

FROM --platform=linux/amd64 ubuntu:22.04

# Set environment variables
ENV DEBIAN_FRONTEND=noninteractive
ENV ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk-0.17.2
ENV ZEPHYR_TOOLCHAIN_VARIANT=zephyr

# Install system dependencies
RUN apt-get update && apt-get install -y \
    git \
    cmake \
    ninja-build \
    gperf \
    ccache \
    dfu-util \
    device-tree-compiler \
    wget \
    unzip \
    python3 \
    python3-pip \
    python3-setuptools \
    python3-wheel \
    python3-dev \
    python3-venv \
    libssl-dev \
    libffi-dev \
    libusb-1.0-0-dev \
    pkg-config \
    build-essential \
    && rm -rf /var/lib/apt/lists/*

# Install Zephyr SDK
RUN wget https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.17.2/zephyr-sdk-0.17.2_linux-x86_64.tar.xz \
    && tar -xf zephyr-sdk-0.17.2_linux-x86_64.tar.xz -C /opt \
    && rm zephyr-sdk-0.17.2_linux-x86_64.tar.xz \
    && /opt/zephyr-sdk-0.17.2/setup.sh -t arm-zephyr-eabi

# Install west
RUN pip3 install west

# Set up workspace directory
WORKDIR /workspace

# Create a build script
RUN echo '#!/bin/bash\n\
echo "🔧 MouthPad USB Docker Build Environment"\n\
echo "====================================="\n\
echo "Building with NCS in Docker..."\n\
echo ""\n\
if [ ! -d ".west" ]; then\n\
    echo "Initializing workspace..."\n\
    west init -m https://github.com/nrfconnect/sdk-nrf.git --mr main\n\
    west update\n\
fi\n\
echo "Building firmware..."\n\
west build -b xiao_ble app --pristine=always\n\
echo ""\n\
echo "Build completed!"\n\
echo "Artifacts:"\n\
ls -la build/app/zephyr/zephyr.*\n\
echo "====================================="\n\
' > /usr/local/bin/build.sh && chmod +x /usr/local/bin/build.sh

# Set the default command
CMD ["/usr/local/bin/build.sh"] 