#!/usr/bin/env bash
# Build the pinned beta11 integration with its reviewed firmware runtime.
set -euo pipefail
SOURCE_DIR="${1:-}"
OUTPUT_ELF="${2:-}"
PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-}"
HYPERHEN_CRT="${HYPERHEN_CRT:-}"
HYPERHEN_LITE="${HYPERHEN_LITE:-0}"
HYPERHEN_JOBS="${HYPERHEN_JOBS:-4}"
BUILD_TIME="${BUILD_TIME:-2026-10-31T00:00:00Z}"
die() { printf 'build_automount: %s\n' "$*" >&2; exit 1; }
[[ -d "$SOURCE_DIR" && -n "$OUTPUT_ELF" ]] || die 'source directory and output ELF required'
[[ -d "$PS5_PAYLOAD_SDK" ]] || die 'PS5_PAYLOAD_SDK required'
[[ -f "$HYPERHEN_CRT" ]] || die 'HYPERHEN_CRT must point to the isolated R42 crt1.o'
[[ "$(sha256sum "$HYPERHEN_CRT" | cut -d ' ' -f 1)" == 817750baf8ff32deb2b480f5c4640978372fe06a952697befa21bc7788edd09b ]] || die 'unexpected CRT checksum'
[[ "$HYPERHEN_LITE" == 0 || "$HYPERHEN_LITE" == 1 ]] || die 'HYPERHEN_LITE must be0 or1'
[[ "$HYPERHEN_JOBS" =~ ^[1-9][0-9]*$ ]] || die 'positive HYPERHEN_JOBS required'
for file in Makefile LICENSE src/main.c src/sm_hyperhen_launch.c include/sm_hyperhen_policy.h tools/build_ps5_json_c.py patches/json-c-0.19-release-free.patch; do
  [[ -f "$SOURCE_DIR/$file" ]] || die "missing source input: $file"
done
export PS5_PAYLOAD_SDK HYPERHEN_CRT
export PATH="$PS5_PAYLOAD_SDK/bin:$PATH"
make -C "$SOURCE_DIR" clean HYPERHEN_CRT="$HYPERHEN_CRT"
make -C "$SOURCE_DIR" -j"$HYPERHEN_JOBS" shadowmountplus.elf \
  HYPERHEN_CRT="$HYPERHEN_CRT" HYPERHEN_LITE="$HYPERHEN_LITE" \
  VERSION_TAG=1.7beta11-hyperhen BUILD_TIME="$BUILD_TIME"
mkdir -p "$(dirname "$OUTPUT_ELF")"
cp "$SOURCE_DIR/shadowmountplus.elf" "$OUTPUT_ELF"
printf '%s\n' 'HyperHEN Automount1.7beta11 integration' \
  'Upstream commit30c5a2b791ada7a78ccbda92f4bc3b1fa1cf27dd' \
  "HYPERHEN_LITE=$HYPERHEN_LITE" \
  'CRT SHA256817750baf8ff32deb2b480f5c4640978372fe06a952697befa21bc7788edd09b' \
  > "$OUTPUT_ELF.provenance"
