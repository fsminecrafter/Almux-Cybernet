#!/usr/bin/env bash
# =============================================================================
# build_and_program.sh
# Builds and programs the dual-core board.
#
# Project layout (relative to this script):
#   software/io/IOmanager.c       ATmega328PB IO bridge firmware
#   software/io/Makefile          AVR build rules
#   software/io/programmer.py     Python flash programmer
#   software/main/CMakeLists.txt  RP2350B dual-core CMake project
#   software/main/core_main.c     CORE MAIN firmware
#   software/main/core_video.c    CORE VIDEO firmware
#   software/main/shared_protocol.h
#   software/main/font8x8.h
#   software/main/tusb_config.h
#
# Usage:
#   ./build_and_program.sh [OPTIONS]
#
# Options:
#   --port   PORT     Serial port for ATmega programmer (default: /dev/ttyUSB0)
#   --skip-deps       Skip dependency installation
#   --build-only      Build but do not flash anything
#   --flash-io        Flash only the ATmega IO manager
#   --flash-main      Flash only CORE MAIN RP2350B
#   --flash-video     Flash only CORE VIDEO RP2350B
#   --flash-all       Flash ATmega + both RP2350Bs (default when not build-only)
#   --clean           Remove all build artefacts before building
#   -h, --help        Show this help
#
# Example — build everything and flash all three chips:
#   ./build_and_program.sh --port /dev/ttyUSB0 --flash-all
#
# Example — build only:
#   ./build_and_program.sh --build-only
# =============================================================================
set -euo pipefail

# ---- colour output ----------------------------------------------------------
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

info()    { echo -e "${CYAN}[INFO]${NC}  $*"; }
success() { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error()   { echo -e "${RED}[ERROR]${NC} $*" >&2; }
die()     { error "$*"; exit 1; }

# ---- resolve script location ------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IO_DIR="${SCRIPT_DIR}/software/io"
MAIN_DIR="${SCRIPT_DIR}/software/main"
BUILD_DIR="${MAIN_DIR}/build"

# ---- defaults ---------------------------------------------------------------
PORT="/dev/ttyUSB0"
SKIP_DEPS=false
BUILD_ONLY=false
DO_FLASH_IO=false
DO_FLASH_MAIN=false
DO_FLASH_VIDEO=false
CLEAN=false

# ---- parse arguments --------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --port)       PORT="$2";         shift 2 ;;
        --skip-deps)  SKIP_DEPS=true;    shift   ;;
        --build-only) BUILD_ONLY=true;   shift   ;;
        --flash-io)   DO_FLASH_IO=true;  shift   ;;
        --flash-main) DO_FLASH_MAIN=true; shift  ;;
        --flash-video)DO_FLASH_VIDEO=true; shift ;;
        --flash-all)
            DO_FLASH_IO=true
            DO_FLASH_MAIN=true
            DO_FLASH_VIDEO=true
            shift ;;
        --clean)      CLEAN=true;        shift   ;;
        -h|--help)
            sed -n '/^# Usage:/,/^# ====/p' "$0" | grep -v '^# ====' | sed 's/^# \?//'
            exit 0 ;;
        *) die "Unknown option: $1  (use --help for usage)" ;;
    esac
done

# default: if no flash flags and not build-only, flash all
if ! $BUILD_ONLY && ! $DO_FLASH_IO && ! $DO_FLASH_MAIN && ! $DO_FLASH_VIDEO; then
    DO_FLASH_IO=true
    DO_FLASH_MAIN=true
    DO_FLASH_VIDEO=true
fi

# ---- banner -----------------------------------------------------------------
echo ""
echo -e "${BOLD}================================================${NC}"
echo -e "${BOLD}  Dual-Core Board — Build & Program Script      ${NC}"
echo -e "${BOLD}  Ubuntu 24.04  |  RP2350B + ATmega328PB        ${NC}"
echo -e "${BOLD}================================================${NC}"
echo ""
info "Serial port : ${PORT}"
info "Build only  : ${BUILD_ONLY}"
info "Flash IO    : ${DO_FLASH_IO}"
info "Flash MAIN  : ${DO_FLASH_MAIN}"
info "Flash VIDEO : ${DO_FLASH_VIDEO}"
echo ""

# =============================================================================
# STEP 1 — Install dependencies
# =============================================================================
install_deps() {
    info "Installing build dependencies..."

    sudo apt-get update -qq

    # AVR toolchain for ATmega328PB
    sudo apt-get install -y -qq \
        gcc-avr \
        avr-libc \
        avrdude

    # ARM toolchain + CMake for RP2350B
    sudo apt-get install -y -qq \
        cmake \
        gcc-arm-none-eabi \
        libnewlib-arm-none-eabi \
        libstdc++-arm-none-eabi-newlib \
        ninja-build \
        git \
        python3 \
        python3-pip \
        python3-serial

    # pyserial for programmer.py
    pip3 install --quiet pyserial

    success "Dependencies installed."
}

