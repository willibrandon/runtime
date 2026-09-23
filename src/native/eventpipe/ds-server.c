#include "ds-rt-config.h"

#ifdef ENABLE_PERFTRACING
#if !defined(DS_INCLUDE_SOURCE_FILES) || defined(DS_FORCE_INCLUDE_SOURCE_FILES)

#define DS_IMPL_SERVER_GETTER_SETTER
#include "ds-server.h"
#include "ds-ipc.h"
#include "ds-protocol.h"
#include "ds-process-protocol.h"
#include "ds-eventpipe-protocol.h"
#include "ds-dump-protocol.h"
#include "ds-profiler-protocol.h"
#include "ds-rt.h"

#ifdef DS_NATIVEAOT_FORK_LISTENER
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

extern bool ep_rt_aot_join_server_thread (void);
static int _server_interrupt [2] = {-1, -1};
static volatile uint32_t _server_pausing;
static bool _server_paused;
static DiagnosticsIpcStream *_server_pending_stream;
static DiagnosticsIpcStream *_server_response_stream;
static EventPipeCollectTracingCommandPayload *_server_eventpipe_payload;
static EventPipeSessionID _server_response_session_id;
static DiagnosticsIpcMessage _server_pending_message;
static uint32_t _server_received;

static void server_begin_response (DiagnosticsIpcStream *stream)
{
	EP_ASSERT (_server_response_stream == NULL);
	ds_ipc_stream_begin_response (stream);
	_server_response_stream = stream;
}

static bool server_create_interrupt (void)
{
	if (pipe (_server_interrupt) != 0)
		return false;

	for (uint32_t i = 0; i < 2; i++) {
		int flags = fcntl (_server_interrupt [i], F_GETFL, 0);
		if (flags < 0 || fcntl (_server_interrupt [i], F_SETFL, flags | O_NONBLOCK) != 0 ||
			fcntl (_server_interrupt [i], F_SETFD, FD_CLOEXEC) != 0) {
			close (_server_interrupt [0]);
			close (_server_interrupt [1]);
			_server_interrupt [0] = _server_interrupt [1] = -1;
			return false;
		}
	}

	ds_ipc_stream_factory_set_interrupt (_server_interrupt [0]);
	return true;
}
#endif

/*
 * Globals and volatile access functions.
 */

static volatile uint32_t _server_shutting_down_state = 0;
static ep_rt_wait_event_handle_t _server_resume_runtime_startup_event = { 0 };
static bool _server_disabled = false;
static volatile bool _is_paused_for_startup = false;

static
inline
bool
server_volatile_load_shutting_down_state (void)
{
	return (ep_rt_volatile_load_uint32_t (&_server_shutting_down_state) != 0) ? true : false;
}

static
inline
void
server_volatile_store_shutting_down_state (bool state)
{
	ep_rt_volatile_store_uint32_t (&_server_shutting_down_state, state ? 1 : 0);
}

/*
 * Forward declares of all static functions.
 */

static
void
server_error_callback_create (
	const ep_char8_t *message,
	uint32_t code);

static
void
server_error_callback_close (
	const ep_char8_t *message,
	uint32_t code);

static
void
server_warning_callback (
	const ep_char8_t *message,
	uint32_t code);

static
bool
server_protocol_helper_unknown_command (
	DiagnosticsIpcMessage *message,
	DiagnosticsIpcStream *stream);

/*
 * DiagnosticServer.
 */

static
void
server_error_callback_create (
	const ep_char8_t *message,
	uint32_t code)
{
	EP_ASSERT (message != NULL);
	DS_LOG_ERROR_2 ("Failed to create diagnostic IPC: error (%d): %s.", code, message);
}

static
void
server_error_callback_close (
	const ep_char8_t *message,
	uint32_t code)
{
	EP_ASSERT (message != NULL);
	DS_LOG_ERROR_2 ("Failed to close diagnostic IPC: error (%d): %s.", code, message);
}

static
bool
server_protocol_helper_unknown_command (
	DiagnosticsIpcMessage *message,
	DiagnosticsIpcStream *stream)
{
	DS_LOG_WARNING_1 ("Received unknown request type (%d)", ds_ipc_header_get_commandset (ds_ipc_message_get_header_ref (message)));
	ds_ipc_message_send_error (stream, DS_IPC_E_UNKNOWN_COMMAND);
	ds_ipc_stream_free (stream);
	return true;
}

static
void
server_warning_callback (
	const ep_char8_t *message,
	uint32_t code)
{
	EP_ASSERT (message != NULL);
	DS_LOG_WARNING_2 ("warning (%d): %s.", code, message);
}

