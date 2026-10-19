/* SPDX-License-Identifier: GPL-2.0-only Typed rpcd adapter; never opens an AT
 * device. */
#include "common.h"
#include "frequency.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#ifndef DEVICE_ROOT
#define DEVICE_ROOT ""
#endif
static const char *cellular_allowed[] = {"status",
					 "refresh",
					 "connect",
					 "disconnect",
					 "radio",
					 "apn",
					 "advanced_refresh",
					 "at_debug",
					 "network_mode",
					 "network_preset",
					 "band_lock",
					 "cell_lock",
					 "unlock",
					 "sms_refresh",
					 "sms_send",
					 "sms_delete",
					 "cells_refresh",
					 "modem_info",
					 "policy",
					 "apn_templates",
					 "apn_template_save",
					 "apn_template_delete",
					 "apn_select",
					 "sms_history",
					 "sms_settings",
					 "sms_mark_read",
					 "sms_delete_history",
					 "radio_options",
					 "nr_capability",
					 "sms_delete_batch",
					 "physical_slot",
					 "operation_status",
					 "network_configure",
					 "operator_catalog",
					 "operators_scan",
					 "operator_auto",
					 "operator_select",
					 "imei_read",
					 "imei_write"};
static const char *method_description =
    "{\"frequency_status\":{},\"frequency_apply\":{\"cpu_governor\":\"\",\"cpu_min_khz\":0,\"cpu_"
    "max_khz\":0,\"ddr_governor\":\"\",\"ddr_frequency_hz\":0},"
    "\"status\":{},\"sms_status\":{},\"apn_"
    "status\":{},\"sms_history_status\":"
    "{},\"sms_receipt\":{"
    "\"request_id\":\"\"},\"cellular\":{\"operation\":\"\",\"arguments\":{}},"
    "\"usb\":{\"profile\":"
    "\"\",\"acm\":false}}";
