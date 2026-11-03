#include "sm_platform.h"

#include <stdatomic.h>

#include "sm_log.h"
#include "sm_usb.h"

#define USB_DEVICE_LIMIT 8u
#define USB_MOUNT_POINT_SIZE 128u
#define USB_IPMI_MIN_STORAGE_SIZE 0x1000u

typedef struct {
  const void *data;
  size_t size;
} usb_ipmi_input_t;

// Client BufferInfo has the same 24-byte stride as the server output array.
// The third field is written by IPMI, including for a single output argument.
typedef struct {
  void *data;
  size_t capacity;
  size_t written;
} usb_ipmi_output_t;

typedef struct {
  void *storage;
  void *client;
} usb_ipmi_client_t;

// Import the firmware's C++ ABI through the SDK's libSceIpmi link stub.
extern void usb_ipmi_config_ctor(void *)
    __asm__("_ZN4IPMI6Client6ConfigC1Ev");
extern uint64_t usb_ipmi_storage_size(void *)
    __asm__("_ZN4IPMI6Client6Config24estimateClientMemorySizeEv");
extern int usb_ipmi_create(void **, const void *, void *, void *)
    __asm__("_ZN4IPMI6Client6createEPPS0_PKNS0_6ConfigEPvS6_");
extern int usb_ipmi_connect(void *, const void *, size_t, int32_t *)
    __asm__("_ZN4IPMI4impl10ClientImpl7connectEPKvmPi");
extern int usb_ipmi_invoke(void *, uint32_t, const usb_ipmi_input_t *, uint32_t,
                            int32_t *, usb_ipmi_output_t *, uint32_t)
    __asm__("_ZN4IPMI4impl10ClientImpl16invokeSyncMethodEjPKNS_8DataInfoEjPiPNS_10BufferInfoEj");
extern void usb_ipmi_terminate(void *)
    __asm__("_ZN4IPMI4impl10ClientImpl19terminateConnectionEv");
extern void usb_ipmi_destroy(void *)
    __asm__("_ZN4IPMI4impl10ClientImpl7destroyEv");

// ClientImpl::destroy is void and can leave a live kid on failure. Its kid
// field (+8) and success sentinel (-1) were verified in libSceIpmi 9.40.
typedef struct {
  void *vtable;
  uint32_t kid;
} usb_ipmi_client_header_t;

// A failed destroy retains its storage until process exit. Fence
// further requests so that this exceptional retention cannot grow unbounded.
static atomic_bool g_usb_cleanup_failed = ATOMIC_VAR_INIT(false);

_Static_assert(sizeof(usb_ipmi_input_t) == 16, "IPMI DataInfo ABI");
_Static_assert(sizeof(usb_ipmi_output_t) == 24, "IPMI BufferInfo ABI");

bool sm_usb_mount_path(const char *path) {
  return path && (!strcmp(path, "/mnt/ext0") ||
      (strlen(path) == 9 && !strncmp(path, "/mnt/usb", 8) &&
       path[8] >= '0' && path[8] <= '7'));
}

uint64_t sm_usb_filesystem_id(const struct statfs *mount) {
  uint64_t id;
  _Static_assert(sizeof(mount->f_fsid) == sizeof(id), "filesystem identity ABI");
  memcpy(&id, &mount->f_fsid, sizeof(id));
  return id;
}

bool sm_usb_mount_matches(const struct statfs *mount, const char *path,
                           const char *source, uint64_t mount_id) {
  return !strcmp(mount->f_mntonname, path) &&
         !strcmp(mount->f_mntfromname, source) &&
         strcmp(mount->f_fstypename, "nullfs") &&
         sm_usb_filesystem_id(mount) == mount_id;
}

int sm_usb_check_mount(const char *path, const char *source, uint64_t mount_id) {
  if (!sm_usb_mount_path(path) || !source || !source[0])
    return EINVAL;
  struct statfs current;
  if (statfs(path, &current) != 0)
    return errno ? errno : EIO;
  // The mount directory may remain on its parent tmpfs after native unmount.
  if (strcmp(current.f_mntonname, path))
    return ENOENT;
  return sm_usb_mount_matches(&current, path, source, mount_id) ? 0 : ESTALE;
}

static int usb_native_error(int32_t status) {
  if (((uint32_t)status & 0xffff0000u) == 0x80020000u &&
      ((uint32_t)status & 0xffffu))
    return (uint32_t)status & 0xffffu;
  return status ? EIO : 0;
}

static void close_usb_ipmi(usb_ipmi_client_t *ipc) {
  if (ipc->client) {
    // End the connection locally before destroy; destroy on a connected
    // client returns EBUSY. No peer round trip or callback thread is needed.
    usb_ipmi_terminate(ipc->client);
    usb_ipmi_destroy(ipc->client);
    uint32_t kid;
    memcpy(&kid, (const char *)ipc->client +
                     offsetof(usb_ipmi_client_header_t, kid), sizeof(kid));
    if (kid != UINT32_MAX) {
      atomic_store_explicit(&g_usb_cleanup_failed, true, memory_order_release);
      log_debug("  [USB] IPMI client destroy failed: kid=0x%08x; "
                "retaining storage and disabling eject until restart", kid);
      return;
    }
  }
  free(ipc->storage);
}