# =============================================================================
# STEP 2 — Clone pico-sdk and PicoDVI if not present
# =============================================================================
clone_sdk() {
    local SDK_DIR="${SCRIPT_DIR}/pico-sdk"
    local DVI_DIR="${SCRIPT_DIR}/PicoDVI"

    if [[ ! -d "${SDK_DIR}" ]]; then
        info "Cloning pico-sdk..."
        git clone --depth 1 --branch master \
            https://github.com/raspberrypi/pico-sdk.git \
            "${SDK_DIR}"
        cd "${SDK_DIR}"
        git submodule update --init --depth 1
        cd "${SCRIPT_DIR}"
        success "pico-sdk cloned."
    else
        info "pico-sdk found at ${SDK_DIR}"
    fi

    if [[ ! -d "${DVI_DIR}" ]]; then
        info "Cloning PicoDVI..."
        git clone --depth 1 \
            https://github.com/Wren6991/PicoDVI.git \
            "${DVI_DIR}"
        success "PicoDVI cloned."
    else
        info "PicoDVI found at ${DVI_DIR}"
    fi

    export PICO_SDK_PATH="${SDK_DIR}"
    export PICODVI_PATH="${DVI_DIR}"
}

# =============================================================================
# STEP 3 — Build ATmega IO manager
# =============================================================================
build_io() {
    info "Building ATmega328PB IO manager..."

    [[ -d "${IO_DIR}" ]] || die "IO source directory not found: ${IO_DIR}"

    if $CLEAN; then
        make -C "${IO_DIR}" clean 2>/dev/null || true
        info "IO build cleaned."
    fi

    # The Makefile uses F_CPU=12000000UL and MCU=atmega328pb
    # Check if avr-gcc supports atmega328pb (requires avr-libc >= 2.0.0)
    if ! avr-gcc --print-multi-lib 2>/dev/null | grep -q 'avr5'; then
        warn "avr-gcc may not fully support atmega328pb — checking..."
    fi

    make -C "${IO_DIR}" \
        MCU=atmega328pb \
        F_CPU=12000000UL \
        TARGET=IOmanager \
        2>&1 | sed 's/^/  /'

    [[ -f "${IO_DIR}/IOmanager.hex" ]] \
        || die "IO build failed — IOmanager.hex not produced."

    success "IO manager built: ${IO_DIR}/IOmanager.hex"
}

# =============================================================================
# STEP 4 — Build RP2350B firmware (both cores via CMake)
# =============================================================================
build_rp2350() {
    info "Building RP2350B firmware (CORE MAIN + CORE VIDEO)..."

    [[ -d "${MAIN_DIR}" ]] || die "Main source directory not found: ${MAIN_DIR}"

    export PICO_SDK_PATH="${PICO_SDK_PATH:-${SCRIPT_DIR}/pico-sdk}"
    export PICODVI_PATH="${PICODVI_PATH:-${SCRIPT_DIR}/PicoDVI}"

    [[ -d "${PICO_SDK_PATH}" ]] \
        || die "pico-sdk not found at ${PICO_SDK_PATH}. Run without --skip-deps to auto-clone."
    [[ -d "${PICODVI_PATH}" ]] \
        || die "PicoDVI not found at ${PICODVI_PATH}. Run without --skip-deps to auto-clone."

    if $CLEAN && [[ -d "${BUILD_DIR}" ]]; then
        rm -rf "${BUILD_DIR}"
        info "CMake build directory cleaned."
    fi

    mkdir -p "${BUILD_DIR}"

    info "Running CMake configure..."
    cmake -S "${MAIN_DIR}" \
          -B "${BUILD_DIR}" \
          -G Ninja \
          -DCMAKE_BUILD_TYPE=Release \
          -DPICO_SDK_PATH="${PICO_SDK_PATH}" \
          -DPICODVI_PATH="${PICODVI_PATH}" \
          -DPICO_BOARD=none \
          2>&1 | sed 's/^/  /'

    info "Building..."
    cmake --build "${BUILD_DIR}" \
          --config Release \
          --parallel "$(nproc)" \
          2>&1 | sed 's/^/  /'

    # Check outputs exist
    [[ -f "${BUILD_DIR}/core_main.bin" ]] \
        || die "CORE MAIN build failed — core_main.bin not found."
    [[ -f "${BUILD_DIR}/core_video.bin" ]] \
        || die "CORE VIDEO build failed — core_video.bin not found."

    success "CORE MAIN  built: ${BUILD_DIR}/core_main.bin"
    success "CORE VIDEO built: ${BUILD_DIR}/core_video.bin"

    # Print sizes
    info "Binary sizes:"
    ls -lh "${BUILD_DIR}/core_main.bin"  2>/dev/null | awk '{print "  core_main.bin  "$5}'
    ls -lh "${BUILD_DIR}/core_video.bin" 2>/dev/null | awk '{print "  core_video.bin "$5}'
}