static char *read_file(const char *name, size_t maximum)
{
	char path[PATH_MAX];
	if (snprintf(path, sizeof(path), "%s%s", DEVICE_ROOT, name) >= (int)sizeof(path))
		return NULL;
	return read_text(path, maximum, true);
}
static void text_file(J *o, const char *key, const char *name)
{
	char *value = read_file(name, 65536);
	string(o, key, value);
	free(value);
}
static bool file_exists(const char *name)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s%s", DEVICE_ROOT, name);
	struct stat st;
	return !stat(path, &st);
}
static void number_file(J *o, const char *key, const char *name, double divisor)
{
	char *s = read_file(name, 256);
	char *end = NULL;
	errno = 0;
	double value = s ? strtod(s, &end) : NAN;
	bool valid = s && *s && end && !*end && !errno && isfinite(value / divisor);
	if (valid)
		real(o, key, value / divisor);
	else
		json_object_object_add(o, key, NULL);
	free(s);
}
static J *document(const char *name, size_t maximum)
{
	char *raw = read_file(name, maximum);
	J *row = raw ? parse_json_document(raw, strlen(raw)) : NULL;
	free(raw);
	if (!json_object_is_type(row, json_type_object)) {
		json_object_put(row);
		row = json_object_new_object();
	}
	return row;
}
static J *unavailable(const char *error)
{
	J *out = json_object_new_object();
	boolean(out, "available", false);
	string(out, "error", error);
	return out;
}
static J *current_document(const char *name, double limit, size_t maximum)
{
	J *row = document(name, maximum);
	if (!json_object_object_length(row)) {
		json_object_put(row);
		return unavailable("Not available");
	}
	char *boot = read_file("/proc/sys/kernel/random/boot_id", 64);
	bool matches = text_equal(str(row, "boot_id"), boot);
	free(boot);
	if (!matches) {
		json_object_put(row);
		return unavailable("Status for the current boot is not available");
	}
	if (limit >= 0) {
		J *stamp = get(row, "uptime");
		double age = mono() - json_object_get_double(stamp);
		if ((!json_object_is_type(stamp, json_type_double) &&
		     !json_object_is_type(stamp, json_type_int)) ||
		    age < 0 || age > limit) {
			json_object_put(row);
			row = unavailable("Cached status has expired; fresh data is not available");
			boolean(row, "stale", true);
			return row;
		}
	}
	boolean(row, "available", true);
	return row;
}
static J *pairs(const char *raw)
{
	J *out = json_object_new_object();
	if (!raw)
		return out;
	char *copy = strdup(raw), *save = NULL;
	if (!copy)
		return out;
	for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *equals = strchr(line, '=');
		if (!equals || equals == line)
			continue;
		bool valid = true;
		for (char *p = line; p < equals; p++)
			if ((*p < 'a' || *p > 'z') && *p != '_')
				valid = false;
		if (valid) {
			*equals = 0;
			string(out, line, equals + 1);
		}
	}
	free(copy);
	return out;
}
static int request(const char *endpoint, const char *payload, char *response, size_t capacity)
{
	char path[PATH_MAX];
	if (snprintf(path, sizeof(path), "%s%s", DEVICE_ROOT, endpoint) >= (int)sizeof(path))
		return errorf(CELL_INVALID, "Socket path too long");
	return seqpacket(path, payload, strlen(payload), response, capacity, 3000, NULL);
}
static J *runtime_status(void)
{
	char raw[1025];
	int n = request("/run/udx-runtime.sock", "status\n", raw, sizeof(raw));
	if (n < 0)
		return unavailable(cell_error);
	if (n < 17 || strncmp(raw, "RUNTIME_STATUS1 ", 16) || raw[n - 1] != '\n')
		return unavailable("Invalid runtime response");
	for (char *s = raw + 16; *s; s++)
		if (*s == ' ')
			*s = '\n';
	J *value = pairs(raw + 16);
	char *boot = read_file("/proc/sys/kernel/random/boot_id", 64);
	bool matches = text_equal(str(value, "boot"), boot);
	free(boot);
	if (!matches) {
		json_object_put(value);
		return unavailable("Runtime boot identity mismatch");
	}
	boolean(value, "available", true);
	return value;
}
static J *usb_status(void)
{
	char *raw = read_file("/run/udx-usb/applied", 65536);
	J *out = pairs(raw);
	free(raw);
	raw = read_file("/run/udx-usb/last-result", 65536);
	J *last = pairs(raw);
	free(raw);
	merge(out, last);
	json_object_put(last);
	text_file(out, "pending", "/run/udx-usb/pending");
	text_file(out, "udc", "/sys/kernel/config/usb_gadget/udx-usb/UDC");
	text_file(out, "state", "/sys/class/udc/29100000.dwc3/state");
	text_file(out, "speed", "/sys/class/udc/29100000.dwc3/current_speed");
	boolean(out, "available", get(out, "state") != NULL);
	J *profiles = json_object_new_array();
	json_object_array_add(profiles, json_object_new_string("ncm"));
	json_object_array_add(profiles, json_object_new_string("ncm-software"));
	json_object_object_add(out, "profiles", profiles);
	text_file(out, "offload_state", "/sys/bus/platform/devices/2b300000.pamu3/offload_state");
	return out;
}
static J *kernel_ipv6(void)
{
	char *raw = read_file("/proc/net/if_inet6", 65536);
	if (!raw)
		return unavailable("Kernel IPv6 addresses are not available");
	J *addresses = json_object_new_array();
	char *save = NULL;
	for (char *line = strtok_r(raw, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char hex[33], device[32];
		unsigned index, prefix, scope, flags;
		int used = 0;
		if (sscanf(line, "%32[0-9a-fA-F] %x %x %x %x %31s%n", hex, &index, &prefix, &scope,
			   &flags, device, &used) != 6 ||
		    line[used] || strcmp(device, "sipa_eth0") || strlen(hex) != 32 ||
		    prefix > 128 || scope || flags & 0x48)
			continue;
		struct in6_addr address;
		for (size_t i = 0; i < 16; i++) {
			char byte[3] = {hex[2 * i], hex[2 * i + 1], 0};
			address.s6_addr[i] = (unsigned char)strtoul(byte, NULL, 16);
		}
		if (IN6_IS_ADDR_LINKLOCAL(&address) || IN6_IS_ADDR_LOOPBACK(&address) ||
		    IN6_IS_ADDR_UNSPECIFIED(&address) || IN6_IS_ADDR_MULTICAST(&address))
			continue;
		char text[INET6_ADDRSTRLEN];
		inet_ntop(AF_INET6, &address, text, sizeof(text));
		bool duplicate = false;
		for (size_t i = 0; i < json_object_array_length(addresses); i++)
			if (text_equal(
				json_object_get_string(json_object_array_get_idx(addresses, i)),
				text))
				duplicate = true;
		if (!duplicate)
			json_object_array_add(addresses, json_object_new_string(text));
	}
	free(raw);
	J *out = json_object_new_object();
	boolean(out, "available", true);
	string(out, "device", "sipa_eth0");
	json_object_object_add(out, "ipv6_global_addresses", addresses);
	return out;
}
static void child_path(char out[PATH_MAX], const char *base, const char *file)
{
	snprintf(out, PATH_MAX, "%s/%s", base, file);
}
static J *sysfs_array(const char *pattern, int kind)
{
	char rooted[PATH_MAX];
	snprintf(rooted, sizeof(rooted), "%s%s", DEVICE_ROOT, pattern);
	glob_t paths = {0};
	J *out = json_object_new_array();
	glob(rooted, 0, NULL, &paths);
	for (size_t i = 0; i < paths.gl_pathc; i++) {
		const char *path = paths.gl_pathv[i] + strlen(DEVICE_ROOT),
			   *name = strrchr(path, '/');
		name = name ? name + 1 : path;
		char sub[PATH_MAX];
		if (kind == 1) {
			child_path(sub, path, "device/ap_vote_hz");
			if (!strstr(name, "ddr") && !file_exists(sub))
				continue;
		}
		if (kind == 3 && !strcmp(name, "lo"))
			continue;
		J *row = json_object_new_object();
		if (kind == 0) {
			string(row, "id", name);
			child_path(sub, path, "type");
			text_file(row, "name", sub);
			child_path(sub, path, "temp");
			number_file(row, "temperature_c", sub, 1000);
		} else if (kind == 1) {
			string(row, "name", name);
			child_path(sub, path, "cur_freq");
			number_file(row, "current_hz", sub, 1);
			child_path(sub, path, "available_frequencies");
			text_file(row, "available_frequencies", sub);
			child_path(sub, path, "governor");
			text_file(row, "governor", sub);
			const char *keys[] = {"ap_vote_hz", "cp_vote_hz", "firmware_dfs_enabled",
					      "firmware_auto_enabled"};
			for (size_t j = 0; j < 4; j++) {
				char file[96];
				snprintf(file, sizeof(file), "device/%s", keys[j]);
				child_path(sub, path, file);
				number_file(row, keys[j], sub, 1);
			}
		} else if (kind == 2) {
			string(row, "device", name);
			const char *text[] = {"name", "type"},
				   *numeric[] = {"data_bytes", "reserved_ebs"};
			for (size_t j = 0; j < 2; j++) {
				child_path(sub, path, text[j]);
				text_file(row, text[j], sub);
				child_path(sub, path, numeric[j]);
				number_file(row, numeric[j], sub, 1);
			}
		} else if (kind == 3) {
			string(row, "name", name);
			child_path(sub, path, "operstate");
			text_file(row, "operstate", sub);
			child_path(sub, path, "statistics/rx_bytes");
			number_file(row, "rx_bytes", sub, 1);
			child_path(sub, path, "statistics/tx_bytes");
			number_file(row, "tx_bytes", sub, 1);
		} else {
			string(row, "policy", name);
			child_path(sub, path, "scaling_cur_freq");
			number_file(row, "current_khz", sub, 1);
			child_path(sub, path, "scaling_governor");
			text_file(row, "governor", sub);
			child_path(sub, path, "scaling_min_freq");
			number_file(row, "min_khz", sub, 1);
			child_path(sub, path, "scaling_max_freq");
			number_file(row, "max_khz", sub, 1);
		}
		json_object_array_add(out, row);
	}
	globfree(&paths);
	return out;
}
static J *storage_status(void)
{
	J *out = json_object_new_array();
	char *raw = read_file("/proc/mounts", 65536), *save = NULL;
	if (!raw)
		return out;
	for (char *line = strtok_r(raw, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char source[256], mount[PATH_MAX], type[64], options[512];
		if (sscanf(line, "%255s %4095s %63s %511s", source, mount, type, options) != 4 ||
		    (strcmp(mount, "/") && strcmp(mount, "/rom") && strcmp(mount, "/overlay") &&
		     strcmp(mount, "/tmp")))
			continue;
		J *row = json_object_new_object();
		string(row, "mount", mount);
		string(row, "source", source);
		string(row, "type", type);
		bool ro = false;
		char *next = NULL;
		for (char *opt = strtok_r(options, ",", &next); opt;
		     opt = strtok_r(NULL, ",", &next))
			if (!strcmp(opt, "ro"))
				ro = true;
		boolean(row, "readonly", ro);
		char path[PATH_MAX];
		int path_length = snprintf(path, sizeof(path), "%s%s", DEVICE_ROOT, mount);
		struct statvfs fs;
		if (path_length >= (int)sizeof(path) || statvfs(path, &fs)) {
			json_object_object_add(row, "total_bytes", NULL);
			json_object_object_add(row, "free_bytes", NULL);
		} else {
			integer(row, "total_bytes", fs.f_blocks * fs.f_frsize);
			integer(row, "free_bytes", fs.f_bavail * fs.f_frsize);
		}
		json_object_array_add(out, row);
	}
	free(raw);
	return out;
}
static J *status(void)
{
	J *out = json_object_new_object();
	real(out, "sample_uptime", mono());
	J *board = json_object_new_object();
	text_file(board, "model", "/sys/firmware/devicetree/base/model");
	text_file(board, "kernel", "/proc/sys/kernel/osrelease");
	text_file(board, "boot_id", "/proc/sys/kernel/random/boot_id");
	json_object_object_add(out, "board", board);
	J *boot = json_object_new_object();
	text_file(boot, "policy", "/sys/firmware/devicetree/base/chosen/udx,boot-policy");
	text_file(boot, "target", "/sys/firmware/devicetree/base/chosen/udx,boot-target");
	text_file(boot, "root_marker", "/etc/udx-openwrt-root");
	json_object_object_add(out, "boot", boot);
	json_object_object_add(out, "runtime", runtime_status());
	json_object_object_add(out, "thermal", sysfs_array("/sys/class/thermal/thermal_zone*", 0));
	json_object_object_add(out, "ddr", sysfs_array("/sys/class/devfreq/*", 1));
	json_object_object_add(out, "cpu",
			       sysfs_array("/sys/devices/system/cpu/cpufreq/policy*", 4));
	json_object_object_add(out, "storage", storage_status());
	json_object_object_add(out, "ubi", sysfs_array("/sys/class/ubi/ubi[0-9]*_[0-9]*", 2));
	json_object_object_add(out, "interfaces", sysfs_array("/sys/class/net/*", 3));
	json_object_object_add(out, "usb", usb_status());
	J *upgrade = json_object_new_object();
	boolean(upgrade, "enabled", file_exists("/lib/upgrade/platform.sh"));
	json_object_object_add(out, "upgrade", upgrade);
	json_object_object_add(
	    out, "cellular",
	    current_document("/run/udx-cellular/cellular-status.json", 35, 262144));
	json_object_object_add(out, "cellular_operation",
			       current_document("/run/udx-cellular/operation.json", -1, 262144));
	json_object_object_add(out, "cellular_kernel", kernel_ipv6());
	json_object_object_add(
	    out, "cellular_netifd",
	    current_document("/run/udx-cellular/cellular-netifd.json", 35, 262144));
	return out;
}
static J *rejected(const char *error, bool uncertain)
{
	J *out = json_object_new_object();
	boolean(out, "accepted", false);
	if (uncertain)
		boolean(out, "uncertain", true);
	string(out, "error", error);
	return out;
}
static J *cellular_request(J *arguments)
{
	const char *operation = str(arguments, "operation");
	bool valid = false;
	for (size_t i = 0; i < sizeof(cellular_allowed) / sizeof(*cellular_allowed); i++)
		if (text_equal(operation, cellular_allowed[i]))
			valid = true;
	if (!valid) {
		errorf(CELL_INVALID, "Unknown cellular operation");
		return NULL;
	}
	J *message = json_object_new_object();
	string(message, "operation", operation);
	json_object_object_add(message, "arguments", json_object_get(get(arguments, "arguments")));
	const char *wire = json_object_to_json_string_ext(
	    message, JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
	if (strlen(wire) > 8192) {
		json_object_put(message);
		errorf(CELL_INVALID, "Request too large");
		return NULL;
	}
	char *raw = malloc(65537);
	if (!raw) {
		json_object_put(message);
		errorf(CELL_SYSTEM, "Response allocation failed");
		return NULL;
	}
	int n = request("/run/udx-cellular/control.sock", wire, raw, 65537);
	json_object_put(message);
	J *reply = n >= 0 ? parse_json(raw, (size_t)n) : NULL;
	free(raw);
	if (!reply || !json_object_is_type(get(reply, "accepted"), json_type_boolean)) {
		json_object_put(reply);
		char error[640];
		snprintf(error, sizeof(error), "Submission result is not available: %s",
			 n < 0 ? cell_error : "Invalid cellular response");
		return rejected(error, true);
	}
	J *row = current_document("/run/udx-cellular/operation.json", -1, 262144);
	if (json_object_get_boolean(get(reply, "accepted")) && get(reply, "operation_id") &&
	    json_object_equal(get(row, "id"), get(reply, "operation_id")) &&
	    text_equal(str(row, "operation"), operation) &&
	    (!str(reply, "broker_id") ||
	     text_equal(str(row, "broker_id"), str(reply, "broker_id")))) {
		J *ref = json_object_new_object();
		const char *keys[] = {"id", "boot_id", "broker_id", "operation"};
		for (size_t i = 0; i < 4; i++)
			json_object_object_add(ref, keys[i], json_object_get(get(row, keys[i])));
		json_object_object_add(reply, "operation_ref", ref);
	}
	json_object_put(row);
	return reply;
}
static J *sms_receipt(const char *id)
{
	if (!hex_string(id, 32)) {
		errorf(CELL_INVALID, "Invalid request identifier");
		return NULL;
	}
	char path[256];
	snprintf(path, sizeof(path), "/etc/udx-cellular/requests/%s.json", id);
	J *row = document(path, 262144);
	if (!text_equal(str(row, "request_id"), id) ||
	    !text_equal(str(row, "operation"), "sms_send")) {
		json_object_put(row);
		row = unavailable("Send receipt is not available");
		string(row, "request_id", id);
		return row;
	}
	J *active = current_document("/run/udx-cellular/operation.json", -1, 262144);
	if (text_equal(str(active, "request_id"), id)) {
		json_object_put(row);
		row = active;
		active = NULL;
	} else if (text_equal(str(row, "state"), "queued") ||
		   text_equal(str(row, "state"), "running")) {
		string(row, "state", "uncertain");
		string(row, "error", "A final send receipt is not available yet");
	}
	json_object_put(active);
	J *progress = document("/run/udx-cellular/sms-send-progress.json", 262144);
	if (text_equal(str(progress, "request_id"), id))
		json_object_object_add(row, "submission_progress", json_object_get(progress));
	json_object_put(progress);
	boolean(row, "available", true);
	return row;
}
static J *usb_request(J *arguments)
{
	const char *profile = str(arguments, "profile");
	J *acm = get(arguments, "acm");
	if ((!text_equal(profile, "ncm") && !text_equal(profile, "ncm-software")) ||
	    !json_object_is_type(acm, json_type_boolean)) {
		errorf(CELL_INVALID, "Invalid USB profile");
		return NULL;
	}
	const char *argv[] = {"/usr/sbin/udx-usbctl", "switch", profile,
			      json_object_get_boolean(acm) ? "acm" : "no-acm", NULL};
	char *output = NULL;
	int rc = command_run(argv, NULL, 8000, 65536, &output);
	if (rc < 0)
		return rejected(cell_error, cell_error_kind == CELL_UNCERTAIN);
	char expected[80];
	snprintf(expected, sizeof(expected), "queued=switch %s %d", profile,
		 json_object_get_boolean(acm));
	bool queued = false;
	char *save = NULL;
	for (char *line = strtok_r(output, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
		if (!strcmp(line, expected))
			queued = true;
	free(output);
	J *row = json_object_new_object();
	boolean(row, "accepted", queued);
	string(row, "state", queued ? "queued" : "failed");
	string(row, "error", queued ? NULL : "USB request was not queued");
	return row;
}
static J *dispatch(const char *method, J *args)
{
	J *methods = parse_json(method_description, strlen(method_description));
	J *schema = get(methods, method);
	bool valid = schema && json_object_is_type(args, json_type_object) &&
		     json_object_object_length(schema) == json_object_object_length(args);
	if (valid) {
		json_object_object_foreach(schema, key, value)
		{
			(void)value;
			J *entry;
			if (!json_object_object_get_ex(args, key, &entry))
				valid = false;
		}
	}
	json_object_put(methods);
	if (!valid) {
		errorf(CELL_INVALID, "Unknown method or fields");
		return NULL;
	}
	if (!strcmp(method, "frequency_status"))
		return frequency_status();
	if (!strcmp(method, "frequency_apply"))
		return frequency_apply(args, true);
	if (!strcmp(method, "status"))
		return status();
	if (!strcmp(method, "sms_status"))
		return current_document("/run/udx-cellular/sms-status.json", 300, 262144);
	if (!strcmp(method, "sms_history_status"))
		return current_document("/run/udx-cellular/sms-history.json", -1, 524288);
	if (!strcmp(method, "apn_status"))
		return current_document("/run/udx-cellular/apn-status.json", 300, 262144);
	if (!strcmp(method, "sms_receipt"))
		return sms_receipt(str(args, "request_id"));
	if (!strcmp(method, "cellular"))
		return cellular_request(args);
	if (!strcmp(method, "usb"))
		return usb_request(args);
	return NULL;
}
int main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);
	if (argc == 2 && !strcmp(argv[1], "apply-frequency"))
		return frequency_restore();
	if (argc == 2 && !strcmp(argv[1], "list")) {
		puts(method_description);
		return 0;
	}
	if (argc != 3 || strcmp(argv[1], "call"))
		return 2;
	char input[8193];
	size_t n = fread(input, 1, sizeof(input), stdin);
	J *args = NULL, *result = NULL;
	if (n > 8192)
		errorf(CELL_INVALID, "Request too large");
	else if (ferror(stdin))
		errorf(CELL_SYSTEM, "Request read failed");
	else {
		args = parse_json(n ? input : "{}", n ? n : 2);
		if (args) {
			if (!json_object_is_type(args, json_type_object))
				errorf(CELL_INVALID, "Expected object");
			else {
				J *session = NULL;
				bool has =
				    json_object_object_get_ex(args, "ubus_rpc_session", &session);
				if (has && !hex_string(str(args, "ubus_rpc_session"), 32))
					errorf(CELL_INVALID, "Invalid RPC session metadata");
				else {
					if (has)
						json_object_object_del(args, "ubus_rpc_session");
					result = dispatch(argv[2], args);
				}
			}
		}
	}
	if (!result)
		result = rejected(cell_error, false);
	puts(json_object_to_json_string_ext(result, JSON_C_TO_STRING_PLAIN |
							JSON_C_TO_STRING_NOSLASHESCAPE));
	json_object_put(result);
	json_object_put(args);
	return 0;
}
