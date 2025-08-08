#!/usr/bin/env bash
# Copyright (c) 2024-2026 fei_cong(https://github.com/feicong/frida-course)
# export https_proxy=http://192.168.0.120:7890 http_proxy=http://192.168.0.120:7890 all_proxy=socks5://192.168.0.120:7890

set -euo pipefail
trap 'echo "Error: in $0 on line $LINENO"' ERR

if [ "$(id -u)" -ne 0 ]; then
	echo "Please run script as root user."
	exit 1
fi

export DEBIAN_FRONTEND=noninteractive
export TZ="Asia/Shanghai"

ARCH="$(uname -m)"

# 检查当前系统版本是否为 noble
OS_VERSION=$(lsb_release -cs)
if [ "$OS_VERSION" != "noble" ]; then
    echo "This script is designed for Ubuntu Noble. Exiting."
    exit 1
fi

echo "$(whoami) ALL=(ALL) NOPASSWD:ALL" | tee /etc/sudoers.d/$(whoami)
visudo -cf /etc/sudoers.d/$(whoami)

# Update apt sources list based on architecture
if [ "$ARCH" = "x86_64" ] || [ "$ARCH" = "i686" ] || [ "$ARCH" = "x86" ]; then
	tee /etc/apt/sources.list.d/ubuntu.sources <<EOF

Types: deb
URIs: https://mirrors.tuna.tsinghua.edu.cn/ubuntu
Suites: noble noble-updates noble-backports
Components: main restricted universe multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg

# 默认注释了源码镜像以提高 apt update 速度，如有需要可自行取消注释
# Types: deb-src
# URIs: https://mirrors.tuna.tsinghua.edu.cn/ubuntu
# Suites: noble noble-updates noble-backports
# Components: main restricted universe multiverse
# Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg

# 以下安全更新软件源包含了官方源与镜像站配置，如有需要可自行修改注释切换
Types: deb
URIs: http://security.ubuntu.com/ubuntu/
Suites: noble-security
Components: main restricted universe multiverse
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg

# Types: deb-src
# URIs: http://security.ubuntu.com/ubuntu/
# Suites: noble-security
# Components: main restricted universe multiverse
# Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg

# 预发布软件源，不建议启用

# Types: deb
# URIs: https://mirrors.tuna.tsinghua.edu.cn/ubuntu
# Suites: noble-proposed
# Components: main restricted universe multiverse
# Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg

# # Types: deb-src
# # URIs: https://mirrors.tuna.tsinghua.edu.cn/ubuntu
# # Suites: noble-proposed
# # Components: main restricted universe multiverse
# # Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg

EOF
fi

# Install required packages
sudo apt-get update &&
	sudo apt-get install -y libzstd-dev libedit-dev cmake vim \
		lsb-release software-properties-common tree sed wget apt-file \
		gnupg gnupg2 unzip ninja-build git python3 python3-dev python3-pip python3-venv \
		libelf-dev x11-apps patchelf curl xz-utils build-essential file flex bc bison \
		tzdata qemu-user ca-certificates iputils-ping gperf pkg-config socat ltrace strace openjdk-21-jdk \
		help2man autoconf gawk libtool-bin libncurses-dev texinfo unifdef p7zip-full libc6-dev \
		bazel-bootstrap ccache libssl-dev pahole && apt-file update

wget -qO - 'https://proget.makedeb.org/debian-feeds/prebuilt-mpr.pub' | gpg --dearmor | sudo tee /usr/share/keyrings/prebuilt-mpr-archive-keyring.gpg 1> /dev/null
echo "deb [arch=all,$(dpkg --print-architecture) signed-by=/usr/share/keyrings/prebuilt-mpr-archive-keyring.gpg] https://proget.makedeb.org prebuilt-mpr $(lsb_release -cs)" | sudo tee /etc/apt/sources.list.d/prebuilt-mpr.list
sudo apt update && sudo apt install -y just

ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ >/etc/timezone

# pip setup
pip3 config set global.index-url https://pypi.tuna.tsinghua.edu.cn/simple
pip3 install -U pip --user --break-system-packages
pip3 install lief ninja meson typing-extensions colorama prompt-toolkit pygments graphlib --user --break-system-packages
