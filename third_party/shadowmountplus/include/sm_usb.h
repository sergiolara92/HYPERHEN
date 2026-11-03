#ifndef SM_USB_H
#define SM_USB_H

#include <stdbool.h>
#include <stdint.h>

struct statfs;

// AutoMounter owns USB mount slots and extended USB storage (ext0). ext1 is M.2.
bool sm_usb_mount_path(const char *path);
uint64_t sm_usb_filesystem_id(const struct statfs *mount);
bool sm_usb_mount_matches(const struct statfs *mount, const char *path,
                           const char *source, uint64_t mount_id);
// Check one live volume without allocating a mount-table snapshot. ENOENT
// includes a surviving mount directory that now resolves to its parent FS.
int sm_usb_check_mount(const char *path, const char *source, uint64_t mount_id);
// Ask ShellCore to unmount the device owning this exact mount point once.
// Returns a positive errno; native_status preserves a firmware/transport error.
int sm_usb_request_eject(const char *path, const char *source, uint64_t mount_id,
                          int32_t *native_status);

#endif
