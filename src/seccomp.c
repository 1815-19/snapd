/*
 * Copyright (C) 2015 Canonical Ltd
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <search.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>

// needed for search mappings
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <seccomp.h>

#include "seccomp.h"
#include "utils.h"

// libseccomp maximum per ARG_COUNT_MAX in src/arch.h
#define SC_ARGS_MAXLENGTH	6
#define SC_MAX_LINE_LENGTH	82	// 80 + '\n' + '\0'

char *filter_profile_dir = "/var/lib/snapd/seccomp/profiles/";
struct hsearch_data sc_map_htab;

enum parse_ret {
	PARSE_INVALID_SYSCALL = -2,
	PARSE_ERROR = -1,
	PARSE_OK = 0,
};

struct preprocess {
	bool unrestricted;
	bool complain;
};

/*
 * arg_cmp contains items of type scmp_arg_cmp (from SCMP_CMP macro) and
 * length is the number of items in arg_cmp that are active such that if
 * length is '3' arg_cmp[0], arg_cmp[1] and arg_cmp[2] are used, when length
 * is '1' only arg_cmp[0] and when length is '0', none are used.
 */
struct seccomp_args {
	int syscall_nr;
	unsigned int length;
	struct scmp_arg_cmp arg_cmp[SC_ARGS_MAXLENGTH];
};

/*
 * Setup an hsearch map to map strings in the policy (eg, AF_UNIX) to
 * scmp_datum_t values. Abstract away hsearch implementation behind sc_map_*
 * functions in case we want to swap this out.
 *
 * sc_map_init()		- initialize the hash map
 * sc_map_add(key, value)	- add key/value pair to the map. Value is scmp_datum_t
 * sc_map_search(s)	- if found, return scmp_datum_t for key, else set errno
 * sc_map_destroy()	- destroy the hash map
 */
scmp_datum_t sc_map_search(char *s)
{
	ENTRY e;
	ENTRY *ep = NULL;
	scmp_datum_t val = 0;
	errno = 0;

	e.key = s;
	if (hsearch_r(e, FIND, &ep, &sc_map_htab) == 0)
		die("hsearch_r failed");

	if (ep != NULL)
		val = (scmp_datum_t) (ep->data);
	else
		errno = EINVAL;

	return val;
}

void sc_map_add(char *key, void *data)
{
	ENTRY e;
	ENTRY *ep = NULL;
	errno = 0;

	e.key = key;
	e.data = data;
	if (hsearch_r(e, ENTER, &ep, &sc_map_htab) == 0)
		die("hsearch_r failed");

	if (ep == NULL)
		die("could not initialize map");
}

