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
        git clone https://github.com/KernelSU-Next/AnyKernel3.git -b gki --depth 1
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
    cd {{CONFIG}}/common

    # 判断分支
    BRANCH_ARG=""
    if [[ "{{KERNELSU_BRANCH}}" == "Stable" ]]; then
        BRANCH_ARG=""
    elif [[ "{{KERNELSU_BRANCH}}" == "Dev" ]]; then
        if [[ "{{KERNELSU_VARIANT}}" == "KSU" || "{{KERNELSU_VARIANT}}" == "MKSU" ]]; then
            BRANCH_ARG="-s main"
        elif [[ "{{KERNELSU_VARIANT}}" == "KSU_NEXT" ]]; then
            BRANCH_ARG="-s next"
        elif [[ "{{KERNELSU_VARIANT}}" == "WILD" ]]; then
            BRANCH_ARG="-s wild"
        fi
    fi

    # 应用 KernelSU
    case "{{KERNELSU_VARIANT}}" in
        "KSU")
            if [ -d "./drivers/kernelsu" ]; then
                echo "KernelSU 补丁已应用，跳过。"
                exit 0
            fi
            echo "正在添加 KernelSU 补丁..."
            if [ -d "KernelSU" ]; then
                bash KernelSU/kernel/setup.sh $BRANCH_ARG
                rm -rf ./drivers/kernelsu
                cp -r -f KernelSU/kernel ./drivers/kernelsu
                sed -i 's/^.*ccflags-y += -DKSU_VERSION=.*$/ccflags-y += -DKSU_VERSION=12345/' ./drivers/kernelsu/Makefile
                echo "KernelSU 本地添加补丁成功。"
                exit 0
            fi
            echo "添加 KernelSU 官方版本..."
            curl -LSs "https://ghfast.top/https://raw.githubusercontent.com/tiann/KernelSU/main/kernel/setup.sh" | bash -s -- $BRANCH_ARG
            ;;
        "KSU_NEXT")
            echo "添加 KernelSU Next 版本..."
            curl -LSs "https://raw.githubusercontent.com/KernelSU-Next/KernelSU-Next/next/kernel/setup.sh" | bash -s -- $BRANCH_ARG
            ;;
        "MKSU")
            echo "添加 KernelSU MKSU 版本..."
            curl -LSs "https://raw.githubusercontent.com/5ec1cff/KernelSU/main/kernel/setup.sh" | bash -s -- $BRANCH_ARG
            ;;
        "WILD")
            echo "添加 Wild KSU 版本..."
            curl -LSs "https://raw.githubusercontent.com/WildKernels/Wild_KSU/wild/kernel/setup.sh" | bash -s -- $BRANCH_ARG
            ;;
    esac

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
        cd ./common/KernelSU
        cp ../../../susfs4ksu/kernel_patches/KernelSU/10_enable_susfs_for_ksu.patch ./
        if patch -p1 --dry-run -N < 10_enable_susfs_for_ksu.patch | grep -q 'Reversed (or previously applied) patch detected'; then
            echo "10_enable_susfs_for_ksu.patch 已应用，跳过。"
        else
            patch -p1 --forward --fuzz=3 < 10_enable_susfs_for_ksu.patch
        fi
    elif [[ "{{KERNELSU_VARIANT}}" == "KSU_NEXT" ]]; then
        echo "为 KernelSU-Next 应用 SUSFS 补丁..."
        cd ./common/KernelSU-Next
        cp ../../../kernel_patches/next/kernel-patch-susfs-v1.5.7-to-KernelSU-Next.patch ./
        if patch -p1 --dry-run -N < kernel-patch-susfs-v1.5.7-to-KernelSU-Next.patch | grep -q 'Reversed (or previously applied) patch detected'; then
            echo "kernel-patch-susfs-v1.5.7-to-KernelSU-Next.patch 已应用，跳过。"
        else
            patch -p1 --forward --fuzz=3 < kernel-patch-susfs-v1.5.7-to-KernelSU-Next.patch || true
        fi
    elif [[ "{{KERNELSU_VARIANT}}" == "MKSU" ]]; then
        echo "应用 SUSFS 补丁..."
        cd ./common/KernelSU
        cp ../../../susfs4ksu/kernel_patches/KernelSU/10_enable_susfs_for_ksu.patch ./
        if patch -p1 --dry-run -N < 10_enable_susfs_for_ksu.patch | grep -q 'Reversed (or previously applied) patch detected'; then
            echo "10_enable_susfs_for_ksu.patch 已应用，跳过。"
        else
            patch -p1 --forward --fuzz=3 < 10_enable_susfs_for_ksu.patch || true
        fi
    fi
    cd ../
    if patch -p1 --dry-run -N < 50_add_susfs_in_gki-{{ANDROID_VERSION}}-{{KERNEL_VERSION}}.patch | grep -q 'Reversed (or previously applied) patch detected'; then
    echo "50_add_susfs_in_gki-{{ANDROID_VERSION}}-{{KERNEL_VERSION}}.patch 已应用，跳过。"
    else
        patch -p1 --fuzz=3 < 50_add_susfs_in_gki-{{ANDROID_VERSION}}-{{KERNEL_VERSION}}.patch || true
    fi
    echo "SUSFS 补丁应用完成。"

