#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Original 13.xx rollback scope; 12.70 is handled by its separate adapter. */
static inline bool hypercore_13xx_uses_public_v092(uint32_t firmware_version) {
  switch (firmware_version & 0xffff0000U) {
  case 0x13000000U:
  case 0x13200000U:
  case 0x13600000U:
    return true;
  default:
    return false;
  }
}