void sc_map_init()
{
	// first initialize the htab for our map
	memset((void *)&sc_map_htab, 0, sizeof(sc_map_htab));

	const int sc_map_length = 82;	// one for each sc_map_add
	if (hcreate_r(sc_map_length, &sc_map_htab) == 0)
		die("could not create map");

	// man 2 socket - domain
	sc_map_add("AF_UNIX", (void *)AF_UNIX);
	sc_map_add("AF_LOCAL", (void *)AF_LOCAL);
	sc_map_add("AF_INET", (void *)AF_INET);
	sc_map_add("AF_INET6", (void *)AF_INET6);
	sc_map_add("AF_IPX", (void *)AF_IPX);
	sc_map_add("AF_NETLINK", (void *)AF_NETLINK);
	sc_map_add("AF_X25", (void *)AF_X25);
	sc_map_add("AF_AX25", (void *)AF_AX25);
	sc_map_add("AF_ATMPVC", (void *)AF_ATMPVC);
	sc_map_add("AF_APPLETALK", (void *)AF_APPLETALK);
	sc_map_add("AF_PACKET", (void *)AF_PACKET);
	sc_map_add("AF_ALG", (void *)AF_ALG);

	// man 2 socket - type
	sc_map_add("SOCK_STREAM", (void *)SOCK_STREAM);
	sc_map_add("SOCK_DGRAM", (void *)SOCK_DGRAM);
	sc_map_add("SOCK_SEQPACKET", (void *)SOCK_SEQPACKET);
	sc_map_add("SOCK_RAW", (void *)SOCK_RAW);
	sc_map_add("SOCK_RDM", (void *)SOCK_RDM);
	sc_map_add("SOCK_PACKET", (void *)SOCK_PACKET);

	// man 2 prctl
	sc_map_add("PR_CAP_AMBIENT", (void *)PR_CAP_AMBIENT);
	sc_map_add("PR_CAP_AMBIENT_RAISE", (void *)PR_CAP_AMBIENT_RAISE);
	sc_map_add("PR_CAP_AMBIENT_LOWER", (void *)PR_CAP_AMBIENT_LOWER);
	sc_map_add("PR_CAP_AMBIENT_IS_SET", (void *)PR_CAP_AMBIENT_IS_SET);
	sc_map_add("PR_CAP_AMBIENT_CLEAR_ALL",
		   (void *)PR_CAP_AMBIENT_CLEAR_ALL);
	sc_map_add("PR_CAPBSET_READ", (void *)PR_CAPBSET_READ);
	sc_map_add("PR_CAPBSET_DROP", (void *)PR_CAPBSET_DROP);
	sc_map_add("PR_SET_CHILD_SUBREAPER", (void *)PR_SET_CHILD_SUBREAPER);
	sc_map_add("PR_GET_CHILD_SUBREAPER", (void *)PR_GET_CHILD_SUBREAPER);
	sc_map_add("PR_SET_DUMPABLE", (void *)PR_SET_DUMPABLE);
	sc_map_add("PR_GET_DUMPABLE", (void *)PR_GET_DUMPABLE);
	sc_map_add("PR_SET_ENDIAN", (void *)PR_SET_ENDIAN);
	sc_map_add("PR_GET_ENDIAN", (void *)PR_GET_ENDIAN);
	sc_map_add("PR_SET_FPEMU", (void *)PR_SET_FPEMU);
	sc_map_add("PR_GET_FPEMU", (void *)PR_GET_FPEMU);
	sc_map_add("PR_SET_FPEXC", (void *)PR_SET_FPEXC);
	sc_map_add("PR_GET_FPEXC", (void *)PR_GET_FPEXC);
	sc_map_add("PR_SET_KEEPCAPS", (void *)PR_SET_KEEPCAPS);
	sc_map_add("PR_GET_KEEPCAPS", (void *)PR_GET_KEEPCAPS);
	sc_map_add("PR_MCE_KILL", (void *)PR_MCE_KILL);
	sc_map_add("PR_MCE_KILL_GET", (void *)PR_MCE_KILL_GET);
	sc_map_add("PR_SET_MM", (void *)PR_SET_MM);
	sc_map_add("PR_SET_MM_START_CODE", (void *)PR_SET_MM_START_CODE);
	sc_map_add("PR_SET_MM_END_CODE", (void *)PR_SET_MM_END_CODE);
	sc_map_add("PR_SET_MM_START_DATA", (void *)PR_SET_MM_START_DATA);
	sc_map_add("PR_SET_MM_END_DATA", (void *)PR_SET_MM_END_DATA);
	sc_map_add("PR_SET_MM_START_STACK", (void *)PR_SET_MM_START_STACK);
	sc_map_add("PR_SET_MM_START_BRK", (void *)PR_SET_MM_START_BRK);
	sc_map_add("PR_SET_MM_BRK", (void *)PR_SET_MM_BRK);
	sc_map_add("PR_SET_MM_ARG_START", (void *)PR_SET_MM_ARG_START);
	sc_map_add("PR_SET_MM_ARG_END", (void *)PR_SET_MM_ARG_END);
	sc_map_add("PR_SET_MM_ENV_START", (void *)PR_SET_MM_ENV_START);
	sc_map_add("PR_SET_MM_ENV_END", (void *)PR_SET_MM_ENV_END);
	sc_map_add("PR_SET_MM_AUXV", (void *)PR_SET_MM_AUXV);
	sc_map_add("PR_SET_MM_EXE_FILE", (void *)PR_SET_MM_EXE_FILE);
	sc_map_add("PR_MPX_ENABLE_MANAGEMENT",
		   (void *)PR_MPX_ENABLE_MANAGEMENT);
	sc_map_add("PR_MPX_DISABLE_MANAGEMENT",
		   (void *)PR_MPX_DISABLE_MANAGEMENT);
	sc_map_add("PR_SET_NAME", (void *)PR_SET_NAME);
	sc_map_add("PR_GET_NAME", (void *)PR_GET_NAME);
	sc_map_add("PR_SET_NO_NEW_PRIVS", (void *)PR_SET_NO_NEW_PRIVS);
	sc_map_add("PR_GET_NO_NEW_PRIVS", (void *)PR_GET_NO_NEW_PRIVS);
	sc_map_add("PR_SET_PDEATHSIG", (void *)PR_SET_PDEATHSIG);
	sc_map_add("PR_GET_PDEATHSIG", (void *)PR_GET_PDEATHSIG);
	sc_map_add("PR_SET_PTRACER", (void *)PR_SET_PTRACER);
	sc_map_add("PR_SET_SECCOMP", (void *)PR_SET_SECCOMP);
	sc_map_add("PR_GET_SECCOMP", (void *)PR_GET_SECCOMP);
	sc_map_add("PR_SET_SECUREBITS", (void *)PR_SET_SECUREBITS);
	sc_map_add("PR_GET_SECUREBITS", (void *)PR_GET_SECUREBITS);
	sc_map_add("PR_SET_THP_DISABLE", (void *)PR_SET_THP_DISABLE);
	sc_map_add("PR_TASK_PERF_EVENTS_DISABLE",
		   (void *)PR_TASK_PERF_EVENTS_DISABLE);
	sc_map_add("PR_TASK_PERF_EVENTS_ENABLE",
		   (void *)PR_TASK_PERF_EVENTS_ENABLE);
	sc_map_add("PR_GET_THP_DISABLE", (void *)PR_GET_THP_DISABLE);
	sc_map_add("PR_GET_TID_ADDRESS", (void *)PR_GET_TID_ADDRESS);
	sc_map_add("PR_SET_TIMERSLACK", (void *)PR_SET_TIMERSLACK);
	sc_map_add("PR_GET_TIMERSLACK", (void *)PR_GET_TIMERSLACK);
	sc_map_add("PR_SET_TIMING", (void *)PR_SET_TIMING);
	sc_map_add("PR_GET_TIMING", (void *)PR_GET_TIMING);
	sc_map_add("PR_SET_TSC", (void *)PR_SET_TSC);
	sc_map_add("PR_GET_TSC", (void *)PR_GET_TSC);
	sc_map_add("PR_SET_UNALIGN", (void *)PR_SET_UNALIGN);
	sc_map_add("PR_GET_UNALIGN", (void *)PR_GET_UNALIGN);

	// man 2 getpriority
	sc_map_add("PRIO_PROCESS", (void *)PRIO_PROCESS);
	sc_map_add("PRIO_PGRP", (void *)PRIO_PGRP);
	sc_map_add("PRIO_USER", (void *)PRIO_USER);
}

