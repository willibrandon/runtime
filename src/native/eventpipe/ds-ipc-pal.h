#ifndef __DIAGNOSTICS_IPC_PAL_H__
#define __DIAGNOSTICS_IPC_PAL_H__

#include "ds-rt-config.h"

#ifdef ENABLE_PERFTRACING
#include "ds-ipc-pal-types.h"
#include "ep-ipc-stream.h"

#undef DS_IMPL_GETTER_SETTER
#ifdef DS_IMPL_IPC_PAL_GETTER_SETTER
#define DS_IMPL_GETTER_SETTER
#endif
#include "ds-getter-setter.h"

/*
 * DiagnosticsIpc.
 */

bool
ds_ipc_pal_init (void);

bool
ds_ipc_pal_shutdown (void);

int32_t
ds_ipc_get_handle_int32_t (DiagnosticsIpc *ipc);

DiagnosticsIpc *
ds_ipc_alloc (
	const ep_char8_t *ipc_name,
	DiagnosticsIpcConnectionMode mode,
	ds_ipc_error_callback_func callback);

void
ds_ipc_free (DiagnosticsIpc *ipc);

void
ds_ipc_reset (DiagnosticsIpc *ipc);

// Poll
// Parameters:
// - IpcPollHandle * poll_handles_data: Array of IpcPollHandles to poll
// - uint32_t timeout_ms: The timeout in milliseconds for the poll ((uint32_t)-1 == infinite)
// Returns:
// int32_t: -1 on error, 0 on timeout, >0 on successful poll
// Remarks:
// Check the events returned in revents for each IpcPollHandle to find the signaled handle.
// Signaled DiagnosticsIpcs can call accept() without blocking.
// Signaled IpcStreams can call read(...) without blocking.
// The caller is responsible for cleaning up "hung up" connections.
int32_t
ds_ipc_poll (
	DiagnosticsIpcPollHandle *poll_handles_data,
	size_t poll_handles_data_len,
	uint32_t timeout_ms,
	ds_ipc_error_callback_func callback);

#ifdef DS_NATIVEAOT_FORK_LISTENER
// A caller-owned wake descriptor interrupts only this poll, preserving all sockets.
int32_t
ds_ipc_poll_interruptible (
	DiagnosticsIpcPollHandle *poll_handles_data,
	size_t poll_handles_data_len,
	uint32_t timeout_ms,
	ds_ipc_error_callback_func callback,
	int interrupt_fd);

// Returns bytes received, zero on EOF, -1 on error, or -2 on interruption.
int32_t
ds_ipc_stream_read_interruptible (
	DiagnosticsIpcStream *ipc_stream,
	uint8_t *buffer,
	uint32_t capacity,
	int interrupt_fd);

// Returns 1 with one received descriptor, 0 on error/EOF, or -2 on interruption.
int32_t
ds_ipc_stream_read_fd_interruptible (
	DiagnosticsIpcStream *ipc_stream,
	int *data_fd,
	int interrupt_fd);

// The listener owns the stream during response capture and drain. A handler may
// transfer the stream only after the complete response has been delivered.
void ds_ipc_stream_begin_response (DiagnosticsIpcStream *stream);
// Returns 1 when sent, 0 on failure, or -2 on pause; unsent bytes remain owned.
int32_t ds_ipc_stream_resume_response (DiagnosticsIpcStream *stream, int interrupt_fd);
uint64_t ds_ipc_stream_response_remaining (DiagnosticsIpcStream *stream);
// Discards queued bytes and optionally releases the stream. A tracing session
// retains its stream after the listener has delivered the start response.
void ds_ipc_stream_end_response (DiagnosticsIpcStream *stream, bool release_stream);
#endif

// puts the DiagnosticsIpc into Listening Mode
// Re-entrant safe
bool
ds_ipc_listen (
	DiagnosticsIpc *ipc,
	ds_ipc_error_callback_func callback);

// produces a connected stream from a server-mode DiagnosticsIpc.
// Blocks until a connection is available.
DiagnosticsIpcStream *
ds_ipc_accept (
	DiagnosticsIpc *ipc,
	ds_ipc_error_callback_func callback);

// Connect to a server and returns a connected stream
DiagnosticsIpcStream *
ds_ipc_connect (
	DiagnosticsIpc *ipc,
	uint32_t timeout_ms,
	ds_ipc_error_callback_func callback,
	bool *timed_out);

// Closes an open IPC.
// Only attempts minimal cleanup if is_shutdown==true, i.e.,
// unlinks Unix Domain Socket on Linux, no-op on Windows
void
ds_ipc_close (
	DiagnosticsIpc *ipc,
	bool is_shutdown,
	ds_ipc_error_callback_func callback);

int32_t
ds_ipc_to_string (
	DiagnosticsIpc *ipc,
	ep_char8_t *buffer,
	uint32_t buffer_len);
/*
 * DiagnosticsIpcStream.
 */

int32_t
ds_ipc_stream_get_handle_int32_t (DiagnosticsIpcStream *ipc_stream);

IpcStream *
ds_ipc_stream_get_stream_ref (DiagnosticsIpcStream *ipc_stream);

void
ds_ipc_stream_free (DiagnosticsIpcStream *ipc_stream);

bool
ds_ipc_stream_read (
	DiagnosticsIpcStream *ipc_stream,
	uint8_t *buffer,
	uint32_t bytes_to_read,
	uint32_t *bytes_read,
	uint32_t timeout_ms);

bool
ds_ipc_stream_read_fd (
	DiagnosticsIpcStream *ipc_stream,
	int *data_fd);

bool
ds_ipc_stream_write (
	DiagnosticsIpcStream *ipc_stream,
	const uint8_t *buffer,
	uint32_t bytes_to_write,
	uint32_t *bytes_written,
	uint32_t timeout_ms);

bool
ds_ipc_stream_flush (DiagnosticsIpcStream *ipc_stream);

bool
ds_ipc_stream_close (
	DiagnosticsIpcStream *ipc_stream,
	ds_ipc_error_callback_func callback);

int32_t
ds_ipc_stream_to_string (
	DiagnosticsIpcStream *ipc_stream,
	ep_char8_t *buffer,
	uint32_t buffer_len);

IpcPollEvents
ds_ipc_stream_poll (
	DiagnosticsIpcStream *ipc_stream,
	uint32_t timeout_ms);

#endif /* ENABLE_PERFTRACING */
#endif /* __DIAGNOSTICS_IPC_PAL_H__ */