# 应用其他补丁
apply-patches:
    #!/bin/bash
    echo "正在应用其他补丁..."
    cd {{CONFIG}}
    
    # KSU 变体应用 Managers 补丁（已应用则跳过）
    if [[ "{{KERNELSU_VARIANT}}" == "KSU" ]]; then
        cd common/KernelSU
        if [[ "{{INCLUDE_SUSFS}}" == "true" ]]; then
            cp ../../../kernel_patches/apk_sign.c_fix.patch ./
            if patch -p1 --dry-run -N < apk_sign.c_fix.patch | grep -q 'Reversed (or previously applied) patch detected'; then
                echo "apk_sign.c_fix.patch 已应用，跳过。"
            else
                patch -p1 --fuzz=3 < apk_sign.c_fix.patch
            fi
        else
            cp ../../../kernel_patches/no-susfs_apk_sign.c_fix.patch ./
            if patch -p1 --dry-run -N < no-susfs_apk_sign.c_fix.patch | grep -q 'Reversed (or previously applied) patch detected'; then
                echo "no-susfs_apk_sign.c_fix.patch 已应用，跳过。"
            else
                patch -p1 --fuzz=3 < no-susfs_apk_sign.c_fix.patch
            fi
        fi
        cd ..
    fi

    # 应用 hooks 补丁（已应用则跳过）
    if [[ "{{KERNELSU_VARIANT}}" == "KSU_NEXT" ]]; then
        echo "为 KernelSU-Next 应用 hooks 补丁..."
        cp ../../../kernel_patches/next/syscall_hooks.patch ./
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
    echo "其他补丁应用完成。"

