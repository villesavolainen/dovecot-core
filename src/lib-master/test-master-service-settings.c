/* Copyright (c) 2023 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "write-full.h"
#include "env-util.h"
#include "master-interface.h"
#include "master-service.h"
#include "master-service-settings.h"
#include "test-common.h"

#define DATA(data) (const unsigned char *)data"\xff", sizeof(data"\xff")-2

static const struct {
	const unsigned char *data;
	size_t size;
	const char *error;
} tests[] = {
	{ DATA("D"),
	  "File header doesn't begin with DOVECOT-CONFIG line" },
	{ DATA("DOVECOT-CONFIG\t"),
	  "File header doesn't begin with DOVECOT-CONFIG line" },
	{ DATA("DOVECOT-CONFIG\t1.0"),
	  "File header doesn't begin with DOVECOT-CONFIG line" },
	{ DATA("DOVECOT-CONFIG\t2.3\n"),
	  "Unsupported config file version '2.3'" },

	/* full file size = 1, but file is still truncated */
	{ DATA("DOVECOT-CONFIG\t1.0\n" // 19 bytes
	       "\x00\x00\x00\x00\x00\x00\x00\x01"), // full size
	  "Full size mismatch" },
	/* full file size is 7 bytes, which makes the first block size
	   truncated, since it needs 8 bytes */
	{ DATA("DOVECOT-CONFIG\t1.0\n"
	       "\x00\x00\x00\x00\x00\x00\x00\x07" // full size
	       "\x00\x00\x00\x00\x00\x00\x00"), // block size
	  "Area too small when reading size of 'block size'" },
	/* first block size is 0, which is too small */
	{ DATA("DOVECOT-CONFIG\t1.0\n"
	       "\x00\x00\x00\x00\x00\x00\x00\x08" // full size
	       "\x00\x00\x00\x00\x00\x00\x00\x00"), // block size
	  "'block name' points outside area" },
	/* first block size is 1, but full file size is too small */
	{ DATA("DOVECOT-CONFIG\t1.0\n"
	       "\x00\x00\x00\x00\x00\x00\x00\x08" // full size
	       "\x00\x00\x00\x00\x00\x00\x00\x01"), // block size
	  "'block size' points outside are" },
	/* block name is not NUL-terminated */
	{ DATA("DOVECOT-CONFIG\t1.0\n"
	       "\x00\x00\x00\x00\x00\x00\x00\x0A" // full size
	       "\x00\x00\x00\x00\x00\x00\x00\x01" // block size
	       "N"
	       "\x00"), // trailing garbage so we can have NUL
	  "Settings block doesn't end with NUL at offset" },

	/* settings block size is truncated */
	{ DATA("DOVECOT-CONFIG\t1.0\n"
	       "\x00\x00\x00\x00\x00\x00\x00\x11" // full size
	       "\x00\x00\x00\x00\x00\x00\x00\x09" // block size
	       "N\x00" // block name
	       "\x00\x00\x00\x00\x00\x00\x00"),
	  "Area too small when reading size of 'base settings size'" },
	/* base settings size is zero */
	{ DATA("DOVECOT-CONFIG\t1.0\n"
	       "\x00\x00\x00\x00\x00\x00\x00\x12" // full size
	       "\x00\x00\x00\x00\x00\x00\x00\x0A" // block size
	       "N\x00" // block name
	       "\x00\x00\x00\x00\x00\x00\x00\x00"), // base settings size
	  "'base settings error' points outside area" },
	/* base settings error is not NUL-terminated */
	{ DATA("DOVECOT-CONFIG\t1.0\n"
	       "\x00\x00\x00\x00\x00\x00\x00\x14" // full size
	       "\x00\x00\x00\x00\x00\x00\x00\x0C" // block size
	       "N\x00" // block name
	       "\x00\x00\x00\x00\x00\x00\x00\x01" // base settings size
	       "E" // base settings error
	       "\x00"), // trailing garbage so we can have NUL
	  "'base settings error' points outside area" },

	/* filter settings size is truncated */
	{ DATA("DOVECOT-CONFIG\t1.0\n"
	       "\x00\x00\x00\x00\x00\x00\x00\x1A" // full size
	       "\x00\x00\x00\x00\x00\x00\x00\x12" // block size
	       "N\x00" // block name
	       "\x00\x00\x00\x00\x00\x00\x00\x01" // base settings size
	       "\x00" // base settings error
	       "\x00\x00\x00\x00\x00\x00\x00"), // filter settings size
	  "Area too small when reading size of 'filter settings size'" },
	/* filter settings size is zero */
	{ DATA("DOVECOT-CONFIG\t1.0\n"
	       "\x00\x00\x00\x00\x00\x00\x00\x1B" // full size
	       "\x00\x00\x00\x00\x00\x00\x00\x13" // block size
	       "N\x00" // block name
	       "\x00\x00\x00\x00\x00\x00\x00\x01" // base settings size
	       "\x00" // base settings error
	       "\x00\x00\x00\x00\x00\x00\x00\x00"), // filter settings size
	  "'filter string' points outside area" },
	/* filter string is not NUL-terminated */
	{ DATA("DOVECOT-CONFIG\t1.0\n"
	       "\x00\x00\x00\x00\x00\x00\x00\x1D" // full size
	       "\x00\x00\x00\x00\x00\x00\x00\x15" // block size
	       "N\x00" // block name
	       "\x00\x00\x00\x00\x00\x00\x00\x01" // base settings size
	       "\x00" // base settings error
	       "\x00\x00\x00\x00\x00\x00\x00\x01" // filter settings size
	       "F" // filter string
	       "\x00"), // trailing garbage so we can have NUL
	  "'filter string' points outside area" },
	/* filter error is missing */
	{ DATA("DOVECOT-CONFIG\t1.0\n"
	       "\x00\x00\x00\x00\x00\x00\x00\x1D" // full size
	       "\x00\x00\x00\x00\x00\x00\x00\x15" // block size
	       "N\x00" // block name
	       "\x00\x00\x00\x00\x00\x00\x00\x01" // base settings size
	       "\x00" // base settings error
	       "\x00\x00\x00\x00\x00\x00\x00\x02" // filter settings size
	       "F\x00"), // filter string
	  "'filter settings error' points outside area" },
	/* filter error is not NUL-terminated */
	{ DATA("DOVECOT-CONFIG\t1.0\n"
	       "\x00\x00\x00\x00\x00\x00\x00\x1F" // full size
	       "\x00\x00\x00\x00\x00\x00\x00\x17" // block size
	       "N\x00" // block name
	       "\x00\x00\x00\x00\x00\x00\x00\x01" // base settings size
	       "\x00" // base settings error
	       "\x00\x00\x00\x00\x00\x00\x00\x03" // filter settings size
	       "F\x00" // filter string
	       "E" // filter error
	       "\x00"), // trailing garbage so we can have NUL
	  "'filter settings error' points outside area" },
	/* invalid filter string */
	{ DATA("DOVECOT-CONFIG\t1.0\n"
	       "\x00\x00\x00\x00\x00\x00\x00\x1E" // full size
	       "\x00\x00\x00\x00\x00\x00\x00\x16" // block size
	       "N\x00" // block name
	       "\x00\x00\x00\x00\x00\x00\x00\x01" // base settings size
	       "\x00" // base settings error
	       "\x00\x00\x00\x00\x00\x00\x00\x03" // filter settings size
	       "F\x00" // filter string
	       "\x00"), // filter error
	  "Received invalid filter 'F': event filter: syntax error" },

	/* Duplicate block name */
	{ DATA("DOVECOT-CONFIG\t1.0\n"
	       "\x00\x00\x00\x00\x00\x00\x00\x27" // full size
	       "\x00\x00\x00\x00\x00\x00\x00\x15" // block size
	       "N\x00" // block name
	       "\x00\x00\x00\x00\x00\x00\x00\x01" // base settings size
	       "\x00" // base settings error
	       "\x00\x00\x00\x00\x00\x00\x00\x02" // filter settings size
	       "\x00" // filter string
	       "\x00" // filter error
	       "\x00\x00\x00\x00\x00\x00\x00\x02" // 2nd block size
	       "N\x00"), // 2nd block name
	  "Duplicate block name 'N'" },
};