static int open_usb_ipmi(usb_ipmi_client_t *ipc, int32_t *native_status) {
  memset(ipc, 0, sizeof(*ipc));
  if (atomic_load_explicit(&g_usb_cleanup_failed, memory_order_acquire))
    return EIO;
  unsigned char config[0x200] __attribute__((aligned(16))) = {0};
  usb_ipmi_config_ctor(config);
  // The firmware service name is 17 characters, not a 16-byte string.
  memcpy(config, "SceAutoMounterIpc", sizeof("SceAutoMounterIpc"));
  uint32_t flag = 1;
  uint64_t request_size = 0x200, response_size = 0xf800;
  memcpy(config + 0x1c, &flag, sizeof(flag));
  memcpy(config + 0x28, &request_size, sizeof(request_size));
  memcpy(config + 0x30, &response_size, sizeof(response_size));
  uint64_t size = usb_ipmi_storage_size(config);
  if (size < USB_IPMI_MIN_STORAGE_SIZE || size > 0x100000u)
    return ENOTSUP;
  ipc->storage = calloc(1, (size_t)size);
  if (!ipc->storage)
    return ENOMEM;
  int32_t rc = usb_ipmi_create(&ipc->client, config, NULL, ipc->storage);
  if (rc || !ipc->client) {
    *native_status = rc;
    ipc->client = NULL;
    return rc ? usb_native_error(rc) : EIO;
  }
  int32_t result = 0;
  rc = usb_ipmi_connect(ipc->client, NULL, 0, &result);
  *native_status = rc ? rc : result;
  return usb_native_error(*native_status);
}

static int invoke_usb_ipmi(usb_ipmi_client_t *ipc, uint32_t method,
                            const usb_ipmi_input_t *input,
                            usb_ipmi_output_t *output, uint32_t output_count,
                            int32_t *native_status) {
  int32_t result = 0;
  int32_t rc = usb_ipmi_invoke(ipc->client, method, input, input ? 1 : 0,
                              &result, output, output_count);
  *native_status = rc ? rc : result;
  return usb_native_error(*native_status);
}

int sm_usb_request_eject(const char *path, const char *source, uint64_t mount_id,
                          int32_t *native_status) {
  if (!native_status)
    return EINVAL;
  *native_status = 0;
  if (!sm_usb_mount_path(path) || !source || !source[0])
    return EINVAL;

  usb_ipmi_client_t ipc;
  int status = open_usb_ipmi(&ipc, native_status);
  if (status)
    goto done;

  // Wire formats and method IDs verified in ShellCore's dispatch table and
  // libSceUsbStorage on 9.40: list2=0x30015, mountPoint=0x30013, eject=0x30025.
  uint32_t ids[USB_DEVICE_LIMIT] = {0};
  int32_t count = 0;
  uint64_t capacity = USB_DEVICE_LIMIT;
  usb_ipmi_input_t input = {&capacity, sizeof(capacity)};
  usb_ipmi_output_t output[] = {
      {ids, sizeof(ids), 0}, {&count, sizeof(count), 0}};
  status = invoke_usb_ipmi(&ipc, 0x30015, &input, output, 2, native_status);
  if (status)
    goto done;
  if (count < 0 || count > (int32_t)USB_DEVICE_LIMIT ||
      output[0].written != sizeof(ids) || output[1].written != sizeof(count)) {
    status = EIO;
    goto done;
  }

  status = ENOENT;
  for (int32_t i = 0; i < count; ++i) {
    char mount_point[USB_MOUNT_POINT_SIZE] = {0};
    uint32_t args[] = {ids[i], sizeof(mount_point)};
    usb_ipmi_input_t point_input = {args, sizeof(args)};
    usb_ipmi_output_t point_output = {mount_point, sizeof(mount_point), 0};
    status = invoke_usb_ipmi(&ipc, 0x30013, &point_input, &point_output, 1,
                              native_status);
    if (status)
      goto done;
    if (!memchr(mount_point, '\0', sizeof(mount_point)) ||
        point_output.written != sizeof(mount_point)) {
      status = EIO;
      goto done;
    }
    if (strcmp(path, mount_point)) {
      status = ENOENT;
      continue;
    }
    // Enumeration/connect may take time. A replaced volume must not inherit
    // an ejection request merely because its usbN/daN names were reused.
    status = sm_usb_check_mount(path, source, mount_id);
    if (status)
      goto done;
    usb_ipmi_input_t eject_input = {&ids[i], sizeof(ids[i])};
    status = invoke_usb_ipmi(&ipc, 0x30025, &eject_input, NULL, 0,
                              native_status);
    goto done; // Never repeat an ejection request, including after an error.
  }

done:
  close_usb_ipmi(&ipc);
  log_debug("  [USB] eject path=%s status=%d native=0x%08x", path,
            status, (unsigned)*native_status);
  return status;
}
