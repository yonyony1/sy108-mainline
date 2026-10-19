/* SPDX-License-Identifier: GPL-2.0-only */
#include "cellular.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <syslog.h>
#include <unistd.h>

struct control {
	struct transport *transport;
	pthread_mutex_t lock;
	pthread_t thread;
	int socket;
	atomic_bool accepting;
	bool busy, started;
	J *job, *arguments, *receipts[32];
	uint64_t sequence;
	char broker[33];
};
static volatile sig_atomic_t running = 1;
static void stop(int sig)
{
	(void)sig;
	running = 0;
}
/* The latest status file is not a receipt store: a second client can replace
 * it. */
static void remember(struct control *control, J *row)
{
	unsigned index = (unsigned)json_object_get_int64(get(row, "id")) % 32;
	json_object_put(control->receipts[index]);
	control->receipts[index] = jclone(row);
}
static J *dispatch(struct control *control, J *request)
{
	const char *const fields[] = {"operation", "arguments"};
	if (!keys(request, fields, 2)) {
		errorf(CELL_INVALID, "Invalid control request");
		return NULL;
	}
	const char *op = str(request, "operation");
	J *args = get(request, "arguments");
	if (controller_validate(op, args) < 0)
		return NULL;
	if (!strcmp(op, "operation_status")) {
		uint64_t id = (uint64_t)json_object_get_int64(get(args, "id"));
		pthread_mutex_lock(&control->lock);
		J *row = control->receipts[id % 32], *reply = NULL;
		if (row && json_object_equal(get(row, "id"), get(args, "id")) &&
		    text_equal(str(row, "boot_id"), str(args, "boot_id")) &&
		    text_equal(str(row, "broker_id"), str(args, "broker_id"))) {
			reply = jclone(row);
			boolean(reply, "accepted", true);
		} else
			errorf(CELL_UNCERTAIN, "Operation receipt expired or service restarted; "
					       "request was not replayed");
		pthread_mutex_unlock(&control->lock);
		return reply;
	}
	if (!strcmp(op, "status")) {
		J *value = read_json(CELL_RUN "/cellular-status.json", 262144);
		if (!value)
			return NULL;
		boolean(value, "accepted", true);
		boolean(value, "cached", true);
		double age = mono() - json_object_get_double(get(value, "uptime"));
		real(value, "snapshot_age_seconds", age < 0 ? 0 : age);
		return value;
	}
	pthread_mutex_lock(&control->lock);
	J *reply = NULL, *row = NULL;
	if (!atomic_load(&control->accepting)) {
		errorf(CELL_SYSTEM, "Cellular service is stopping");
		goto end;
	}
	if (control->transport->poisoned && strcmp(op, "quiesce")) {
		errorf(CELL_UNCERTAIN, "AT outcome uncertain; no automatic replay");
		goto end;
	}
	if (control->busy || control->job) {
		reply = json_object_new_object();
		boolean(reply, "accepted", false);
		boolean(reply, "busy", true);
		string(reply, "error", "Another cellular operation is running");
		goto end;
	}
	row = json_object_new_object();
	integer(row, "id", ++control->sequence);
	string(row, "boot_id", control->transport->boot);
	string(row, "broker_id", control->broker);
	string(row, "operation", op);
	string(row, "state", "queued");
	real(row, "queued_uptime", mono());
	if (!strcmp(op, "sms_send")) {
		int state;
		J *old = receipt_reserve(op, args, row, &state);
		if (state < 0)
			goto end;
		if (old) {
			reply = json_object_new_object();
			boolean(reply, "accepted", true);
			boolean(reply, "duplicate", true);
			json_object_object_add(reply, "operation_id",
					       json_object_get(get(old, "id")));
			json_object_object_add(reply, "original_boot_id",
					       json_object_get(get(old, "boot_id")));
			json_object_object_add(reply, "state", json_object_get(get(old, "state")));
			json_object_object_add(reply, "result",
					       json_object_get(get(old, "result")));
			json_object_object_add(reply, "error", json_object_get(get(old, "error")));
			if (!text_equal(str(old, "broker_id"), control->broker) &&
			    (text_equal(str(old, "state"), "queued") ||
			     text_equal(str(old, "state"), "running"))) {
				string(reply, "state", "uncertain");
				string(reply, "error",
				       "Previous broker ended without a terminal receipt; request "
				       "will not be "
				       "replayed");
			}
			json_object_put(old);
			goto end;
		}
	}
	if (atomic_json(CELL_RUN "/operation.json", row, false) < 0)
		goto end;
	remember(control, row);
	control->job = json_object_get(row);
	control->arguments = jclone(args);
	reply = json_object_new_object();
	boolean(reply, "accepted", true);
	integer(reply, "operation_id", control->sequence);
	string(reply, "broker_id", control->broker);
	string(reply, "boot_id", control->transport->boot);
	string(reply, "state", "queued");
end:
	json_object_put(row);
	pthread_mutex_unlock(&control->lock);
	return reply;
}
static void *serve(void *pointer)
{
	struct control *control = pointer;
	while (atomic_load(&control->accepting)) {
		struct pollfd p = {control->socket, POLLIN, 0};
		int ready = poll(&p, 1, 200);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready <= 0)
			continue;
		int client = accept4(control->socket, NULL, NULL, SOCK_CLOEXEC);
		if (client < 0)
			continue;
		struct timeval timeout = {2, 0};
		setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
		setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
		J *reply = NULL, *request = NULL;
		struct ucred cred;
		socklen_t size = sizeof(cred);
		if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cred, &size) || cred.uid) {
			errorf(CELL_INVALID, "Root peer required");
			goto respond;
		}
		char buffer[8193];
		struct iovec io = {buffer, 8192};
		struct msghdr message = {.msg_iov = &io, .msg_iovlen = 1};
		ssize_t n = recvmsg(client, &message, 0);
		if (n <= 0 || (message.msg_flags & MSG_TRUNC)) {
			errorf(CELL_INVALID, "Invalid or oversized control packet");
			goto respond;
		}
		request = parse_json(buffer, (size_t)n);
		if (request)
			reply = dispatch(control, request);
	respond:
		if (!reply) {
			reply = json_object_new_object();
			boolean(reply, "accepted", false);
			string(reply, "error", cell_error);
		}
		const char *text = json_object_to_json_string_ext(
		    reply, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
		send(client, text, strlen(text), MSG_NOSIGNAL);
		close(client);
		json_object_put(request);
		json_object_put(reply);
	}
	return NULL;
}
static int control_start(struct control *control, struct transport *transport)
{
	memset(control, 0, sizeof(*control));
	control->socket = -1;
	control->transport = transport;
	atomic_init(&control->accepting, true);
	pthread_mutex_init(&control->lock, NULL);
	if (random_hex(control->broker, 16) < 0)
		return -1;
	struct stat st;
	if (!lstat(CELL_RUN "/control.sock", &st)) {
		if (!S_ISSOCK(st.st_mode))
			return errorf(CELL_SYSTEM, "Unexpected cellular socket path");
		if (unlink(CELL_RUN "/control.sock"))
			return errorf(CELL_SYSTEM, "Remove stale control socket: %s",
				      strerror(errno));
	} else if (errno != ENOENT)
		return errorf(CELL_SYSTEM, "Inspect control socket: %s", strerror(errno));
	control->socket = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (control->socket < 0)
		return errorf(CELL_SYSTEM, "Create control socket: %s", strerror(errno));
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	snprintf(addr.sun_path, sizeof(addr.sun_path), CELL_RUN "/control.sock");
	if (bind(control->socket, (void *)&addr, sizeof(addr)) || chmod(addr.sun_path, 0600) ||
	    listen(control->socket, 4))
		return errorf(CELL_SYSTEM, "Bind control socket: %s", strerror(errno));
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 512 * 1024);
	int rc = pthread_create(&control->thread, &attr, serve, control);
	pthread_attr_destroy(&attr);
	if (rc)
		return errorf(CELL_SYSTEM, "Create control thread: %s", strerror(rc));
	control->started = true;
	return 0;
}
static J *take(struct control *control, J **args)
{
	pthread_mutex_lock(&control->lock);
	J *job = control->job;
	*args = control->arguments;
	control->job = control->arguments = NULL;
	if (job)
		control->busy = true;
	pthread_mutex_unlock(&control->lock);
	return job;
}
static bool foreground_pending(void *opaque)
{
	struct control *control = opaque;
	pthread_mutex_lock(&control->lock);
	bool pending = control->job != NULL || control->busy;
	pthread_mutex_unlock(&control->lock);
	return pending || json_object_array_length(control->transport->sms_events) > 0;
}
static int finish(struct control *control, J *row)
{
	pthread_mutex_lock(&control->lock);
	int rc = receipt_finish(row);
	if (!rc)
		rc = atomic_json(CELL_RUN "/operation.json", row, false);
	remember(control, row);
	control->busy = false;
	pthread_mutex_unlock(&control->lock);
	return rc;
}
static void control_close(struct control *control)
{
	atomic_store(&control->accepting, false);
	if (control->started)
		pthread_join(control->thread, NULL);
	if (control->socket >= 0) {
		close(control->socket);
		unlink(CELL_RUN "/control.sock");
	}
	json_object_put(control->job);
	json_object_put(control->arguments);
	for (size_t i = 0; i < 32; i++)
		json_object_put(control->receipts[i]);
	pthread_mutex_destroy(&control->lock);
}
static void failed(struct controller *c, const char *prefix)
{
	char saved[512];
	snprintf(saved, sizeof(saved), "%s", cell_error);
	snprintf(c->error, sizeof(c->error), "%s%.450s", prefix ? prefix : "", saved);
	if (c->transport.poisoned)
		controller_set_data(c, NULL);
	else
		controller_publish(c);
}
int main(void)
{
	signal(SIGTERM, stop);
	signal(SIGINT, stop);
	signal(SIGPIPE, SIG_IGN);
	openlog("udx-cellular", LOG_PID, LOG_DAEMON);
	double deadline = mono() + 180;
	struct owner_status owner;
	while (owner_query(&owner) < 0) {
		if (!running || mono() >= deadline) {
			fprintf(stderr, "%s\n", cell_error);
			return 1;
		}
		delay_ms(500);
	}
	close(owner.pidfd);
	struct controller *c = calloc(1, sizeof(*c));
	if (!c)
		return 1;
	if (controller_init(c) < 0) {
		fprintf(stderr, "%s\n", cell_error);
		controller_destroy(c);
		free(c);
		return 1;
	}
	struct control control;
	if (controller_publish(c) < 0) {
		controller_destroy(c);
		free(c);
		return 1;
	}
	J *history = sms_publish(c);
	if (!history) {
		fprintf(stderr, "%s\n", cell_error);
		controller_destroy(c);
		free(c);
		return 1;
	}
	json_object_put(history);
	if (control_start(&control, &c->transport) < 0) {
		fprintf(stderr, "%s\n", cell_error);
		control_close(&control);
		controller_destroy(c);
		free(c);
		return 1;
	}
	c->foreground_pending = foreground_pending;
	c->foreground_context = &control;
	double last = 0, next_connect = 0, next_reopen = 0;
	bool quiesced = false;
	int exit_status = 0;
	while (running) {
		double now = mono();
		J *args = NULL, *job = take(&control, &args);
		if (c->transport.poisoned && now >= next_reopen &&
		    !(job && text_equal(str(job, "operation"), "quiesce"))) {
			if (transport_reopen(&c->transport) < 0)
				snprintf(c->error, sizeof(c->error), "%s", cell_error);
			else {
				c->initialized = c->ims_initialized = false;
				memset(c->telemetry_due, 0, sizeof(c->telemetry_due));
				c->error[0] = 0;
				controller_publish(c);
			}
			next_reopen = mono() + 15;
		}
		if (job) {
			string(job, "state", "running");
			real(job, "started_uptime", mono());
			pthread_mutex_lock(&control.lock);
			remember(&control, job);
			pthread_mutex_unlock(&control.lock);
			int recorded = atomic_json(CELL_RUN "/operation.json", job, false);
			J *result =
			    recorded ? NULL : controller_execute(c, str(job, "operation"), args);
			if (result) {
				json_object_object_add(job, "result", result);
				bool debug_failed =
				    text_equal(str(job, "operation"), "at_debug") &&
				    !json_object_get_boolean(get(result, "success"));
				string(job, "state",
				       !debug_failed	       ? "complete"
				       : c->transport.poisoned ? "uncertain"
							       : "failed");
				if (debug_failed)
					string(job, "error", str(result, "error"));
			} else {
				string(job, "state",
				       c->transport.poisoned ? "uncertain" : "failed");
				string(job, "error", cell_error);
				failed(c, NULL);
			}
			real(job, "finished_uptime", mono());
			if (finish(&control, job) < 0) {
				failed(c, "Receipt persistence: ");
				exit_status = 1;
				running = 0;
			}
			controller_publish(c);
			last = 0;
			quiesced = text_equal(str(job, "operation"), "quiesce") &&
				   text_equal(str(job, "state"), "complete");
			json_object_put(job);
			json_object_put(args);
			if (quiesced || !running)
				break;
		}
		if (!c->transport.poisoned && c->radio_restore_pending && mono() - last >= 10) {
			if (radio_restore(c, c->restore_radio, c->restore_data) < 0)
				failed(c, "Radio restoration: ");
		}
		if (!c->transport.poisoned && !radio_enabled(c) && mono() - last >= 10 &&
		    radio_restore(c, false, c->want_data) < 0)
			failed(c, "Radio-off policy: ");
		if (!c->transport.poisoned && mono() - last >= 10) {
			c->background = true;
			c->background_deferred = false;
			if (controller_refresh(c) < 0 && !c->background_deferred)
				failed(c, NULL);
			c->background = false;
			last = c->background_deferred ? 0 : mono();
		}
		if (radio_enabled(c) && !c->radio_restore_pending && c->want_data && !c->data &&
		    !c->transport.poisoned && mono() >= next_connect) {
			J *result = controller_connect(c, 10);
			if (!result) {
				failed(c, NULL);
				controller_set_data(c, NULL);
			}
			json_object_put(result);
			next_connect = mono() + 15;
		}
		if (!c->transport.poisoned &&
		    (transport_drain(&c->transport) < 0 || sms_process_events(c) < 0))
			failed(c, "SMS receive: ");
		if (!c->transport.poisoned && telemetry_step(c) < 0)
			failed(c, "Telemetry: ");
		delay_ms(200);
	}
	control_close(&control);
	c->want_data = false;
	char shutdown_error[512] = "";
	if (!quiesced) {
		J *result = controller_disconnect(c, true);
		if (!result) {
			snprintf(shutdown_error, sizeof(shutdown_error), "%s", cell_error);
			failed(c, "shutdown: ");
			controller_set_data(c, NULL);
		}
		json_object_put(result);
	}
	bool poisoned = c->transport.poisoned;
	transport_close(&c->transport);
	J *row = json_object_new_object();
	string(row, "boot_id", c->transport.boot);
	integer(row, "pid", getpid());
	boolean(row, "network_retired", !*shutdown_error);
	boolean(row, "at_closed", true);
	string(row, "pdp_state",
	       poisoned || *shutdown_error   ? "unknown"
	       : str(c->status, "pdp_state") ? str(c->status, "pdp_state")
					     : "unknown");
	string(row, "error", *shutdown_error ? shutdown_error : NULL);
	real(row, "uptime", mono());
	if (atomic_json(CELL_RUN "/shutdown.json", row, false) < 0)
		exit_status = 1;
	json_object_put(row);
	controller_destroy(c);
	free(c);
	closelog();
	return exit_status;
}