# 配置内核
configure:
    #!/bin/bash
    echo "正在配置内核..."
    cd {{CONFIG}}
    
    GKI_DEFCONFIG_ARM64="./common/arch/arm64/configs/gki_defconfig"
    GKI_DEFCONFIG_X86="./common/arch/x86/configs/gki_defconfig"

    # 为 arm64 和 x86 添加 KSU 配置
    echo "CONFIG_KSU=y" >> ${GKI_DEFCONFIG_ARM64}
    echo "CONFIG_KSU=y" >> ${GKI_DEFCONFIG_X86}
    
    if [[ "{{KERNELSU_VARIANT}}" == "KSU_NEXT" ]]; then
        echo "CONFIG_KSU_WITH_KPROBES=n" >> ${GKI_DEFCONFIG_ARM64}
        echo "CONFIG_KSU_WITH_KPROBES=n" >> ${GKI_DEFCONFIG_X86}
    fi
    
    # 如启用 SUSFS，为 arm64 和 x86 添加相关配置
    if [[ "{{INCLUDE_SUSFS}}" == "true" ]]; then
        SUSFS_CONFIGS=(
            "CONFIG_KSU_SUSFS=y"
            "CONFIG_KSU_SUSFS_HAS_MAGIC_MOUNT=y"
            "CONFIG_KSU_SUSFS_SUS_PATH=y"
            "CONFIG_KSU_SUSFS_SUS_MOUNT=y"
            "CONFIG_KSU_SUSFS_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT=y"
            "CONFIG_KSU_SUSFS_AUTO_ADD_SUS_BIND_MOUNT=y"
            "CONFIG_KSU_SUSFS_SUS_KSTAT=y"
            "CONFIG_KSU_SUSFS_SUS_OVERLAYFS=n"
            "CONFIG_KSU_SUSFS_TRY_UMOUNT=y"
            "CONFIG_KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT=y"
            "CONFIG_KSU_SUSFS_SPOOF_UNAME=y"
            "CONFIG_KSU_SUSFS_ENABLE_LOG=y"
            "CONFIG_KSU_SUSFS_HIDE_KSU_SUSFS_SYMBOLS=y"
            "CONFIG_KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG=y"
            "CONFIG_KSU_SUSFS_OPEN_REDIRECT=y"
        )
        for config in "${SUSFS_CONFIGS[@]}"; do
            echo "$config" >> ${GKI_DEFCONFIG_ARM64}
            echo "$config" >> ${GKI_DEFCONFIG_X86}
        done
        
        if [[ "{{KERNELSU_VARIANT}}" == "KSU_NEXT" ]]; then
            echo "CONFIG_KSU_SUSFS_SUS_SU=n" >> ${GKI_DEFCONFIG_ARM64}
            echo "CONFIG_KSU_SUSFS_SUS_SU=n" >> ${GKI_DEFCONFIG_X86}
        else
            echo "CONFIG_KSU_SUSFS_SUS_SU=y" >> ${GKI_DEFCONFIG_ARM64}
            echo "CONFIG_KSU_SUSFS_SUS_SU=y" >> ${GKI_DEFCONFIG_X86}
        fi
    fi
    
    # 为 arm64 和 x86 添加额外配置
    EXTRA_CONFIGS=(
        "CONFIG_TMPFS_XATTR=y"
        "CONFIG_TMPFS_POSIX_ACL=y"
        "CONFIG_IP_NF_TARGET_TTL=y"
        "CONFIG_IP6_NF_TARGET_HL=y"
        "CONFIG_IP6_NF_MATCH_HL=y"
    )
    for config in "${EXTRA_CONFIGS[@]}"; do
        echo "$config" >> ${GKI_DEFCONFIG_ARM64}
        echo "$config" >> ${GKI_DEFCONFIG_X86}
    done

    # 为 arm64 添加 BBR 配置
    BBR_CONFIGS=(
        "CONFIG_TCP_CONG_ADVANCED=y"
        "CONFIG_TCP_CONG_BBR=y"
        "CONFIG_NET_SCH_FQ=y"
        "CONFIG_TCP_CONG_BIC=n"
        "CONFIG_TCP_CONG_WESTWOOD=n"
        "CONFIG_TCP_CONG_HTCP=n"
    )
    for config in "${BBR_CONFIGS[@]}"; do
        echo "$config" >> ${GKI_DEFCONFIG_ARM64}
    done

    # 为内核添加 FTRACE 配置
    FTRACE_CONFIGS=(
        "CONFIG_FUNCTION_TRACER=y"
        "CONFIG_FUNCTION_GRAPH_TRACER=y"
        "CONFIG_STACK_TRACER=y"
        "CONFIG_DYNAMIC_FTRACE=y"
        "CONFIG_FTRACE_SYSCALLS=y"
    )
    
    # 移除 check_defconfig
    sed -i 's/check_defconfig//' ./common/build.config.gki
    
    # 修改内核名称
    perl -pi -e 's{UTS_VERSION="\$\(echo \$UTS_VERSION \$CONFIG_FLAGS \$TIMESTAMP \| cut -b -\$UTS_LEN\)"}{UTS_VERSION="#1 SMP PREEMPT Sat Apr 20 04:20:00 UTC 2024"}' ./common/scripts/mkcompile_h
    
    sed -i '/^[[:space:]]*"protected_exports_list"[[:space:]]*:[[:space:]]*"android\/abi_gki_protected_exports_aarch64",$/d' ./common/BUILD.bazel
    rm -rf ./common/android/abi_gki_protected_exports_*
    sed -i "/stable_scmversion_cmd/s/-maybe-dirty//g" ./build/kernel/kleaf/impl/stamp.bzl
    echo "内核配置已完成。"

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
build-cvd-kernel-aarch64:
    #!/bin/bash
    echo "正在编译 aarch64 CVD 内核..."
    cd {{CONFIG}}
    
    set -e
    set -x
    
    tools/bazel build --disk_cache=$HOME/.cache/bazel --config=fast --lto=thin //common-modules/virtual-device:virtual_device_aarch64_dist
    
    ccache --show-stats

# 构建CVD内核
build-cvd-kernel-x86_64:
    #!/bin/bash
    echo "正在编译 x86_64 CVD 内核..."
    cd {{CONFIG}}
    
    set -e
    set -x
    
    tools/bazel build --disk_cache=$HOME/.cache/bazel --config=fast --lto=thin //common-modules/virtual-device:virtual_device_x86_64_dist
    
    ccache --show-stats
    echo "编译完成！"

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

# 构建x86_64 CVD内核
cook-cvd: setup download-gki build-cvd-kernel-x86_64
    @echo ""
    @echo "构建 x86_64 模拟器内核完成！"
    @echo ""

# 构建x86_64 KernelSU CVD内核
cook-cvd-ksu: setup download-gki apply-kernelsu apply-susfs apply-patches configure build-cvd-kernel-x86_64
    @echo ""
    @echo "构建 x86_64 KernelSU 模拟器内核完成！"
    @echo ""

# 构建aarch64 CVD内核
cook-cvd-aarch64: setup download-gki build-cvd-kernel-aarch64
    @echo ""
    @echo "构建 aarch64 模拟器内核完成！"
    @echo ""

