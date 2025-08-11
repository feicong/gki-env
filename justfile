# Copyright (C) 2025-2026 fei_cong(https://github.com/feicong/feicong-course)
# 安卓GKI内核构建脚本，支持 KernelSU

# 支持通过 env 文件配置参数，支持安卓13/14/15 多版本编译
# 如果编译安卓14内核，先执行 cp env.android14 .env
set dotenv-load

export ANDROID_VERSION := env_var_or_default("ANDROID_VERSION", "android13")
export KERNEL_VERSION := env_var_or_default("KERNEL_VERSION", "5.15")
export SUB_LEVEL := env_var_or_default("SUB_LEVEL", "167")
export OS_PATCH_LEVEL := env_var_or_default("OS_PATCH_LEVEL", "2024-11")
export KERNELSU_VARIANT := env_var_or_default("KERNELSU_VARIANT", "KSU")
export KERNELSU_BRANCH := env_var_or_default("KERNELSU_BRANCH", "Stable")
export INCLUDE_SUSFS := env_var_or_default("INCLUDE_SUSFS", "true")

# 目录设置
WORKSPACE := justfile_directory()
CONFIG := ANDROID_VERSION + "-" + KERNEL_VERSION + "-" + SUB_LEVEL
TOOLCHAIN_DIR := WORKSPACE + "/" + CONFIG + "/prebuilts/kernel-build-tools"
MKBOOTIMG_DIR := WORKSPACE + "/" + CONFIG + "/tools/mkbootimg"
DIST_DIR := WORKSPACE + "/dist"

# 环境变量
export https_proxy := env_var_or_default("https_proxy", "")
export HTTP_PROXY := env_var_or_default("HTTP_PROXY", "")
export CCACHE_COMPILERCHECK := "%compiler% -dumpmachine; %compiler% -dumpversion"
export CCACHE_NOHASHDIR := "true"
export CCACHE_HARDLINK := "true"
export CCACHE_DIR := env_var_or_default("HOME", "") + "/.ccache"

# 默认目标
default:
    @just --list

# 安装依赖并设置构建环境
setup:
    #!/bin/bash
    echo "正在设置构建环境..."
    
    # 安装系统依赖
    sudo apt update
    sudo apt install -y bazel-bootstrap ccache curl git python3 python3-pip build-essential bc bison flex libssl-dev libelf-dev pahole
    
    # 配置 ccache
    mkdir -p ~/.cache/bazel
    ccache --version
    ccache --max-size=2G
    ccache --set-config=compression=true
    
    # 安装 repo 工具
    mkdir -p {{WORKSPACE}}/git-repo
    if [ ! -f {{WORKSPACE}}/git-repo/repo ]; then
        echo "正在下载 repo 工具..."
        curl https://storage.googleapis.com/git-repo-downloads/repo > {{WORKSPACE}}/git-repo/repo
        chmod a+rx {{WORKSPACE}}/git-repo/repo
    fi
    
    echo "环境设置完成！"

# 下载依赖（AnyKernel3、SUSFS、内核补丁）
download-deps:
    #!/bin/bash
    if [ -d "AnyKernel3" ] && [ -d "susfs4ksu" ] && [ -d "kernel_patches" ]; then
        echo "依赖已下载，跳过。"
        exit 0
    fi
    echo "正在下载依赖..."
    # 克隆 AnyKernel3
    if [ ! -d "AnyKernel3" ]; then
        git clone https://github.com/osm0sis/AnyKernel3.git -b gki --depth 1
    fi
    # 克隆 SUSFS
    if [ ! -d "susfs4ksu" ]; then
        git clone https://gitlab.com/simonpunk/susfs4ksu.git -b gki-{{ANDROID_VERSION}}-{{KERNEL_VERSION}} --depth 1
    fi
    # 克隆内核补丁
    if [ ! -d "kernel_patches" ]; then
        git clone https://github.com/WildKernels/kernel_patches.git --depth 1
    fi