# =============================================================================
# STEP 5 — Flash ATmega IO manager via avrdude (CH340N bootloader)
# =============================================================================
flash_io() {
    info "Flashing ATmega328PB IO manager via ${PORT}..."

    [[ -f "${IO_DIR}/IOmanager.hex" ]] \
        || die "IOmanager.hex not found — run build first."

    [[ -e "${PORT}" ]] \
        || die "Serial port ${PORT} not found. Is the board connected?"

    # Set fuses for 12MHz external crystal (from schematic: X1 = 8MHz)
    # NOTE: X1 on schematic is 8MHz — adjusting F_CPU accordingly
    # lfuse=0xFF = full-swing crystal osc, no divide
    # hfuse=0xD9 = no bootloader, SPI prog enabled, BOD 2.7V
    # efuse=0xFD = BOD enabled
    info "Setting fuses (8MHz external crystal)..."
    avrdude \
        -p atmega328pb \
        -c arduino \
        -P "${PORT}" \
        -b 115200 \
        -U lfuse:w:0xFF:m \
        -U hfuse:w:0xD9:m \
        -U efuse:w:0xFD:m \
        2>&1 | sed 's/^/  /'

    info "Flashing firmware..."
    avrdude \
        -p atmega328pb \
        -c arduino \
        -P "${PORT}" \
        -b 115200 \
        -U "flash:w:${IO_DIR}/IOmanager.hex:i" \
        2>&1 | sed 's/^/  /'

    success "ATmega328PB flashed successfully."
}

# =============================================================================
# STEP 6 — Flash RP2350B cores via IO bridge programmer
# =============================================================================
flash_rp2350() {
    local target="$1"   # MAIN or VIDEO
    local binfile="$2"

    info "Flashing CORE ${target} via IO bridge on ${PORT}..."

    [[ -f "${binfile}" ]] \
        || die "${binfile} not found — run build first."

    [[ -e "${PORT}" ]] \
        || die "Serial port ${PORT} not found."

    # Ensure PE2 is HIGH (bridge mode) — user instruction
    echo ""
    warn "ACTION REQUIRED:"
    warn "  Make sure PE2 (TSENSE) on the ATmega is pulled HIGH"
    warn "  before continuing. This enables SPI bridge mode."
    warn "  Press ENTER when ready, or Ctrl+C to abort."
    read -r

    python3 "${IO_DIR}/programmer.py" \
        --port  "${PORT}" \
        --target "${target}" \
        --write  "${binfile}" \
        2>&1 | sed 's/^/  /'

    local exit_code=$?
    if [[ ${exit_code} -eq 0 ]]; then
        success "CORE ${target} flashed and verified."
    else
        die "CORE ${target} flash failed (exit ${exit_code})."
    fi
}

# =============================================================================
# STEP 7 — Post-flash: return PE2 low reminder
# =============================================================================
post_flash_reminder() {
    echo ""
    warn "Remember to pull PE2 (TSENSE) LOW again after programming"
    warn "so the ATmega returns to normal UART passthrough mode."
    echo ""
}

# =============================================================================
# Main flow
# =============================================================================

# Step 1 — deps
if ! $SKIP_DEPS; then
    install_deps
    clone_sdk
else
    info "Skipping dependency installation (--skip-deps)."
    # Still export paths if sdks exist in default locations
    [[ -d "${SCRIPT_DIR}/pico-sdk" ]] && export PICO_SDK_PATH="${SCRIPT_DIR}/pico-sdk"
    [[ -d "${SCRIPT_DIR}/PicoDVI" ]]  && export PICODVI_PATH="${SCRIPT_DIR}/PicoDVI"
fi

echo ""
echo -e "${BOLD}-- Build phase ------------------------------------------------${NC}"

# Step 2 — build IO manager
build_io

# Step 3 — build RP2350B firmware
build_rp2350

echo ""
success "All targets built successfully."

if $BUILD_ONLY; then
    echo ""
    info "Build-only mode — skipping flash steps."
    info "Output files:"
    info "  ${IO_DIR}/IOmanager.hex"
    info "  ${BUILD_DIR}/core_main.bin"
    info "  ${BUILD_DIR}/core_video.bin"
    exit 0
fi

echo ""
echo -e "${BOLD}-- Flash phase ------------------------------------------------${NC}"

# Step 4 — flash ATmega
if $DO_FLASH_IO; then
    flash_io
fi

# Step 5 — flash CORE MAIN
if $DO_FLASH_MAIN; then
    flash_rp2350 "MAIN" "${BUILD_DIR}/core_main.bin"
fi

# Step 6 — flash CORE VIDEO
if $DO_FLASH_VIDEO; then
    flash_rp2350 "VIDEO" "${BUILD_DIR}/core_video.bin"
fi

if $DO_FLASH_IO || $DO_FLASH_MAIN || $DO_FLASH_VIDEO; then
    post_flash_reminder
fi

echo ""
echo -e "${BOLD}================================================${NC}"
echo -e "${GREEN}${BOLD}  All done — board programmed successfully!     ${NC}"
echo -e "${BOLD}================================================${NC}"
echo ""
