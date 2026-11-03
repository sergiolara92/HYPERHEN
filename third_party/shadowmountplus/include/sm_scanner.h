#ifndef SM_SCANNER_H
#define SM_SCANNER_H

#include <stdbool.h>

// Initialize scanner service resources such as wake pipe and file watches.
bool sm_scanner_init(void);
// Wake a blocked scanner wait. Safe to call from signal context.
void sm_scanner_wake(void);
// Return true after scanner-owned USB directory descriptors are closed.
bool sm_scanner_usb_watches_suspended(void);
// Exclude an external mutation from full/targeted scan and install work.
// Returns false immediately when scanner-owned work is already active.
bool sm_scanner_try_begin_external_mutation(void);
void sm_scanner_end_external_mutation(void);
// Caller holds the external mutation gate. Scanner closes its USB directory
// fds itself and acknowledges before ejection; resume also applies on failure.
bool sm_scanner_suspend_usb_for_eject(void);
void sm_scanner_resume_usb_after_eject(void);
// Run the initial full synchronization cycle.
bool sm_scanner_run_startup_sync(void);
// Run the steady-state scanner wait/scan loop until shutdown is requested.
void sm_scanner_run_loop(void);
// Release scanner service resources.
void sm_scanner_shutdown(void);

#endif