static size_t server_loop_tick (void* data) {
	if (server_volatile_load_shutting_down_state ())
		return 1; // done
#ifdef DS_NATIVEAOT_FORK_LISTENER
	if (ep_rt_volatile_load_uint32_t (&_server_pausing) != 0)
		return 1;

	if (_server_eventpipe_payload != NULL) {
		EventPipeSessionID pending_session_id = 0;
		int32_t result = ds_eventpipe_protocol_helper_resume_ipc_message (
			&_server_pending_message,
			_server_pending_stream,
			&_server_eventpipe_payload,
			_server_interrupt [0],
			&pending_session_id);
		if (result == -2)
			return 1;

		_server_pending_stream = NULL;
		_server_received = 0;
		ds_ipc_message_fini (&_server_pending_message);
		if (pending_session_id != 0)
			_server_response_session_id = pending_session_id;

		return 0;
	}

	if (_server_response_stream != NULL) {
		int32_t result = ds_ipc_stream_resume_response (_server_response_stream, _server_interrupt [0]);
		if (result == -2)
			return 1;

		EventPipeSessionID pending_session_id = _server_response_session_id;
		ds_ipc_stream_end_response (_server_response_stream, pending_session_id == 0);
		_server_response_stream = NULL;
		_server_response_session_id = 0;
		if (pending_session_id != 0) {
			if (result == 1)
				ep_start_streaming (pending_session_id);
			else
				ep_disable (pending_session_id);
		}

		return 0;
	}

	DiagnosticsIpcStream *stream = _server_pending_stream;
	DiagnosticsIpcMessage *message = &_server_pending_message;
	if (stream == NULL) {
		stream = ds_ipc_stream_factory_get_next_available_stream (server_warning_callback);
		if (stream == NULL)
			return ep_rt_volatile_load_uint32_t (&_server_pausing) != 0 ? 1 : 0;

		ds_rt_auto_trace_signal ();
		ds_ipc_message_init (message);
		_server_pending_stream = stream;
		_server_received = 0;
	}

	int32_t read_result = ds_ipc_message_resume_stream (message, stream, &_server_received, _server_interrupt [0]);
	if (read_result == -2 || ep_rt_volatile_load_uint32_t (&_server_pausing) != 0)
		return 1;

	if (read_result == 0) {
		_server_pending_stream = NULL;
		_server_received = 0;
		server_begin_response (stream);
		ds_ipc_message_send_error (stream, DS_IPC_E_BAD_ENCODING);
		ds_ipc_stream_free (stream);
		ds_ipc_message_fini (message);
		return 0;
	}
#else
	DiagnosticsIpcStream *stream = ds_ipc_stream_factory_get_next_available_stream (server_warning_callback);
	if (!stream)
		return 0;

	ds_rt_auto_trace_signal ();
	DiagnosticsIpcMessage storage;
	DiagnosticsIpcMessage *message = &storage;
	if (!ds_ipc_message_init (message)) {
		ds_ipc_stream_free (stream);
		return 0;
	}

	if (!ds_ipc_message_initialize_stream (message, stream)) {
		ds_ipc_message_send_error (stream, DS_IPC_E_BAD_ENCODING);
		ds_ipc_stream_free (stream);
		ds_ipc_message_fini (message);
		return 0;
	}
#endif

	bool valid_magic = ep_rt_utf8_string_compare (
		(const ep_char8_t *)ds_ipc_header_get_magic_ref (ds_ipc_message_get_header_ref (message)),
		(const ep_char8_t *)DOTNET_IPC_V1_MAGIC) == 0;
#ifdef DS_NATIVEAOT_FORK_LISTENER
	bool is_eventpipe = valid_magic &&
		ds_ipc_header_get_commandset (ds_ipc_message_get_header_ref (message)) == DS_SERVER_COMMANDSET_EVENTPIPE;
	if (!is_eventpipe) {
		_server_pending_stream = NULL;
		_server_received = 0;
		server_begin_response (stream);
	}
#endif
	if (!valid_magic) {

		ds_ipc_message_send_error (stream, DS_IPC_E_UNKNOWN_MAGIC);
		ds_ipc_stream_free (stream);
		ds_ipc_message_fini (message);
		return 0; // continue
	}

	DS_LOG_INFO_2 ("DiagnosticServer - received IPC message with command set (%d) and command id (%d)", ds_ipc_header_get_commandset (ds_ipc_message_get_header_ref (message)), ds_ipc_header_get_commandid (ds_ipc_message_get_header_ref (message)));

	switch ((DiagnosticsServerCommandSet)ds_ipc_header_get_commandset (ds_ipc_message_get_header_ref (message))) {
	case DS_SERVER_COMMANDSET_DUMP:
		ds_dump_protocol_helper_handle_ipc_message (message, stream);
		break;
	case DS_SERVER_COMMANDSET_EVENTPIPE: {
#ifdef DS_NATIVEAOT_FORK_LISTENER
		server_begin_response (stream);
		EventPipeSessionID pending_session_id = 0;
		int32_t eventpipe_result = ds_eventpipe_protocol_helper_resume_ipc_message (
			message,
			stream,
			&_server_eventpipe_payload,
			_server_interrupt [0],
			&pending_session_id);
		if (eventpipe_result == -2)
			return 1;

		_server_pending_stream = NULL;
		_server_received = 0;
		if (pending_session_id != 0)
			_server_response_session_id = pending_session_id;
#else
		ds_eventpipe_protocol_helper_handle_ipc_message (message, stream);
#endif
		break;
	}
	case DS_SERVER_COMMANDSET_PROFILER:
		ds_profiler_protocol_helper_handle_ipc_message (message, stream);
		break;
	case DS_SERVER_COMMANDSET_PROCESS:
		ds_process_protocol_helper_handle_ipc_message (message, stream);
		break;
	default:
		server_protocol_helper_unknown_command (message, stream);
		break;
	}

	ds_ipc_message_fini (message);

	(void)data; // unused
	return 0; // continue
}