# 构建gki内核并打包
cook-gki: setup download-gki build-gki create-bootimg create-anykernel
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

# 构建kprobe模块
kprobe:
    #!/bin/bash
    @echo "正在编译 kprobe 模块..."
    cd modules/kprobe && make clean && make
    echo "kprobe 模块编译完成。"

# 构建ftrace模块
ftrace:
    #!/bin/bash
    @echo "正在编译 ftrace 模块..."
    cd modules/ftrace && make clean && make
    echo "ftrace 模块编译完成。"

workspace:
    #!/bin/bash
    set -e
    echo "正在配置内核..."
    cd {{CONFIG}}
    # 判断当前目录的WORKSPACE文件是否有modules目录的引入，没有就添加
    if ! grep -q "local_repository(" WORKSPACE; then
        echo "" >> WORKSPACE
        echo "local_repository(" >> WORKSPACE
        echo "    name = \"external_modules\"," >> WORKSPACE
        echo "    path = \"../modules\"," >> WORKSPACE
        echo ")" >> WORKSPACE
    else
        echo "WORKSPACE文件已包含modules目录的引入，跳过。"
    fi
    
    echo "WORKSPACE配置完成。"

# 清理内核模块
clean-mod:
    #!/bin/bash
    set -e
    echo "正在清理模块..."
    for dir in hello cpuinfo kprobe ftrace; do
        if [ -d "modules/$dir" ]; then
            echo "清理 $dir 模块..."
            cd modules/$dir
            make clean KERNEL_SRC=/lib/modules/$(uname -r)/build
            cd ../..
        else
            echo "$dir 模块目录不存在，跳过。"
        fi
    done
    echo "模块清理完成。"

# 编译 hello 内核模块
hello:
    #!/bin/bash
    set -e
    echo "正在编译内核模块..."
    cd modules/hello
    make clean KERNEL_SRC=/lib/modules/$(uname -r)/build
    make  KERNEL_SRC=/lib/modules/$(uname -r)/build
    echo "内核模块编译完成。"

# 测试 hello 内核模块
hello-test: hello
    #!/bin/bash
    set -e
    echo "正在测试 hello 内核模块..."
    cd modules/hello
    sudo insmod hello.ko
    if [ $? -ne 0 ]; then
        echo "hello 内核模块加载失败，请检查日志。"
        exit 1
    fi
    echo "hello 内核模块加载成功。"
    sudo dmesg | tail -n 20
    sudo rmmod hello
    if [ $? -ne 0 ]; then
        echo "hello 内核模块卸载失败，请检查日志。"
        exit 1
    fi
    echo "hello 内核模块卸载成功。"

# 编译 hello 内核模块（GKI x86_64）
hello-gki-x86_64:
    #!/bin/bash
    set -e
    echo "正在编译GKI内核模块..."
    cd {{CONFIG}}
    DIST_HELLO_DIR=./common/hello
    rm -rf $DIST_HELLO_DIR
    cp -r -f ../modules/hello $DIST_HELLO_DIR
    tools/bazel build --disk_cache=$HOME/.cache/bazel --config=fast //common/hello:hello
    tree -f . | grep hello.ko
    rm -rf $DIST_HELLO_DIR
    echo "内核模块编译完成。"

# 编译 hello 内核模块（CVD x86_64）
hello-cvd-x86_64:
    #!/bin/bash
    set -e
    echo "正在编译CVD内核模块..."
    cd {{CONFIG}}
    DIST_HELLO_DIR=./common-modules/virtual-device/hello
    rm -rf $DIST_HELLO_DIR
    cp -r -f ../modules/hello $DIST_HELLO_DIR
    tools/bazel build --disk_cache=$HOME/.cache/bazel --config=fast //common-modules/virtual-device:virtual_device_x86_64_dist
    # tree -f . | grep hello.ko
    rm -rf $DIST_HELLO_DIR
    echo "内核模块编译完成。"

# 编译 hello 内核模块（CVD x86_64）
hello-cvd-x86_642:
    #!/bin/bash
    set -e
    echo "正在编译CVD内核模块..."
    cd {{CONFIG}}
    DIST_HELLO_DIR=./common-modules/virtual-device/hello
    rm -rf $DIST_HELLO_DIR
    cp -r -f ../modules/hello $DIST_HELLO_DIR
    mv -f $DIST_HELLO_DIR/BUILD.cf.bazel $DIST_HELLO_DIR/BUILD.bazel
    tools/bazel build --disk_cache=$HOME/.cache/bazel --config=fast //common-modules/virtual-device/hello:hello
    # tree -f . | grep hello.ko
    rm -rf $DIST_HELLO_DIR
    echo "内核模块编译完成。"
