# Android 13 GKI 5.15 Kernel Build Script with KernelSU Support

# Default configuration
ANDROID_VERSION := "android13"
KERNEL_VERSION := "5.15"
SUB_LEVEL := "167"
OS_PATCH_LEVEL := "2024-11"
KERNELSU_VARIANT := "KSU"  # Options: KSU, KSU_NEXT, MKSU
KERNELSU_BRANCH := "Stable"  # Options: Stable, Dev, Other
INCLUDE_SUSFS := "true"

# Directories
WORKSPACE := justfile_directory()
CONFIG := ANDROID_VERSION + "-" + KERNEL_VERSION + "-" + SUB_LEVEL
TOOLCHAIN_DIR := WORKSPACE + "/kernel-build-tools"
MKBOOTIMG_DIR := WORKSPACE + "/mkbootimg"

# Environment variables
export https_proxy := env_var_or_default("https_proxy", "")
export HTTP_PROXY := env_var_or_default("HTTP_PROXY", "")
export CCACHE_COMPILERCHECK := "%compiler% -dumpmachine; %compiler% -dumpversion"
export CCACHE_NOHASHDIR := "true"
export CCACHE_HARDLINK := "true"
export CCACHE_DIR := env_var_or_default("HOME", "") + "/.ccache"

default:
    @just --list

# Install dependencies and setup environment
setup:
    #!/bin/bash
    echo "Setting up build environment..."
    
    # Install system dependencies
    sudo apt update
    sudo apt install -y ccache curl git python3 python3-pip build-essential bc bison flex libssl-dev libelf-dev
    
    # Setup ccache
    mkdir -p ~/.cache/bazel
    ccache --version
    ccache --max-size=2G
    ccache --set-config=compression=true
    
    # Download toolchain if not exists
    just download-toolchain
    
    # Install repo tool
    mkdir -p {{WORKSPACE}}/git-repo
    if [ ! -f {{WORKSPACE}}/git-repo/repo ]; then
        echo "Downloading repo tool..."
        curl https://storage.googleapis.com/git-repo-downloads/repo > {{WORKSPACE}}/git-repo/repo
        chmod a+rx {{WORKSPACE}}/git-repo/repo
    fi
    
    echo "Setup completed!"

# Download toolchain
download-toolchain:
    #!/bin/bash
    if [ -d "{{TOOLCHAIN_DIR}}" ] && [ -d "{{MKBOOTIMG_DIR}}" ]; then
        echo "Toolchain already exists, skipping download"
        exit 0
    fi
    echo "Downloading toolchain..."
    AOSP_MIRROR=https://android.googlesource.com
    BRANCH=main-kernel-build-2024
    git clone $AOSP_MIRROR/kernel/prebuilts/build-tools -b $BRANCH --depth 1 {{TOOLCHAIN_DIR}}
    git clone $AOSP_MIRROR/platform/system/tools/mkbootimg -b $BRANCH --depth 1 {{MKBOOTIMG_DIR}}

# Download dependencies (AnyKernel3, SUSFS, kernel patches)
download-deps:
    #!/bin/bash
    if [ -d "AnyKernel3" ] && [ -d "susfs4ksu" ] && [ -d "kernel_patches" ]; then
        echo "Dependencies already downloaded, skipping."
        exit 0
    fi
    echo "Downloading dependencies..."
    # Clone AnyKernel3
    if [ ! -d "AnyKernel3" ]; then
        git clone https://github.com/ukriu/AnyKernel3.git -b gki --depth 1
    fi
    # Clone SUSFS
    if [ ! -d "susfs4ksu" ]; then
        git clone https://gitlab.com/simonpunk/susfs4ksu.git -b gki-{{ANDROID_VERSION}}-{{KERNEL_VERSION}} --depth 1
    fi
    # Clone kernel patches
    if [ ! -d "kernel_patches" ]; then
        git clone https://github.com/ukriu/kernel_patches.git --depth 1
    fi