# 下载 GKI 内核源码
download-gki: download-deps
    #!/bin/bash
    if [ -d "{{CONFIG}}/.repo" ]; then
        echo "GKI 内核源码已下载，跳过。"
        exit 0
    fi
    echo "正在下载 {{CONFIG}} GKI 内核源码..."
    # 创建配置目录
    mkdir -p {{CONFIG}}
    cd {{CONFIG}}
    # 初始化 repo
    FORMATTED_BRANCH="{{ANDROID_VERSION}}-{{KERNEL_VERSION}}-{{OS_PATCH_LEVEL}}"
    {{WORKSPACE}}/git-repo/repo init --depth=1 --u https://android.googlesource.com/kernel/manifest -b common-${FORMATTED_BRANCH} --repo-rev=v2.16
    # 检查分支是否已废弃并更新 manifest
    REMOTE_BRANCH=$(git ls-remote https://android.googlesource.com/kernel/common ${FORMATTED_BRANCH})
    if echo "$REMOTE_BRANCH" | grep -q deprecated; then
        echo "发现分支已废弃: $FORMATTED_BRANCH"
        sed -i "s/\"${FORMATTED_BRANCH}\"/\"deprecated\/${FORMATTED_BRANCH}\"/g" .repo/manifests/default.xml
    fi
    # 同步 repo
    {{WORKSPACE}}/git-repo/repo sync -c -j$(nproc --all) --no-tags --fail-fast

# 应用 KernelSU 补丁
apply-kernelsu:
    #!/bin/bash
    set -e
    echo "正在应用 KernelSU 补丁..."
    cd {{CONFIG}}

    KERNEL_LINK="common/drivers/kernelsu/kernel"

    # 检查是否已有 kernel 软链接
    if [ -L "$KERNEL_LINK" ]; then
        echo "KernelSU 补丁已应用（软链接指向：$REAL_PATH），跳过 setup.sh。"
        exit 0
    fi

    if [ -d "KernelSU/kernel" ]; then
        echo "KernelSU 目录存在，跳过 setup.sh。"
        exit 0
    fi

    # 判断分支
    if [[ "{{KERNELSU_BRANCH}}" == "Stable" ]]; then
        BRANCH=""
    elif [[ "{{KERNELSU_BRANCH}}" == "Dev" && ( "{{KERNELSU_VARIANT}}" == "KSU" || "{{KERNELSU_VARIANT}}" == "MKSU" ) ]]; then
        BRANCH="-s main"
    elif [[ "{{KERNELSU_BRANCH}}" == "Dev" && "{{KERNELSU_VARIANT}}" == "KSU_NEXT" ]]; then
        BRANCH="-s next"
    fi

    # 应用 KernelSU
    if [[ "{{KERNELSU_VARIANT}}" == "KSU" ]]; then
        echo "添加 KernelSU 官方版本..."
        curl -LSs "https://raw.githubusercontent.com/tiann/KernelSU/main/kernel/setup.sh" | bash $BRANCH
    elif [[ "{{KERNELSU_VARIANT}}" == "KSU_NEXT" ]]; then
        echo "添加 KernelSU Next 版本..."
        curl -LSs "https://raw.githubusercontent.com/KernelSU-Next/KernelSU-Next/next/kernel/setup.sh" | bash $BRANCH
    elif [[ "{{KERNELSU_VARIANT}}" == "MKSU" ]]; then
        echo "添加 KernelSU MKSU 版本..."
        curl -LSs "https://raw.githubusercontent.com/5ec1cff/KernelSU/main/kernel/setup.sh" | bash $BRANCH
    fi

# 应用 SUSFS 补丁
apply-susfs:
    #!/bin/bash
    if [[ "{{INCLUDE_SUSFS}}" != "true" ]]; then
        echo "未启用 SUSFS，跳过补丁"
        exit 0
    fi
    
    echo "正在应用 SUSFS 补丁..."
    cd {{CONFIG}}
    
    # 拷贝 SUSFS 补丁
    cp ../susfs4ksu/kernel_patches/50_add_susfs_in_gki-{{ANDROID_VERSION}}-{{KERNEL_VERSION}}.patch ./common/
    cp ../susfs4ksu/kernel_patches/fs/* ./common/fs/
    cp ../susfs4ksu/kernel_patches/include/linux/* ./common/include/linux/
    
    # 应用不同变体的补丁（已应用则跳过）
    if [[ "{{KERNELSU_VARIANT}}" == "KSU" ]]; then
        echo "为 KernelSU 官方版应用 SUSFS 补丁..."
        cd ./KernelSU
        cp ../../susfs4ksu/kernel_patches/KernelSU/10_enable_susfs_for_ksu.patch ./
        if patch -p1 --dry-run -N < 10_enable_susfs_for_ksu.patch | grep -q 'Reversed (or previously applied) patch detected'; then
            echo "10_enable_susfs_for_ksu.patch 已应用，跳过。"
        else
            patch -p1 --forward --fuzz=3 < 10_enable_susfs_for_ksu.patch
        fi
    elif [[ "{{KERNELSU_VARIANT}}" == "KSU_NEXT" ]]; then
        echo "为 KernelSU-Next 应用 SUSFS 补丁..."
        cd ./KernelSU-Next
        cp ../../kernel_patches/next/kernel-patch-susfs-v1.5.7-to-KernelSU-Next.patch ./
        if patch -p1 --dry-run -N < kernel-patch-susfs-v1.5.7-to-KernelSU-Next.patch | grep -q 'Reversed (or previously applied) patch detected'; then
            echo "kernel-patch-susfs-v1.5.7-to-KernelSU-Next.patch 已应用，跳过。"
        else
            patch -p1 --forward --fuzz=3 < kernel-patch-susfs-v1.5.7-to-KernelSU-Next.patch || true
        fi
    elif [[ "{{KERNELSU_VARIANT}}" == "MKSU" ]]; then
        echo "应用 SUSFS 补丁..."
        cd ./KernelSU
        cp ../../susfs4ksu/kernel_patches/KernelSU/10_enable_susfs_for_ksu.patch ./
        if patch -p1 --dry-run -N < 10_enable_susfs_for_ksu.patch | grep -q 'Reversed (or previously applied) patch detected'; then
            echo "10_enable_susfs_for_ksu.patch 已应用，跳过。"
        else
            patch -p1 --forward --fuzz=3 < 10_enable_susfs_for_ksu.patch || true
        fi
    fi
    cd ../common
    if patch -p1 --dry-run -N < 50_add_susfs_in_gki-{{ANDROID_VERSION}}-{{KERNEL_VERSION}}.patch | grep -q 'Reversed (or previously applied) patch detected'; then
    echo "50_add_susfs_in_gki-{{ANDROID_VERSION}}-{{KERNEL_VERSION}}.patch 已应用，跳过。"
    else
        patch -p1 --fuzz=3 < 50_add_susfs_in_gki-{{ANDROID_VERSION}}-{{KERNEL_VERSION}}.patch || true
    fi

# 应用其他补丁
apply-patches:
    #!/bin/bash
    echo "正在应用其他补丁..."
    cd {{CONFIG}}
    
    # KSU 变体应用 Managers 补丁（已应用则跳过）
    if [[ "{{KERNELSU_VARIANT}}" == "KSU" ]]; then
        cd KernelSU
        if [[ "{{INCLUDE_SUSFS}}" == "true" ]]; then
            cp ../../kernel_patches/apk_sign.c_fix.patch ./
            if patch -p1 --dry-run -N < apk_sign.c_fix.patch | grep -q 'Reversed (or previously applied) patch detected'; then
                echo "apk_sign.c_fix.patch 已应用，跳过。"
            else
                patch -p1 --fuzz=3 < apk_sign.c_fix.patch
            fi
        else
            cp ../../kernel_patches/no-susfs_apk_sign.c_fix.patch ./
            if patch -p1 --dry-run -N < no-susfs_apk_sign.c_fix.patch | grep -q 'Reversed (or previously applied) patch detected'; then
                echo "no-susfs_apk_sign.c_fix.patch 已应用，跳过。"
            else
                patch -p1 --fuzz=3 < no-susfs_apk_sign.c_fix.patch
            fi
        fi
        cd ..
    fi

    # 应用 hooks 补丁（已应用则跳过）
    cd common
    if [[ "{{KERNELSU_VARIANT}}" == "KSU_NEXT" ]]; then
        echo "为 KernelSU-Next 应用 hooks 补丁..."
        cp ../../kernel_patches/next/syscall_hooks.patch ./
        if patch -p1 --dry-run -N < syscall_hooks.patch | grep -q 'Reversed (or previously applied) patch detected'; then
            echo "syscall_hooks.patch 已应用，跳过。"
        else
            patch -p1 -F 3 < syscall_hooks.patch
        fi
    fi

    # 应用隐藏补丁（已应用则跳过）
    cp ../../kernel_patches/69_hide_stuff.patch ./
    if patch -p1 --dry-run -N < 69_hide_stuff.patch | grep -q 'Reversed (or previously applied) patch detected'; then
        echo "69_hide_stuff.patch 已应用，跳过。"
    else
        patch -p1 -F 3 < 69_hide_stuff.patch
    fi

# 配置内核
configure:
    #!/bin/bash
    echo "正在配置内核..."
    cd {{CONFIG}}
    
    # 添加 KSU 配置
    echo "CONFIG_KSU=y" >> ./common/arch/arm64/configs/gki_defconfig
    
    if [[ "{{KERNELSU_VARIANT}}" == "KSU_NEXT" ]]; then
        echo "CONFIG_KSU_WITH_KPROBES=n" >> ./common/arch/arm64/configs/gki_defconfig
    fi
    
    # 如启用 SUSFS，添加相关配置
    if [[ "{{INCLUDE_SUSFS}}" == "true" ]]; then
        echo "CONFIG_KSU_SUSFS=y" >> ./common/arch/arm64/configs/gki_defconfig
        echo "CONFIG_KSU_SUSFS_HAS_MAGIC_MOUNT=y" >> ./common/arch/arm64/configs/gki_defconfig
        echo "CONFIG_KSU_SUSFS_SUS_PATH=y" >> ./common/arch/arm64/configs/gki_defconfig
        echo "CONFIG_KSU_SUSFS_SUS_MOUNT=y" >> ./common/arch/arm64/configs/gki_defconfig
        echo "CONFIG_KSU_SUSFS_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT=y" >> ./common/arch/arm64/configs/gki_defconfig
        echo "CONFIG_KSU_SUSFS_AUTO_ADD_SUS_BIND_MOUNT=y" >> ./common/arch/arm64/configs/gki_defconfig
        echo "CONFIG_KSU_SUSFS_SUS_KSTAT=y" >> ./common/arch/arm64/configs/gki_defconfig
        echo "CONFIG_KSU_SUSFS_SUS_OVERLAYFS=n" >> ./common/arch/arm64/configs/gki_defconfig
        echo "CONFIG_KSU_SUSFS_TRY_UMOUNT=y" >> ./common/arch/arm64/configs/gki_defconfig
        echo "CONFIG_KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT=y" >> ./common/arch/arm64/configs/gki_defconfig
        echo "CONFIG_KSU_SUSFS_SPOOF_UNAME=y" >> ./common/arch/arm64/configs/gki_defconfig
        echo "CONFIG_KSU_SUSFS_ENABLE_LOG=y" >> ./common/arch/arm64/configs/gki_defconfig
        echo "CONFIG_KSU_SUSFS_HIDE_KSU_SUSFS_SYMBOLS=y" >> ./common/arch/arm64/configs/gki_defconfig
        echo "CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG=y" >> ./common/arch/arm64/configs/gki_defconfig
        echo "CONFIG_KSU_SUSFS_OPEN_REDIRECT=y" >> ./common/arch/arm64/configs/gki_defconfig
        
        if [[ "{{KERNELSU_VARIANT}}" == "KSU_NEXT" ]]; then
            echo "CONFIG_KSU_SUSFS_SUS_SU=n" >> ./common/arch/arm64/configs/gki_defconfig
        else
            echo "CONFIG_KSU_SUSFS_SUS_SU=y" >> ./common/arch/arm64/configs/gki_defconfig
        fi
    fi
    
    # 添加额外配置
    echo "CONFIG_TMPFS_XATTR=y" >> ./common/arch/arm64/configs/gki_defconfig
    echo "CONFIG_TMPFS_POSIX_ACL=y" >> ./common/arch/arm64/configs/gki_defconfig
    echo "CONFIG_IP_NF_TARGET_TTL=y" >> ./common/arch/arm64/configs/gki_defconfig
    echo "CONFIG_IP6_NF_TARGET_HL=y" >> ./common/arch/arm64/configs/gki_defconfig
    echo "CONFIG_IP6_NF_MATCH_HL=y" >> ./common/arch/arm64/configs/gki_defconfig
    
    # 添加 BBR 配置
    echo "CONFIG_TCP_CONG_ADVANCED=y" >> ./common/arch/arm64/configs/gki_defconfig
    echo "CONFIG_TCP_CONG_BBR=y" >> ./common/arch/arm64/configs/gki_defconfig
    echo "CONFIG_NET_SCH_FQ=y" >> ./common/arch/arm64/configs/gki_defconfig
    echo "CONFIG_TCP_CONG_BIC=n" >> ./common/arch/arm64/configs/gki_defconfig
    echo "CONFIG_TCP_CONG_WESTWOOD=n" >> ./common/arch/arm64/configs/gki_defconfig
    echo "CONFIG_TCP_CONG_HTCP=n" >> ./common/arch/arm64/configs/gki_defconfig
    
    # 移除 check_defconfig
    sed -i 's/check_defconfig//' ./common/build.config.gki
    
    # 修改内核名称
    perl -pi -e 's{UTS_VERSION="\$\(echo \$UTS_VERSION \$CONFIG_FLAGS \$TIMESTAMP \| cut -b -\$UTS_LEN\)"}{UTS_VERSION="#1 SMP PREEMPT Sat Apr 20 04:20:00 UTC 2024"}' ./common/scripts/mkcompile_h
    
    sed -i '/^[[:space:]]*"protected_exports_list"[[:space:]]*:[[:space:]]*"android\/abi_gki_protected_exports_aarch64",$/d' ./common/BUILD.bazel
    rm -rf ./common/android/abi_gki_protected_exports_*
    sed -i "/stable_scmversion_cmd/s/-maybe-dirty//g" ./build/kernel/kleaf/impl/stamp.bzl

# 构建gki内核
build-gki:
    #!/bin/bash
    echo "正在编译内核..."
    cd {{CONFIG}}
    
    set -e
    set -x
    
    tools/bazel build --disk_cache=$HOME/.cache/bazel --config=fast --lto=thin //common:kernel_aarch64_dist
    
    ccache --show-stats

# 构建CVD内核
build-cvd-kernel:
    #!/bin/bash
    echo "正在编译 CVD 内核..."
    cd {{CONFIG}}
    
    set -e
    set -x
    
    tools/bazel build --disk_cache=$HOME/.cache/bazel --config=fast --lto=thin //common-modules/virtual-device:virtual_device_aarch64_dist
    
    ccache --show-stats

# 构建CVD内核
build-cvd-kernel-x86_64:
    #!/bin/bash
    echo "正在编译 CVD 内核..."
    cd {{CONFIG}}
    
    set -e
    set -x
    
    tools/bazel build --disk_cache=$HOME/.cache/bazel --config=fast --lto=thin //common-modules/virtual-device:virtual_device_x86_64_dist
    
    ccache --show-stats

# 生成 boot.img 镜像
create-bootimg:
    #!/bin/bash
    echo "正在生成 boot 镜像..."
    
    mkdir -p {{DIST_DIR}}
    
    cd {{CONFIG}}
    
    cp -f ./bazel-bin/common/kernel_aarch64/Image {{DIST_DIR}}
    cp -f ./bazel-bin/common/kernel_aarch64/Image.lz4 {{DIST_DIR}}

    cd {{DIST_DIR}}
    gzip -n -k -f -9 ./Image > ./Image.gz
    
    echo "生成 boot.img 镜像"
    {{MKBOOTIMG_DIR}}/mkbootimg.py --header_version 4 --kernel Image --output boot.img
    {{TOOLCHAIN_DIR}}/linux-x86/bin/avbtool add_hash_footer --partition_name boot --partition_size $((64 * 1024 * 1024)) --image boot.img --algorithm SHA256_RSA2048 --key {{TOOLCHAIN_DIR}}/linux-x86/share/avb/testkey_rsa2048.pem
    mv -f ./boot.img ./{{KERNELSU_VARIANT}}_{{ANDROID_VERSION}}-{{KERNEL_VERSION}}.{{SUB_LEVEL}}-{{OS_PATCH_LEVEL}}-boot.img
    
    echo "生成 boot-gz.img 镜像"
    {{MKBOOTIMG_DIR}}/mkbootimg.py --header_version 4 --kernel Image.gz --output boot-gz.img
    {{TOOLCHAIN_DIR}}/linux-x86/bin/avbtool add_hash_footer --partition_name boot --partition_size $((64 * 1024 * 1024)) --image boot-gz.img --algorithm SHA256_RSA2048 --key {{TOOLCHAIN_DIR}}/linux-x86/share/avb/testkey_rsa2048.pem
    mv -f ./boot-gz.img ./{{KERNELSU_VARIANT}}_{{ANDROID_VERSION}}-{{KERNEL_VERSION}}.{{SUB_LEVEL}}-{{OS_PATCH_LEVEL}}-boot-gz.img
    
    echo "生成 boot-lz4.img 镜像"
    {{MKBOOTIMG_DIR}}/mkbootimg.py --header_version 4 --kernel Image.lz4 --output boot-lz4.img
    {{TOOLCHAIN_DIR}}/linux-x86/bin/avbtool add_hash_footer --partition_name boot --partition_size $((64 * 1024 * 1024)) --image boot-lz4.img --algorithm SHA256_RSA2048 --key {{TOOLCHAIN_DIR}}/linux-x86/share/avb/testkey_rsa2048.pem
    mv -f ./boot-lz4.img ./{{KERNELSU_VARIANT}}_{{ANDROID_VERSION}}-{{KERNEL_VERSION}}.{{SUB_LEVEL}}-{{OS_PATCH_LEVEL}}-boot-lz4.img

    cd ..

# 生成 AnyKernel3 可刷 zip 包
create-anykernel:
    #!/bin/bash
    ZIP_NAME="{{KERNELSU_VARIANT}}_{{ANDROID_VERSION}}-{{KERNEL_VERSION}}.{{SUB_LEVEL}}-{{OS_PATCH_LEVEL}}-AnyKernel3.zip"
    if [ -f "{{DIST_DIR}}/$ZIP_NAME" ]; then
        echo "$ZIP_NAME 已存在，跳过。"
        exit 0
    fi
    echo "正在生成 AnyKernel3 刷机包..."
    cd AnyKernel3 && rm -rf .git/
    echo "正在打包 zip 文件: $ZIP_NAME..."
    cp {{DIST_DIR}}/Image ./Image
    zip -r "{{DIST_DIR}}/$ZIP_NAME" ./*
    rm -f ./Image

# 清理构建产物
clean:
    @echo "Cleaning build artifacts..."
    rm -rf {{DIST_DIR}}
    rm -f *.img *.img.gz *.zip Image Image.gz Image.lz4

# 全部清理（包括依赖）
clean-all: clean
    @echo "Cleaning all files..."
    rm -rf kernel-build-tools mkbootimg git-repo
    rm -rf AnyKernel3 susfs4ksu kernel_patches

# 重置repo代码
reset:
    #!/bin/bash
    @echo "重置 repo 代码..."
    cd {{CONFIG}}
    repo forall -c "git reset --hard"
    repo forall -c "git clean -fd"

# 构建CVD内核
cook-cvd: setup download-gki build-cvd-kernel-x86_64
    @echo ""
    @echo "构建模拟器内核完成！"
    @echo ""

# 构建gki内核并打包（应用 KernelSU 补丁）
cook-gki: setup download-gki apply-kernelsu build-gki create-bootimg create-anykernel
    @echo ""
    @echo "构建完成！"
    @echo ""
    @echo "生成文件："
    @echo "  - {{DIST_DIR}}/{{KERNELSU_VARIANT}}_{{ANDROID_VERSION}}-{{KERNEL_VERSION}}.{{SUB_LEVEL}}-{{OS_PATCH_LEVEL}}-AnyKernel3.zip"
    @echo "  - {{DIST_DIR}}/{{KERNELSU_VARIANT}}_{{ANDROID_VERSION}}-{{KERNEL_VERSION}}.{{SUB_LEVEL}}-{{OS_PATCH_LEVEL}}-boot*.img.gz"
    @echo "  - {{DIST_DIR}}/{{KERNELSU_VARIANT}}_{{ANDROID_VERSION}}-{{KERNEL_VERSION}}.{{SUB_LEVEL}}-{{OS_PATCH_LEVEL}}-AnyKernel3.zip"
    @echo ""

# 构建 cpuinfo 模块
cpuinfo:
    #!/bin/bash
    @echo "正在编译 cpuinfo 模块..."
    @head -30 /proc/cpuinfo
    cd modules/cpuinfo && make clean && make
    echo "cpuinfo 模块编译完成。"

# 构建 cpuinfo 模块（Android 版本）
cpuinfo-android:
    #!/bin/bash
    export ARCH=arm64
    export SUBARCH=arm64
    export CROSS_COMPILE=arm64-linux-android-
    export PATH={{CONFIG}}/build/kernel/build-tools/path/linux-x86:{{CONFIG}}/prebuilts/clang/host/linux-x86/clang-r450784e/bin:{{CONFIG}}/out/android13-5.15/common/host_tools:$PATH
    export KERNEL_SRC={{WORKSPACE}}/{{CONFIG}}/out/android13-5.15/common
    export CLANG_VERSION=r450784e
    export LC_ADDRESS=zh_CN.UTF-8
    export LC_NAME=zh_CN.UTF-8
    export LTO=thin
    export LANG=en_US.UTF-8
    export KCFLAGS=-D__ANDROID_COMMON_KERNEL__
    export LLVM=1
    cd modules/cpuinfo
    make clean
    make CC={{WORKSPACE}}/{{CONFIG}}/prebuilts/clang/host/linux-x86/clang-r450784e/bin/clang \
        LD={{WORKSPACE}}/{{CONFIG}}/prebuilts/clang/host/linux-x86/clang-r450784e/bin/ld.lld \
        OBJSIZE={{WORKSPACE}}/{{CONFIG}}/prebuilts/clang/host/linux-x86/clang-r450784e/bin/llvm-size \
        NM=={{WORKSPACE}}/{{CONFIG}}/prebuilts/clang/host/linux-x86/clang-r450784e/bin/llvm-nm

# 构建 diamorphine Rootkit模块
diamorphine:
    #!/bin/bash
    @echo "正在编译 diamorphine 模块..."
    cd modules/diamorphine && make clean && make
    echo "diamorphine 模块编译完成。"