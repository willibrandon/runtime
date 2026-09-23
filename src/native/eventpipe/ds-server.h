#ifndef __DIAGNOSTICS_SERVER_H__
#define __DIAGNOSTICS_SERVER_H__

#include "ds-rt-config.h"

#ifdef ENABLE_PERFTRACING
#include "ds-types.h"
#include "ds-rt.h"

/*
 * DiagnosticsServer.
 */

void
ds_server_disable (void);

// Initialize the event pipe (Creates the EventPipe IPC server).
bool
ds_server_init (void);

// Shutdown the event pipe.
bool
ds_server_shutdown (void);

#ifdef DS_NATIVEAOT_FORK_LISTENER
// Lifecycle calls are serialized by the native fork coordinator. Pending input
// and endpoint ownership remain intact while the listener thread is retired.
// Input and ordinary response delivery can pause. An executing command body or
// tracing-session handoff must finish before pause returns.
bool ds_server_pause_listener (void);
bool ds_server_resume_listener (void);
// Only read after a successful pause, when the listener no longer writes it.
uint32_t ds_server_paused_input_bytes (void);
uint64_t ds_server_paused_output_bytes (void);
#endif

// Pauses runtime startup after the Diagnostics Server has been started
// allowing a Diagnostics Monitor to attach perform tasks before
// Startup is completed
EP_NEVER_INLINE
void
ds_server_pause_for_diagnostics_monitor (void);

// Sets event to resume startup in runtime
// This is a no-op if not configured to pause or runtime has already resumed
void
ds_server_resume_runtime_startup (void);

bool
ds_server_is_paused_in_startup (void);

#endif /* ENABLE_PERFTRACING */
#endif /* __DIAGNOSTICS_SERVER_H__ */