# Download GKI kernel source
download-gki: download-deps
    #!/bin/bash
    if [ -d "{{CONFIG}}/.repo" ]; then
        echo "GKI kernel source already downloaded, skipping."
        exit 0
    fi
    echo "Downloading GKI kernel source..."
    # Create config directory
    mkdir -p {{CONFIG}}
    cd {{CONFIG}}
    # Initialize repo
    FORMATTED_BRANCH="{{ANDROID_VERSION}}-{{KERNEL_VERSION}}-{{OS_PATCH_LEVEL}}"
    {{WORKSPACE}}/git-repo/repo init --depth=1 --u https://android.googlesource.com/kernel/manifest -b common-${FORMATTED_BRANCH} --repo-rev=v2.16
    # Check if branch is deprecated and update manifest
    REMOTE_BRANCH=$(git ls-remote https://android.googlesource.com/kernel/common ${FORMATTED_BRANCH})
    if echo "$REMOTE_BRANCH" | grep -q deprecated; then
        echo "Found deprecated branch: $FORMATTED_BRANCH"
        sed -i "s/\"${FORMATTED_BRANCH}\"/\"deprecated\/${FORMATTED_BRANCH}\"/g" .repo/manifests/default.xml
    fi
    # Sync repo
    {{WORKSPACE}}/git-repo/repo sync -c -j$(nproc --all) --no-tags --fail-fast

# Apply KernelSU patches
apply-kernelsu:
    #!/bin/bash
    echo "Applying KernelSU patches..."
    cd {{CONFIG}}
    
    # Determine branch
    if [[ "{{KERNELSU_BRANCH}}" == "Stable" ]]; then
        BRANCH=""
    elif [[ "{{KERNELSU_BRANCH}}" == "Dev" && ( "{{KERNELSU_VARIANT}}" == "KSU" || "{{KERNELSU_VARIANT}}" == "MKSU" ) ]]; then
        BRANCH="-s main"
    elif [[ "{{KERNELSU_BRANCH}}" == "Dev" && "{{KERNELSU_VARIANT}}" == "KSU_NEXT" ]]; then
        BRANCH="-s next"
    fi
    
    # Apply KernelSU
    if [[ "{{KERNELSU_VARIANT}}" == "KSU" ]]; then
        echo "Adding KernelSU Official..."
        curl -LSs "https://raw.githubusercontent.com/tiann/KernelSU/main/kernel/setup.sh" | bash $BRANCH
    elif [[ "{{KERNELSU_VARIANT}}" == "KSU_NEXT" ]]; then
        echo "Adding KernelSU Next..."
        curl -LSs "https://raw.githubusercontent.com/KernelSU-Next/KernelSU-Next/next/kernel/setup.sh" | bash $BRANCH
    elif [[ "{{KERNELSU_VARIANT}}" == "MKSU" ]]; then
        echo "Adding KernelSU MKSU..."
        curl -LSs "https://raw.githubusercontent.com/5ec1cff/KernelSU/main/kernel/setup.sh" | bash $BRANCH
    fi

# Apply SUSFS patches
apply-susfs:
    #!/bin/bash
    if [[ "{{INCLUDE_SUSFS}}" != "true" ]]; then
        echo "SUSFS disabled, skipping patches"
        exit 0
    fi
    
    echo "Applying SUSFS patches..."
    cd {{CONFIG}}
    
    # Copy SUSFS patches
    cp ../susfs4ksu/kernel_patches/50_add_susfs_in_gki-{{ANDROID_VERSION}}-{{KERNEL_VERSION}}.patch ./common/
    cp ../susfs4ksu/kernel_patches/fs/* ./common/fs/
    cp ../susfs4ksu/kernel_patches/include/linux/* ./common/include/linux/
    
    # Apply variant-specific patches
    if [[ "{{KERNELSU_VARIANT}}" == "KSU" ]]; then
        echo "Applying SUSFS patches for Official KernelSU..."
        cd ./KernelSU
        cp ../../susfs4ksu/kernel_patches/KernelSU/10_enable_susfs_for_ksu.patch ./
        patch -p1 --forward --fuzz=3 < 10_enable_susfs_for_ksu.patch
    elif [[ "{{KERNELSU_VARIANT}}" == "KSU_NEXT" ]]; then
        echo "Applying SUSFS patches for KernelSU-Next..."
        cd ./KernelSU-Next
        cp ../../kernel_patches/next/kernel-patch-susfs-v1.5.7-to-KernelSU-Next.patch ./
        patch -p1 --forward --fuzz=3 < kernel-patch-susfs-v1.5.7-to-KernelSU-Next.patch || true
    elif [[ "{{KERNELSU_VARIANT}}" == "MKSU" ]]; then
        echo "Applying SUSFS patches for MKSU..."
        cd ./KernelSU
        cp ../../susfs4ksu/kernel_patches/KernelSU/10_enable_susfs_for_ksu.patch ./
        patch -p1 --forward --fuzz=3 < 10_enable_susfs_for_ksu.patch || true
    fi
    
    cd ../common
    patch -p1 --fuzz=3 < 50_add_susfs_in_gki-{{ANDROID_VERSION}}-{{KERNEL_VERSION}}.patch || true

# Apply additional patches
apply-patches:
    #!/bin/bash
    echo "Applying additional patches..."
    cd {{CONFIG}}
    
    # Apply All Managers patch for KSU
    if [[ "{{KERNELSU_VARIANT}}" == "KSU" ]]; then
        cd KernelSU
        if [[ "{{INCLUDE_SUSFS}}" == "true" ]]; then
            cp ../../kernel_patches/apk_sign.c_fix.patch ./
            patch -p1 --fuzz=3 < apk_sign.c_fix.patch
        else
            cp ../../kernel_patches/no-susfs_apk_sign.c_fix.patch ./
            patch -p1 --fuzz=3 < no-susfs_apk_sign.c_fix.patch
        fi
        cd ..
    fi
    
    # Apply hooks patches
    cd common
    if [[ "{{KERNELSU_VARIANT}}" == "KSU_NEXT" ]]; then
        echo "Applying hooks for KernelSU-Next..."
        cp ../../kernel_patches/next/syscall_hooks.patch ./
        patch -p1 -F 3 < syscall_hooks.patch
    fi
    
    # Apply hide stuff patches
    cp ../../kernel_patches/69_hide_stuff.patch ./
    patch -p1 -F 3 < 69_hide_stuff.patch

# Configure kernel
configure:
    #!/bin/bash
    echo "Configuring kernel..."
    cd {{CONFIG}}
    
    # Add KSU configuration
    echo "CONFIG_KSU=y" >> ./common/arch/arm64/configs/gki_defconfig
    
    if [[ "{{KERNELSU_VARIANT}}" == "KSU_NEXT" ]]; then
        echo "CONFIG_KSU_WITH_KPROBES=n" >> ./common/arch/arm64/configs/gki_defconfig
    fi
    
    # Add SUSFS configuration if enabled
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
    
    # Add additional configurations
    echo "CONFIG_TMPFS_XATTR=y" >> ./common/arch/arm64/configs/gki_defconfig
    echo "CONFIG_TMPFS_POSIX_ACL=y" >> ./common/arch/arm64/configs/gki_defconfig
    echo "CONFIG_IP_NF_TARGET_TTL=y" >> ./common/arch/arm64/configs/gki_defconfig
    echo "CONFIG_IP6_NF_TARGET_HL=y" >> ./common/arch/arm64/configs/gki_defconfig
    echo "CONFIG_IP6_NF_MATCH_HL=y" >> ./common/arch/arm64/configs/gki_defconfig
    
    # Add BBR Config
    echo "CONFIG_TCP_CONG_ADVANCED=y" >> ./common/arch/arm64/configs/gki_defconfig
    echo "CONFIG_TCP_CONG_BBR=y" >> ./common/arch/arm64/configs/gki_defconfig
    echo "CONFIG_NET_SCH_FQ=y" >> ./common/arch/arm64/configs/gki_defconfig
    echo "CONFIG_TCP_CONG_BIC=n" >> ./common/arch/arm64/configs/gki_defconfig
    echo "CONFIG_TCP_CONG_WESTWOOD=n" >> ./common/arch/arm64/configs/gki_defconfig
    echo "CONFIG_TCP_CONG_HTCP=n" >> ./common/arch/arm64/configs/gki_defconfig
    
    # Remove check_defconfig
    sed -i 's/check_defconfig//' ./common/build.config.gki
    
    # Change kernel name
    perl -pi -e 's{UTS_VERSION="\$\(echo \$UTS_VERSION \$CONFIG_FLAGS \$TIMESTAMP \| cut -b -\$UTS_LEN\)"}{UTS_VERSION="#1 SMP PREEMPT Sat Apr 20 04:20:00 UTC 2024"}' ./common/scripts/mkcompile_h
    sed -i '$s|echo "\$res"|echo "\$res-🍻-blehSU-🍻"|' ./common/scripts/setlocalversion
    
    if [ -f "build/build.sh" ]; then
        sed -i 's/-dirty//' ./common/scripts/setlocalversion
    else
        sed -i '/^[[:space:]]*"protected_exports_list"[[:space:]]*:[[:space:]]*"android\/abi_gki_protected_exports_aarch64",$/d' ./common/BUILD.bazel
        rm -rf ./common/android/abi_gki_protected_exports_*
        sed -i "/stable_scmversion_cmd/s/-maybe-dirty//g" ./build/kernel/kleaf/impl/stamp.bzl
        sed -E -i '/^CONFIG_LOCALVERSION=/ s/(.*)"$/\1-🍻-blehSU-🍻"/' ./common/arch/arm64/configs/gki_defconfig
    fi

# Build kernel
build:
    #!/bin/bash
    echo "Building kernel..."
    cd {{CONFIG}}
    
    set -e
    set -x
    
    if [ -f "build/build.sh" ]; then
        LTO=thin BUILD_CONFIG=common/build.config.gki.aarch64 build/build.sh CC="/usr/bin/ccache clang"
    else
        tools/bazel build --disk_cache=$HOME/.cache/bazel --config=fast --lto=thin //common:kernel_aarch64_dist
    fi
    
    ccache --show-stats

# Create boot images
create-bootimg:
    #!/bin/bash
    echo "Creating boot images..."
    
    # Create bootimgs folder
    mkdir -p bootimgs
    
    cd {{CONFIG}}
    
    if [ -f "build/build.sh" ]; then
        # For build.sh method
        cp ./out/{{ANDROID_VERSION}}-{{KERNEL_VERSION}}/dist/Image ../bootimgs/
        cp ./out/{{ANDROID_VERSION}}-{{KERNEL_VERSION}}/dist/Image.lz4 ../bootimgs/
        cp ./out/{{ANDROID_VERSION}}-{{KERNEL_VERSION}}/dist/Image ../
        cp ./out/{{ANDROID_VERSION}}-{{KERNEL_VERSION}}/dist/Image.lz4 ../
    else
        # For bazel method
        cp ./bazel-bin/common/kernel_aarch64/Image ../bootimgs/
        cp ./bazel-bin/common/kernel_aarch64/Image.lz4 ../bootimgs/
        cp ./bazel-bin/common/kernel_aarch64/Image ../
        cp ./bazel-bin/common/kernel_aarch64/Image.lz4 ../
    fi
    
    cd ..
    
    # Create gzip of the Image file
    gzip -n -k -f -9 ./Image > ./Image.gz
    
    cd bootimgs
    gzip -n -k -f -9 ./Image > ./Image.gz
    
    # Create boot images for Android 13
    echo "Building boot.img"
    {{MKBOOTIMG_DIR}}/mkbootimg.py --header_version 4 --kernel Image --output boot.img
    {{TOOLCHAIN_DIR}}/linux-x86/bin/avbtool add_hash_footer --partition_name boot --partition_size $((64 * 1024 * 1024)) --image boot.img --algorithm SHA256_RSA2048 --key {{TOOLCHAIN_DIR}}/linux-x86/share/avb/testkey_rsa2048.pem
    cp ./boot.img ../{{KERNELSU_VARIANT}}_{{ANDROID_VERSION}}-{{KERNEL_VERSION}}.{{SUB_LEVEL}}-{{OS_PATCH_LEVEL}}-boot.img
    
    echo "Building boot-gz.img"
    {{MKBOOTIMG_DIR}}/mkbootimg.py --header_version 4 --kernel Image.gz --output boot-gz.img
    {{TOOLCHAIN_DIR}}/linux-x86/bin/avbtool add_hash_footer --partition_name boot --partition_size $((64 * 1024 * 1024)) --image boot-gz.img --algorithm SHA256_RSA2048 --key {{TOOLCHAIN_DIR}}/linux-x86/share/avb/testkey_rsa2048.pem
    cp ./boot-gz.img ../{{KERNELSU_VARIANT}}_{{ANDROID_VERSION}}-{{KERNEL_VERSION}}.{{SUB_LEVEL}}-{{OS_PATCH_LEVEL}}-boot-gz.img
    
    echo "Building boot-lz4.img"
    {{MKBOOTIMG_DIR}}/mkbootimg.py --header_version 4 --kernel Image.lz4 --output boot-lz4.img
    {{TOOLCHAIN_DIR}}/linux-x86/bin/avbtool add_hash_footer --partition_name boot --partition_size $((64 * 1024 * 1024)) --image boot-lz4.img --algorithm SHA256_RSA2048 --key {{TOOLCHAIN_DIR}}/linux-x86/share/avb/testkey_rsa2048.pem
    cp ./boot-lz4.img ../{{KERNELSU_VARIANT}}_{{ANDROID_VERSION}}-{{KERNEL_VERSION}}.{{SUB_LEVEL}}-{{OS_PATCH_LEVEL}}-boot-lz4.img

# Create AnyKernel3 flashable zip
create-anykernel:
    #!/bin/bash
    ZIP_NAME="{{KERNELSU_VARIANT}}_{{ANDROID_VERSION}}-{{KERNEL_VERSION}}.{{SUB_LEVEL}}-{{OS_PATCH_LEVEL}}-AnyKernel3.zip"
    if [ -f "../$ZIP_NAME" ]; then
        echo "$ZIP_NAME already exists, skipping."
        exit 0
    fi
    echo "Creating AnyKernel3 flashable zip..."
    cd AnyKernel3 && rm -rf .git/
    echo "Creating zip file: $ZIP_NAME..."
    cp ../Image ./Image
    zip -r "../$ZIP_NAME" ./*
    rm ./Image

# Compress images
compress-images:
    @echo "Compressing boot images..."
    for image in *.img; do \
        if [ -f "$image" ]; then \
            gzip -vnf9 "$image"; \
        fi \
    done

# Clean build artifacts
clean:
    @echo "Cleaning build artifacts..."
    rm -rf bootimgs
    rm -f *.img *.img.gz *.zip Image Image.gz Image.lz4

# Clean everything including dependencies
clean-all: clean
    @echo "Cleaning all files..."
    rm -rf kernel-build-tools mkbootimg git-repo
    rm -rf AnyKernel3 susfs4ksu kernel_patches

# Complete build process
cook: setup download-gki apply-kernelsu apply-susfs apply-patches configure build create-bootimg create-anykernel compress-images
    @echo ""
    @echo "🎉 Build completed successfully!"
    @echo ""
    @echo "Generated files:"
    @echo "  - {{KERNELSU_VARIANT}}_{{ANDROID_VERSION}}-{{KERNEL_VERSION}}.{{SUB_LEVEL}}-{{OS_PATCH_LEVEL}}-AnyKernel3.zip"
    @echo "  - {{KERNELSU_VARIANT}}_{{ANDROID_VERSION}}-{{KERNEL_VERSION}}.{{SUB_LEVEL}}-{{OS_PATCH_LEVEL}}-boot*.img.gz"
    @echo ""
    @echo "Flash the AnyKernel3 zip via custom recovery or use boot images with fastboot."

# Show build status
status:
    @echo "Build Status:"
    @echo "============="
    @if [ -d "{{CONFIG}}" ]; then echo "✓ GKI source downloaded"; else echo "✗ GKI source not found"; fi
    @if [ -d "kernel-build-tools" ]; then echo "✓ Toolchain downloaded"; else echo "✗ Toolchain not found"; fi
    @if [ -d "AnyKernel3" ]; then echo "✓ AnyKernel3 downloaded"; else echo "✗ AnyKernel3 not found"; fi
    @if [ -d "susfs4ksu" ]; then echo "✓ SUSFS downloaded"; else echo "✗ SUSFS not found"; fi
    @if [ -d "kernel_patches" ]; then echo "✓ Kernel patches downloaded"; else echo "✗ Kernel patches not found"; fi
    @echo ""
    @if [ -f "*.zip" ]; then echo "Built packages:"; ls -la *.zip 2>/dev/null || true; fi
    @if [ -f "*.img.gz" ]; then echo "Built images:"; ls -la *.img.gz 2>/dev/null || true; fi
