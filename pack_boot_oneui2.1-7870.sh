#!/usr/bin/env bash
set -euo pipefail

KDIR="${KDIR:-/home/graydesigner/Desktop/Thao_mai/oneui2.1-7870}"
OUT="${OUT:-$KDIR/out}"
DEFCONFIG="${DEFCONFIG:-exynos7870-j7y17lte_defconfig}"

ANDROID_MAJOR_VERSION="${ANDROID_MAJOR_VERSION:-q}"
ANDROID_PLATFORM_VERSION="${ANDROID_PLATFORM_VERSION:-10}"

TC_PREFIX="${TC_PREFIX:-/home/graydesigner/Desktop/Thao_mai/SM-G960F_MEA_QQ_Opensource/Kernel/toolchain/gcc-cfp/gcc-ibv-jopp/aarch64-linux-android-4.9/bin/aarch64-linux-android-}"

BASE_BOOT_IMG="${BASE_BOOT_IMG:-$KDIR/modules/boot.img}"
OUT_BOOT_IMG="${OUT_BOOT_IMG:-$KDIR/modules/boot-new.img}"
KERNEL_IMAGE_NAME="${KERNEL_IMAGE_NAME:-Image}"

KSU_VERSION_OVERRIDE="${KSU_VERSION_OVERRIDE:-33294}"
KSU_VERSION_TAG_OVERRIDE="${KSU_VERSION_TAG_OVERRIDE:-v3.4.0}"
KSU_CLAMP_VERSION="${KSU_CLAMP_VERSION:-33294}"

WORKSPACE="${WORKSPACE:-$(dirname "$KDIR")}"
AIK_DIR="${AIK_DIR:-}"
BUILD_MODE="${BUILD_MODE:-auto}"
BUILD_DIR=""

die() { echo "ERROR: $*" >&2; exit 1; }
need_file() { [ -f "$1" ] || die "Missing file: $1"; }
need_exec() { [ -x "$1" ] || die "Missing executable: $1"; }

resolve_build_dir() {
  case "$BUILD_MODE" in
    in_tree)
      BUILD_DIR="$KDIR"
      ;;
    out_dir)
      BUILD_DIR="$OUT"
      mkdir -p "$OUT"
      ;;
    auto)
      if [ -f "$KDIR/.config" ] || [ -d "$KDIR/include/config" ]; then
        BUILD_DIR="$KDIR"
      else
        BUILD_DIR="$OUT"
        mkdir -p "$OUT"
      fi
      ;;
    *)
      die "Unsupported BUILD_MODE: $BUILD_MODE (use auto|in_tree|out_dir)"
      ;;
  esac
}

run_make() {
  if [ "$BUILD_DIR" = "$KDIR" ]; then
    make -C "$KDIR" "$@"
  else
    make -C "$KDIR" O="$BUILD_DIR" "$@"
  fi
}

resolve_aik() {
  if [ -n "$AIK_DIR" ] && [ -x "$AIK_DIR/unpackimg.sh" ] && [ -x "$AIK_DIR/repackimg.sh" ]; then
    return 0
  fi

  for d in \
    "$WORKSPACE/exynox9810_S9/Apollo/A.I.K" \
    "$WORKSPACE/Exynox_9810_ONEUI7/Apollo/A.I.K" \
    "$WORKSPACE/Cronos_8890/Cronos/A.I.K"; do
    if [ -x "$d/unpackimg.sh" ] && [ -x "$d/repackimg.sh" ]; then
      AIK_DIR="$d"
      return 0
    fi
  done

  while IFS= read -r rp; do
    d="$(dirname "$rp")"
    if [ -x "$d/unpackimg.sh" ] && [ -x "$d/repackimg.sh" ]; then
      AIK_DIR="$d"
      return 0
    fi
  done < <(find "$WORKSPACE" -path '*/A.I.K/repackimg.sh' 2>/dev/null | sort)

  die "Cannot locate Android Image Kitchen (A.I.K). Set AIK_DIR=/path/to/A.I.K"
}

build_kernel() {
  export ARCH=arm64
  export SUBARCH=arm64
  export ANDROID_MAJOR_VERSION
  export ANDROID_PLATFORM_VERSION
  export CROSS_COMPILE="$TC_PREFIX"
  export KSU_VERSION_OVERRIDE KSU_VERSION_TAG_OVERRIDE KSU_CLAMP_VERSION

  resolve_build_dir

  echo "Build mode: $BUILD_MODE"
  echo "Build dir : $BUILD_DIR"
  echo "KSU       : ${KSU_VERSION_OVERRIDE} ${KSU_VERSION_TAG_OVERRIDE}"

  run_make ARCH=arm64 "$DEFCONFIG"
  run_make -j"$(nproc)" ARCH=arm64 Image Image.gz dtb.img
}

pack_boot() {
  resolve_build_dir
  resolve_aik
  need_file "$BASE_BOOT_IMG"
  need_file "$BUILD_DIR/arch/arm64/boot/$KERNEL_IMAGE_NAME"
  need_file "$BUILD_DIR/arch/arm64/boot/dtb.img"
  need_exec "$AIK_DIR/unpackimg.sh"
  need_exec "$AIK_DIR/repackimg.sh"

  workdir="$(mktemp -d "$KDIR/.pack_boot.XXXXXX")"
  trap 'rm -rf "$workdir"' EXIT

  cp -a "$AIK_DIR/." "$workdir/"
  cp -f "$BASE_BOOT_IMG" "$workdir/boot.img"

  (
    cd "$workdir"
    bash ./unpackimg.sh --nosudo boot.img >/dev/null
    cp -f "$BUILD_DIR/arch/arm64/boot/$KERNEL_IMAGE_NAME" split_img/boot.img-zImage
    cp -f "$BUILD_DIR/arch/arm64/boot/dtb.img" split_img/boot.img-dtb
    bash ./repackimg.sh --original >/dev/null
  )

  cp -f "$workdir/image-new.img" "$OUT_BOOT_IMG"
  chmod 0644 "$OUT_BOOT_IMG" || true
  versioned="${OUT_BOOT_IMG%.img}-ksunext-v3.4.0-33294.img"
  if [ "$versioned" != "$OUT_BOOT_IMG" ]; then
    cp -f "$OUT_BOOT_IMG" "$versioned"
    chmod 0644 "$versioned" || true
  fi
  echo "OK: $OUT_BOOT_IMG"
  ls -la "$OUT_BOOT_IMG" "$versioned" 2>/dev/null || true
  sha256sum "$OUT_BOOT_IMG"
  trap - EXIT
  rm -rf "$workdir"
}

usage() {
  cat <<EOF
Usage:
  $0 build
  $0 pack
  $0 all

Env overrides:
  KDIR=... OUT=... DEFCONFIG=...
  BUILD_MODE=auto|in_tree|out_dir
  KERNEL_IMAGE_NAME=Image|Image.gz
  TC_PREFIX=... (CROSS_COMPILE prefix, ending with '-') 
  BASE_BOOT_IMG=... OUT_BOOT_IMG=...
  AIK_DIR=... (path to A.I.K)
EOF
}

cmd="${1:-all}"
case "$cmd" in
  build) build_kernel ;;
  pack) pack_boot ;;
  all) build_kernel; pack_boot ;;
  *) usage; exit 2 ;;
esac