void sc_map_destroy()
{
	hdestroy_r(&sc_map_htab);
}

/* Caller must check if errno != 0 */
scmp_datum_t read_number(char *s)
{
	scmp_datum_t val = 0;

	errno = 0;

	// per seccomp.h definition of scmp_datum_t, negative numbers are not
	// supported, so fail if we see one or if we get one. Also fail if
	// string is 0 length.
	if (s[0] == '-' || s[0] == '\0') {
		errno = EINVAL;
		return val;
	}
	// check if number
	for (int i = 0; i < strlen(s); i++) {
		if (isdigit(s[i]) == 0) {
			errno = EINVAL;
			break;
		}
	}
	if (errno == 0) {	// found a number, so parse it
		char *end;
		// strtol may set errno to ERANGE
		val = strtoul(s, &end, 10);
		if (end == s || *end != '\0')
			errno = EINVAL;
	} else			// try our map (sc_map_search sets errno)
		val = sc_map_search(s);

	return val;
}

int parse_line(char *line, struct seccomp_args *sargs)
{
	// strtok_r needs a pointer to keep track of where it is in the
	// string.
	char *buf_saveptr;

	// Initialize our struct
	sargs->length = 0;
	sargs->syscall_nr = -1;

	if (strlen(line) == 0)
		return PARSE_ERROR;

	// Initialize tokenizer and obtain first token.
	char *buf_token = strtok_r(line, " \t", &buf_saveptr);
	if (buf_token == NULL)
		return PARSE_ERROR;

	// syscall not available on this arch/kernel
	sargs->syscall_nr = seccomp_syscall_resolve_name(buf_token);
	if (sargs->syscall_nr == __NR_SCMP_ERROR)
		return PARSE_INVALID_SYSCALL;

	// Parse for syscall arguments. Since we haven't yet searched for the
	// next token, buf_token is still the syscall itself so start 'pos' as
	// -1 and only if there is an arg to parse, increment it.
	int pos = -1;
	while (pos < SC_ARGS_MAXLENGTH) {
		buf_token = strtok_r(NULL, " \t", &buf_saveptr);
		if (buf_token == NULL)
			break;
		// we found a token, so increment position and process it
		pos++;
		if (strcmp(buf_token, "-") == 0)	// skip arg
			continue;

		enum scmp_compare op = -1;
		scmp_datum_t value = 0;
		if (strlen(buf_token) == 0) {
			return PARSE_ERROR;
		} else if (strlen(buf_token) == 1) {
			// syscall N (length of '1' indicates a single digit)
			op = SCMP_CMP_EQ;
			value = read_number(buf_token);
		} else if (strncmp(buf_token, ">=", 2) == 0) {
			// syscall >=N
			op = SCMP_CMP_GE;
			value = read_number(&buf_token[2]);
		} else if (strncmp(buf_token, "<=", 2) == 0) {
			// syscall <=N
			op = SCMP_CMP_LE;
			value = read_number(&buf_token[2]);
		} else if (strncmp(buf_token, "!", 1) == 0) {
			// syscall !N
			op = SCMP_CMP_NE;
			value = read_number(&buf_token[1]);
		} else if (strncmp(buf_token, ">", 1) == 0) {
			// syscall >N
			op = SCMP_CMP_GT;
			value = read_number(&buf_token[1]);
		} else if (strncmp(buf_token, "<", 1) == 0) {
			// syscall <N
			op = SCMP_CMP_LT;
			value = read_number(&buf_token[1]);
		} else {
			// syscall NNN
			op = SCMP_CMP_EQ;
			value = read_number(buf_token);
		}
		if (errno != 0)
			return PARSE_ERROR;

		sargs->arg_cmp[sargs->length] = SCMP_CMP(pos, op, value);
		sargs->length++;
	}
	// too many args
	if (pos >= SC_ARGS_MAXLENGTH)
		return PARSE_ERROR;

	return PARSE_OK;
}