#ifndef PERFTRACING_DISABLE_THREADS
EP_RT_DEFINE_THREAD_FUNC (server_thread)
{
	EP_ASSERT (server_volatile_load_shutting_down_state () || ds_ipc_stream_factory_has_active_ports ());

	ep_rt_set_server_name();

	if (!ds_ipc_stream_factory_has_active_ports ()) {
#ifndef DS_IPC_DISABLE_LISTEN_PORTS
		DS_LOG_ERROR_0 ("Diagnostics IPC listener was undefined");
#endif
		return 1;
	}

	while (server_loop_tick (NULL) == 0) { }
	return (ep_rt_thread_start_func_return_t)0;
}
#endif // PERFTRACING_DISABLE_THREADS

#ifdef DS_NATIVEAOT_FORK_LISTENER
uint32_t ds_server_paused_input_bytes (void)
{
	return _server_paused ? _server_received : UINT32_MAX;
}

uint64_t ds_server_paused_output_bytes (void)
{
	if (!_server_paused)
		return UINT64_MAX;

	return _server_response_stream != NULL ? ds_ipc_stream_response_remaining (_server_response_stream) : 0;
}

bool ds_server_pause_listener (void)
{
	if (_server_interrupt [0] < 0 || _server_paused)
		return true;

	ep_rt_volatile_store_uint32_t (&_server_pausing, 1);
	uint8_t wake = 1;
	ssize_t written;
	do {
		written = write (_server_interrupt [1], &wake, 1);
	} while (written < 0 && errno == EINTR);

	if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
		return false;

	if (!ep_rt_aot_join_server_thread ())
		return false;

	_server_paused = true;
	return true;
}

bool ds_server_resume_listener (void)
{
	if (!_server_paused)
		return true;

	uint8_t buffer [32];
	for (;;) {
		ssize_t count = read (_server_interrupt [0], buffer, sizeof (buffer));
		if (count > 0 || (count < 0 && errno == EINTR))
			continue;

		if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
			return false;

		break;
	}

	ep_rt_volatile_store_uint32_t (&_server_pausing, 0);
	ep_rt_thread_id_t thread_id = ep_rt_uint64_t_to_thread_id_t (0);
	if (!ep_rt_thread_create ((void *)server_thread, NULL, EP_THREAD_TYPE_SERVER, (void *)&thread_id)) {
		ep_rt_volatile_store_uint32_t (&_server_pausing, 1);
		return false;
	}

	_server_paused = false;
	return true;
}
#endif

void
ds_server_disable (void)
{
	_server_disabled = true;
}