static int test_input_to_fd(const unsigned char *data, size_t size)
{
	int fd = test_create_temp_fd();
	if (write_full(fd, data, size) < 0)
		i_fatal("write(temp file) failed: %m");
	if (lseek(fd, 0, SEEK_SET) < 0)
		i_fatal("lseek(temp file) failed: %m");
	return fd;
}

static void test_master_service_settings_read_binary_corruption(void)
{
	const char *error;

	test_begin("master_service_settings_read() - binary corruption");
	for (unsigned int i = 0; i < N_ELEMENTS(tests); i++) {
		struct master_service_settings_input input = {
			.config_fd = test_input_to_fd(tests[i].data, tests[i].size),
		};
		struct master_service_settings_output output;

		test_assert_idx(master_service_settings_read(master_service,
			&input, &output, &error) == -1, i);
		test_assert_idx(strstr(error, tests[i].error) != NULL, i);
		if (strstr(error, tests[i].error) == NULL)
			i_error("%s", error);
	}
	test_end();
}

int main(int argc, char *argv[])
{
	static void (*const test_functions[])(void) = {
		test_master_service_settings_read_binary_corruption,
		NULL
	};
	const enum master_service_flags service_flags =
		MASTER_SERVICE_FLAG_STANDALONE |
		MASTER_SERVICE_FLAG_DONT_SEND_STATS |
		MASTER_SERVICE_FLAG_NO_SSL_INIT;
	master_service = master_service_init("test-master-service-settings",
					     service_flags, &argc, &argv, "");
	int ret = test_run(test_functions);
	master_service_deinit(&master_service);
	return ret;
}
