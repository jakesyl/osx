/* Copyright (c) 2005-2011 Dovecot authors, see the included COPYING file */

#include "auth-common.h"
#include "str.h"
#include "auth-worker-server.h"
#include "password-scheme.h"
#include "passdb.h"
#include "passdb-blocking.h"

#include <stdlib.h>

static void
auth_worker_reply_parse_args(struct auth_request *request,
			     const char *const *args)
{
	if (**args != '\0')
		request->passdb_password = p_strdup(request->pool, *args);
	args++;

	if (*args != NULL) {
		i_assert(auth_stream_is_empty(request->extra_fields) ||
			 request->master_user != NULL ||
			 request->submit_user != NULL);   /* APPLE - urlauth */
		auth_request_set_fields(request, args, NULL);
	}
}

static enum passdb_result
auth_worker_reply_parse(struct auth_request *request, const char *reply)
{
	enum passdb_result ret;
	const char *const *args;

	args = t_strsplit(reply, "\t");

	if (strcmp(*args, "OK") == 0 && args[1] != NULL && args[2] != NULL) {
		/* OK \t user \t password [\t extra] */
		auth_request_set_field(request, "user", args[1], NULL);
		auth_worker_reply_parse_args(request, args + 2);
		return PASSDB_RESULT_OK;
	}

	if (strcmp(*args, "FAIL") == 0 && args[1] != NULL) {
		/* FAIL \t result [\t user \t password [\t extra]] */
		ret = atoi(args[1]);
		if (ret == PASSDB_RESULT_OK) {
			/* shouldn't happen */
		} else if (args[2] == NULL) {
			/* internal failure most likely */
			return ret;
		} else if (args[3] != NULL) {
			if (*args[2] != '\0') {
				auth_request_set_field(request, "user",
						       args[2], NULL);
			}
			auth_worker_reply_parse_args(request, args + 3);
			return ret;
		}
	}

	auth_request_log_error(request, "blocking",
		"Received invalid reply from worker: %s", reply);
	return PASSDB_RESULT_INTERNAL_FAILURE;
}

static bool
verify_plain_callback(const char *reply, void *context)
{
	struct auth_request *request = context;
	enum passdb_result result;

	result = auth_worker_reply_parse(request, reply);
	auth_request_verify_plain_callback(result, request);
	auth_request_unref(&request);
	return TRUE;
}

void passdb_blocking_verify_plain(struct auth_request *request)
{
	struct auth_stream_reply *reply;

	i_assert(auth_stream_is_empty(request->extra_fields) ||
		 request->master_user != NULL ||
		 request->submit_user != NULL);		/* APPLE - urlauth */

	reply = auth_stream_reply_init(pool_datastack_create());
	auth_stream_reply_add(reply, "PASSV", NULL);
	auth_stream_reply_add(reply, NULL, dec2str(request->passdb->passdb->id));
	auth_stream_reply_add(reply, NULL, request->mech_password);
	auth_request_export(request, reply);

	auth_request_ref(request);
	auth_worker_call(request->pool, reply, verify_plain_callback, request);
}

static bool lookup_credentials_callback(const char *reply, void *context)
{
	struct auth_request *request = context;
	enum passdb_result result;
	const char *password = NULL, *scheme = NULL;

	result = auth_worker_reply_parse(request, reply);
	if (result == PASSDB_RESULT_OK && request->passdb_password != NULL) {
		password = request->passdb_password;
		scheme = password_get_scheme(&password);
		if (scheme == NULL) {
			auth_request_log_error(request, "blocking",
				"Received reply from worker without "
				"password scheme");
			result = PASSDB_RESULT_INTERNAL_FAILURE;
		}
	}

	passdb_handle_credentials(result, password, scheme,
				  auth_request_lookup_credentials_callback,
				  request);
	auth_request_unref(&request);
	return TRUE;
}

void passdb_blocking_lookup_credentials(struct auth_request *request)
{
	struct auth_stream_reply *reply;

	i_assert(auth_stream_is_empty(request->extra_fields) ||
		 request->master_user != NULL ||
		 request->submit_user != NULL);		/* APPLE - urlauth */

	reply = auth_stream_reply_init(pool_datastack_create());
	auth_stream_reply_add(reply, "PASSL", NULL);
	auth_stream_reply_add(reply, NULL, dec2str(request->passdb->passdb->id));
	auth_stream_reply_add(reply, NULL, request->credentials_scheme);
	auth_request_export(request, reply);

	auth_request_ref(request);
	auth_worker_call(request->pool, reply,
			 lookup_credentials_callback, request);
}

static bool
set_credentials_callback(const char *reply, void *context)
{
	struct auth_request *request = context;
	bool success;

	success = strcmp(reply, "OK") == 0 || strncmp(reply, "OK\t", 3) == 0;
	request->private_callback.set_credentials(success, request);
	auth_request_unref(&request);
	return TRUE;
}

void passdb_blocking_set_credentials(struct auth_request *request,
				     const char *new_credentials)
{
	struct auth_stream_reply *reply;

	reply = auth_stream_reply_init(pool_datastack_create());
	auth_stream_reply_add(reply, "SETCRED", NULL);
	auth_stream_reply_add(reply, NULL, dec2str(request->passdb->passdb->id));
	auth_stream_reply_add(reply, NULL, new_credentials);
	auth_request_export(request, reply);

	auth_request_ref(request);
	auth_worker_call(request->pool, reply,
			 set_credentials_callback, request);
}
