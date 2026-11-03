#pragma once

#include "hypercore_public_v092_route.h"

typedef enum {
  HYPERCORE_IMAGE_KEEP_EXISTING,
  HYPERCORE_IMAGE_PUBLIC_V092_13XX,
  HYPERCORE_IMAGE_DIAGNOSTIC_1270
} HyperCoreImageRoute;

/* R47 isolates diagnostic code in the existing 12.70 component slot. The
 * public 13.xx image and every other firmware retain their current bytes. */
static inline HyperCoreImageRoute hypercore_image_route(
    uint32_t firmware_version) {
  if (hypercore_1270_uses_public_v092(firmware_version))
    return HYPERCORE_IMAGE_DIAGNOSTIC_1270;
  if (hypercore_13xx_uses_public_v092(firmware_version))
    return HYPERCORE_IMAGE_PUBLIC_V092_13XX;
  return HYPERCORE_IMAGE_KEEP_EXISTING;
}
