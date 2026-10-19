/* SPDX-License-Identifier: GPL-2.0-only */
#include "cellular.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef CELL_PROVIDERS
#define CELL_PROVIDERS "/usr/share/udx-cellular/providers.json"
#endif
static bool plmn_valid(const char *s)
{
	if (!s || (strlen(s) != 5 && strlen(s) != 6))
		return false;
	for (; *s; s++)
		if (!isdigit((unsigned char)*s))
			return false;
	return true;
}
/* PLMN aliases from the pinned AOSP Internet profiles in providers.json. */
const char *operator_brand(const char *plmn)
{
	static const char *const aliases[][2] = {
	    {"46000", "china_mobile"},	{"46002", "china_mobile"},   {"46004", "china_mobile"},
	    {"46007", "china_mobile"},	{"46008", "china_mobile"},   {"46013", "china_mobile"},
	    {"46001", "china_unicom"},	{"46009", "china_unicom"},   {"46003", "china_telecom"},
	    {"46011", "china_telecom"}, {"46015", "china_broadnet"}, {"46060", "china_broadnet"}};
	for (size_t i = 0; i < sizeof(aliases) / sizeof(*aliases); i++)
		if (text_equal(plmn, aliases[i][0]))
			return aliases[i][1];
	return NULL;
}
int operator_validate(const char *op, J *args)
{
	if (!strcmp(op, "operators_scan") || !strcmp(op, "operator_auto") ||
	    !strcmp(op, "operator_catalog"))
		return 0;
	int64_t act = json_object_get_int64(get(args, "act"));
	return plmn_valid(str(args, "plmn")) &&
		       json_object_is_type(get(args, "act"), json_type_int) && act >= 0 && act <= 13
		   ? 0
		   : errorf(CELL_INVALID, "Invalid PLMN or radio technology");
}
J *operator_catalog(struct controller *c)
{
	J *all = read_json(CELL_PROVIDERS, 262144);
	if (!all)
		return NULL;
	J *rows = get(all, "providers"), *selected = json_object_new_array();
	const char *imsi = str(c->status, "imsi");
	size_t longest = 0;
	if (imsi)
		for (size_t i = 0; i < json_object_array_length(rows); i++) {
			const char *plmn = str(json_object_array_get_idx(rows, i), "plmn");
			if (plmn_valid(plmn) && !strncmp(imsi, plmn, strlen(plmn)) &&
			    strlen(plmn) > longest)
				longest = strlen(plmn);
		}
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		J *r = json_object_array_get_idx(rows, i);
		const char *plmn = str(r, "plmn");
		if (imsi && plmn_valid(plmn) && strlen(plmn) == longest &&
		    !strncmp(imsi, plmn, longest))
			json_object_array_add(selected, json_object_get(r));
	}
	json_object_object_add(all, "home_candidates", selected);
	if (json_object_array_length(selected))
		string(all, "home_plmn", str(json_object_array_get_idx(selected, 0), "plmn"));
	json_object_object_del(all, "providers");
	return all;
}
/* A single Internet candidate per SIM, only after a definite activation rejection.
 * Roaming uses the SIM's home identity; manual credentials are never overwritten. */