bool
ds_server_init (void)
{
	if (!ds_ipc_stream_factory_init ())
		return false;

	if (_server_disabled || !ds_rt_config_value_get_enable ())
		return true;

	bool result = false;

	// Initialize PAL layer.
	if (!ds_ipc_pal_init ()) {
		DS_LOG_ERROR_1 ("Failed to initialize PAL layer (%d).", ep_rt_get_last_error ());
		ep_raise_error ();
	}

	// Initialize the RuntimeIdentifier before use
	ds_ipc_advertise_cookie_v1_init ();

	// Ports can fail to be configured
	if (!ds_ipc_stream_factory_configure (server_error_callback_create))
		DS_LOG_ERROR_0 ("At least one Diagnostic Port failed to be configured.");

	if (ds_ipc_stream_factory_any_suspended_ports ()) {
		ep_rt_wait_event_alloc (&_server_resume_runtime_startup_event, true, false);
		ep_raise_error_if_nok (ep_rt_wait_event_is_valid (&_server_resume_runtime_startup_event));
	}

	if (ds_ipc_stream_factory_has_active_ports ()) {
#ifdef DS_NATIVEAOT_FORK_LISTENER
		ep_raise_error_if_nok (server_create_interrupt ());
#endif
		ds_rt_auto_trace_init ();
		ds_rt_auto_trace_launch ();

#ifndef PERFTRACING_DISABLE_THREADS
		ep_rt_thread_id_t thread_id = ep_rt_uint64_t_to_thread_id_t (0);

		if (!ep_rt_thread_create ((void *)server_thread, NULL, EP_THREAD_TYPE_SERVER, (void *)&thread_id)) {
			// Failed to create IPC thread.
			ds_ipc_stream_factory_close_ports (NULL);
			DS_LOG_ERROR_1 ("Failed to create diagnostic server thread (%d).", ep_rt_get_last_error ());
			ep_raise_error ();
		} else {
			ds_rt_auto_trace_wait ();
		}
#else
		ep_rt_queue_job ((void *)server_loop_tick, NULL);
#endif
	}

	result = true;

ep_on_exit:
	return result;

ep_on_error:
	EP_ASSERT (!result);
#ifdef DS_NATIVEAOT_FORK_LISTENER
	if (_server_interrupt [0] >= 0) {
		ds_ipc_stream_factory_set_interrupt (-1);
		close (_server_interrupt [0]);
		close (_server_interrupt [1]);
		_server_interrupt [0] = _server_interrupt [1] = -1;
	}
#endif
	ep_exit_error_handler ();
}

bool
ds_server_shutdown (void)
{
	server_volatile_store_shutting_down_state (true);

	if (ds_ipc_stream_factory_has_active_ports ())
		ds_ipc_stream_factory_shutdown (server_error_callback_close);

	ds_ipc_stream_factory_fini ();
	ds_ipc_pal_shutdown ();
	return true;
}

// This method will block runtime bring-up IFF DOTNET_DefaultDiagnosticPortSuspend != NULL and DOTNET_DiagnosticPorts != 0 (it's default state)
// The _ds_resume_runtime_startup_event event will be signaled when the Diagnostics Monitor uses the ResumeRuntime Diagnostics IPC Command
void
ds_server_pause_for_diagnostics_monitor (void)
{
// pause is not implemented for single-threaded
#ifndef PERFTRACING_DISABLE_THREADS
	_is_paused_for_startup = true;

	if (ds_ipc_stream_factory_any_suspended_ports ()) {
		EP_ASSERT (ep_rt_wait_event_is_valid (&_server_resume_runtime_startup_event));
		DS_LOG_ALWAYS_0 ("The runtime has been configured to pause during startup and is awaiting a Diagnostics IPC ResumeStartup command.");

		if (ep_rt_wait_event_wait (&_server_resume_runtime_startup_event, 5000, false) != 0) {
			ds_rt_server_log_pause_message ();
			DS_LOG_ALWAYS_0 ("The runtime has been configured to pause during startup and is awaiting a Diagnostics IPC ResumeStartup command and has waited 5 seconds.");
			ep_rt_wait_event_wait (&_server_resume_runtime_startup_event, EP_INFINITE_WAIT, false);
		}
	}

	// allow wait failures to fall through and the runtime to continue coming up
#endif
}

void
ds_server_resume_runtime_startup (void)
{
	ds_ipc_stream_factory_resume_current_port ();
	if (!ds_ipc_stream_factory_any_suspended_ports () && ep_rt_wait_event_is_valid (&_server_resume_runtime_startup_event)) {
		ep_rt_wait_event_set (&_server_resume_runtime_startup_event);
		_is_paused_for_startup = false;
	}
}

bool
ds_server_is_paused_in_startup (void)
{
	return _is_paused_for_startup;
}

#endif /* !defined(DS_INCLUDE_SOURCE_FILES) || defined(DS_FORCE_INCLUDE_SOURCE_FILES) */
#endif /* ENABLE_PERFTRACING */

#if !defined(ENABLE_PERFTRACING) || (defined(DS_INCLUDE_SOURCE_FILES) && !defined(DS_FORCE_INCLUDE_SOURCE_FILES))
extern const char quiet_linker_empty_file_warning_diagnostics_server;
const char quiet_linker_empty_file_warning_diagnostics_server = 0;
#endif