// strip whitespace from the end of the given string (inplace)
size_t trim_right(char *s, size_t slen)
{
	while (slen > 0 && isspace(s[slen - 1])) {
		s[--slen] = 0;
	}
	return slen;
}

// Read a relevant line and return the length. Return length '0' for comments,
// empty lines and lines with only whitespace (so a caller can easily skip
// them). The line buffer is right whitespaced trimmed and the final length of
// the trimmed line is returned.
size_t validate_and_trim_line(char *buf, size_t buf_len, size_t lineno)
{
	size_t len = 0;

	// comment, ignore
	if (buf[0] == '#')
		return len;

	// ensure the entire line was read
	len = strlen(buf);
	if (len == 0)
		return len;
	else if (buf[len - 1] != '\n' && len > (buf_len - 2)) {
		fprintf(stderr,
			"seccomp filter line %zu was too long (%zu characters max)\n",
			lineno, buf_len - 2);
		errno = 0;
		die("aborting");
	}
	// kill final newline
	len = trim_right(buf, len);

	return len;
}

void preprocess_filter(FILE * f, struct preprocess *p)
{
	char buf[SC_MAX_LINE_LENGTH];
	size_t lineno = 0;

	p->unrestricted = false;
	p->complain = false;

	while (fgets(buf, sizeof(buf), f) != NULL) {
		lineno++;

		// skip policy-irrelevant lines
		if (validate_and_trim_line(buf, sizeof(buf), lineno) == 0)
			continue;

		// check for special "@unrestricted" rule which short-circuits
		// seccomp sandbox
		if (strcmp(buf, "@unrestricted") == 0)
			p->unrestricted = true;

		// check for special "@complain" rule
		if (strcmp(buf, "@complain") == 0)
			p->complain = true;
	}

	if (fseek(f, 0L, SEEK_SET) != 0)
		die("could not rewind file");

	return;
}

