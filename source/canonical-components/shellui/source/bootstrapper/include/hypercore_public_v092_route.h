#pragma once

#include "hypercore_13xx_rollback.h"
#include <onion/ready.h>

static inline bool hypercore_1270_uses_public_v092(uint32_t firmware_version) {
  return (firmware_version & 0xffff0000U) == 0x12700000U;
}

static inline bool hypercore_uses_public_v092(uint32_t firmware_version) {
  return hypercore_13xx_uses_public_v092(firmware_version) ||
         hypercore_1270_uses_public_v092(firmware_version);
}

/* The frozen v0.9.2 core classifies 12.70 as NO_PPR. Translate that loader
 * contract only after the existing Bypervisor stage has verified completion.
 * Keep this marker for the core's later resume checks in the same boot. */
static inline bool hypercore_1270_prepare_public_v092(
    uint32_t firmware_version, bool (*is_ready)(const char *),
    bool (*publish_ready)(const char *)) {
  if (!hypercore_1270_uses_public_v092(firmware_version))
    return true;
  return is_ready && publish_ready &&
         is_ready(ONION_READY_HYPERVISOR) &&
         publish_ready(ONION_READY_HYPERCORE_NO_PPR);
}