int operator_fallback(struct controller *c)
{
	if (c->fallback_attempted || !text_equal(str(c->config, "apn_mode"), "auto"))
		return 0;
	c->fallback_attempted = true;
	if (info_sim_identity(c) < 0)
		return -1;
	J *catalog = operator_catalog(c);
	if (!catalog)
		return -1;
	J *rows = get(catalog, "home_candidates");
	for (size_t i = 0; i < json_object_array_length(rows); i++) {
		J *r = json_object_array_get_idx(rows, i);
		if (get(r, "mvno_type"))
			continue;
		J *profile = json_object_new_object();
		string(profile, "apn", str(r, "apn"));
		string(profile, "pdp_type", str(r, "pdp_type"));
		integer(profile, "cid", 1);
		int valid = profile_validate(profile);
		json_object_put(profile);
		if (valid < 0 || apn_auth_validate(r) < 0)
			continue;
		json_object_put(c->fallback_profile);
		c->fallback_profile = jclone(r);
		string(c->status, "automatic_apn", str(r, "apn"));
		string(c->status, "home_plmn", str(r, "plmn"));
		json_object_put(catalog);
		return 1;
	}
	json_object_put(catalog);
	return 0;
}
J *operator_parse_scan(const char *raw)
{
	if (!at_complete(raw)) {
		errorf(CELL_INVALID, "Incomplete operator scan");
		return NULL;
	}
	const char *p = strstr(raw, "+COPS:");
	if (!p) {
		errorf(CELL_INVALID, "Missing operator scan");
		return NULL;
	}
	p += 6;
	J *rows = json_object_new_array();
	while (*p && *p != '\r' && *p != '\n') {
		while (*p == ' ' || *p == ',')
			p++;
		if (*p != '(')
			break;
		const char *start = ++p;
		bool quote = false;
		for (; *p && *p != '\r' && *p != '\n'; p++) {
			if (*p == '"')
				quote = !quote;
			else if (*p == ')' && !quote)
				break;
		}
		if (*p != ')' || p - start > 1024)
			goto bad;
		char wire[1100];
		snprintf(wire, sizeof(wire), "+ROW:%.*s\r\nOK\r\n", (int)(p - start), start);
		p++;
		J *fields = at_one(wire, "+ROW:");
		if (!fields)
			goto bad;
		size_t n = json_object_array_length(fields);
		if (memchr(start, '"', (size_t)(p - start - 1))) {
			const char *plmn =
			    json_object_get_string(json_object_array_get_idx(fields, 3));
			int64_t status, act = 0;
			if (json_object_array_length(rows) >= 64 || (n != 4 && n != 5) ||
			    !plmn_valid(plmn) ||
			    !integer_text(
				json_object_get_string(json_object_array_get_idx(fields, 0)), 0, 3,
				&status) ||
			    (n == 5 && !integer_text(json_object_get_string(
							 json_object_array_get_idx(fields, 4)),
						     0, 13, &act))) {
				json_object_put(fields);
				goto bad;
			}
			J *r = json_object_new_object();
			integer(r, "status", status);
			integer(r, "act", act);
			string(r, "plmn", plmn);
			string(r, "name",
			       json_object_get_string(json_object_array_get_idx(fields, 1)));
			string(r, "short_name",
			       json_object_get_string(json_object_array_get_idx(fields, 2)));
			json_object_array_add(rows, r);
		}
		json_object_put(fields);
	}
	return rows;
bad:
	json_object_put(rows);
	errorf(CELL_INVALID, "Malformed operator scan");
	return NULL;
}
J *operator_execute(struct controller *c, const char *op, J *args)
{
	if (!strcmp(op, "operator_catalog"))
		return operator_catalog(c);
	if (!radio_enabled(c)) {
		errorf(CELL_INVALID, "Enable radio before operator selection");
		return NULL;
	}
	if (!strcmp(op, "operators_scan")) {
		char *raw = controller_at(c, "AT+COPS=?", 180);
		J *rows = raw ? operator_parse_scan(raw) : NULL;
		free(raw);
		if (!rows)
			return NULL;
		J *out = json_object_new_object();
		json_object_object_add(out, "operators", rows);
		return out;
	}
	bool wanted = c->want_data;
	c->want_data = false;
	J *disconnected = controller_disconnect(c, false);
	if (!disconnected) {
		c->want_data = wanted;
		return NULL;
	}
	json_object_put(disconnected);
	J *out = NULL, *fields = NULL;
	char *raw = NULL;
	char wire[80];
	/* Configuration permits RF; verify/enable the actual modem before COPS. */
	if (controller_radio_on(c) < 0)
		goto done;
	if (!strcmp(op, "operator_auto"))
		snprintf(wire, sizeof(wire), "AT+COPS=0");
	else
		snprintf(wire, sizeof(wire), "AT+COPS=1,2,\"%s\",%d", str(args, "plmn"),
			 json_object_get_int(get(args, "act")));
	raw = controller_at(c, wire, 180);
	if (!raw)
		goto done;
	free(raw);
	raw = controller_at(c, "AT+COPS=3,2", 5);
	if (!raw)
		goto done;
	fields = controller_query(c, "AT+COPS?", 10, "+COPS:");
	if (!fields)
		goto done;
	bool automatic = !strcmp(op, "operator_auto");
	bool verified =
	    text_equal(json_object_get_string(json_object_array_get_idx(fields, 0)),
		       automatic ? "0" : "1") &&
	    (automatic ||
	     (text_equal(json_object_get_string(json_object_array_get_idx(fields, 2)),
			 str(args, "plmn")) &&
	      text_equal(json_object_get_string(json_object_array_get_idx(fields, 1)), "2")));
	if (!verified) {
		errorf(CELL_MODEM, "Operator selection readback mismatch");
		goto done;
	}
	out = json_object_new_object();
	boolean(out, "readback_verified", true);
done:
	free(raw);
	json_object_put(fields);
	c->want_data = wanted;
	return out;
}