void seccomp_load_filters(const char *filter_profile)
{
	debug("seccomp_load_filters %s", filter_profile);
	int rc = 0;
	scmp_filter_ctx ctx = NULL;
	FILE *f = NULL;
	size_t lineno = 0;
	uid_t real_uid, effective_uid, saved_uid;
	struct preprocess pre;
	struct seccomp_args sargs;

	// initialize hsearch map
	sc_map_init();

	ctx = seccomp_init(SCMP_ACT_KILL);
	if (ctx == NULL) {
		errno = ENOMEM;
		die("seccomp_init() failed");
	}
	// Disable NO_NEW_PRIVS because it interferes with exec transitions in
	// AppArmor. Unfortunately this means that security policies must be
	// very careful to not allow the following otherwise apps can escape
	// the sandbox:
	//   - seccomp syscall
	//   - prctl with PR_SET_SECCOMP
	//   - ptrace (trace) in AppArmor
	//   - capability sys_admin in AppArmor
	// Note that with NO_NEW_PRIVS disabled, CAP_SYS_ADMIN is required to
	// change the seccomp sandbox.

	if (getresuid(&real_uid, &effective_uid, &saved_uid) != 0)
		die("could not find user IDs");

	// If running privileged or capable of raising, disable nnp
	if (real_uid == 0 || effective_uid == 0 || saved_uid == 0)
		if (seccomp_attr_set(ctx, SCMP_FLTATR_CTL_NNP, 0) != 0)
			die("Cannot disable nnp");

	// Note that secure_gettenv will always return NULL when suid, so
	// SNAPPY_LAUNCHER_SECCOMP_PROFILE_DIR can't be (ab)used in that case.
	if (secure_getenv("SNAPPY_LAUNCHER_SECCOMP_PROFILE_DIR") != NULL)
		filter_profile_dir =
		    secure_getenv("SNAPPY_LAUNCHER_SECCOMP_PROFILE_DIR");

	char profile_path[512];	// arbitrary path name limit
	int snprintf_rc = snprintf(profile_path, sizeof(profile_path), "%s/%s",
				   filter_profile_dir, filter_profile);
	if (snprintf_rc < 0 || snprintf_rc >= 512) {
		errno = 0;
		die("snprintf returned unexpected value");
	}

	f = fopen(profile_path, "r");
	if (f == NULL) {
		fprintf(stderr, "Can not open %s (%s)\n", profile_path,
			strerror(errno));
		die("aborting");
	}
	// Note, preprocess_filter() die()s on error
	preprocess_filter(f, &pre);

	if (pre.unrestricted)
		goto out;

	// FIXME: right now complain mode is the equivalent to unrestricted.
	// We'll want to change this once we seccomp logging is in order.
	if (pre.complain)
		goto out;

	char buf[SC_MAX_LINE_LENGTH];
	while (fgets(buf, sizeof(buf), f) != NULL) {
		lineno++;

		// skip policy-irrelevant lines
		if (validate_and_trim_line(buf, sizeof(buf), lineno) == 0)
			continue;

		char *buf_copy = strdup(buf);
		if (buf_copy == NULL)
			die("Out of memory");

		int pr_rc = parse_line(buf_copy, &sargs);
		free(buf_copy);
		if (pr_rc != PARSE_OK) {
			// as this is a syscall whitelist an invalid syscall
			// is ok and the error can be ignored
			if (pr_rc == PARSE_INVALID_SYSCALL)
				continue;
			die("could not parse line");
		}

		rc = seccomp_rule_add_exact_array(ctx, SCMP_ACT_ALLOW,
						  sargs.syscall_nr,
						  sargs.length, sargs.arg_cmp);
		if (rc != 0) {
			rc = seccomp_rule_add_array(ctx, SCMP_ACT_ALLOW,
						    sargs.syscall_nr,
						    sargs.length,
						    sargs.arg_cmp);
			if (rc != 0) {
				fprintf(stderr,
					"seccomp_rule_add_array failed with %i for '%s'\n",
					rc, buf);
				errno = 0;
				die("aborting");
			}
		}
	}

	// If not root but can raise, then raise privileges to load seccomp
	// policy since we don't have nnp
	if (effective_uid != 0 && saved_uid == 0) {
		if (seteuid(0) != 0)
			die("seteuid failed");
		if (geteuid() != 0)
			die("raising privs before seccomp_load did not work");
	}
	// load it into the kernel
	rc = seccomp_load(ctx);

	if (rc != 0) {
		fprintf(stderr, "seccomp_load failed with %i\n", rc);
		die("aborting");
	}
	// drop privileges again
	if (geteuid() == 0) {
		unsigned real_uid = getuid();
		if (seteuid(real_uid) != 0)
			die("seteuid failed");
		if (real_uid != 0 && geteuid() == 0)
			die("dropping privs after seccomp_load did not work");
	}

 out:
	if (f != NULL) {
		if (fclose(f) != 0)
			die("could not close seccomp file");
	}
	seccomp_release(ctx);

	sc_map_destroy();
	return;
}
