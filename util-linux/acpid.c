/* vi: set sw=4 ts=4: */
/*
 * simple ACPI events listener
 *
 * Copyright (C) 2008 by Vladimir Dronnikov <dronnikov@gmail.com>
 *
 * Licensed under GPLv2, see file LICENSE in this source tree.
 */

//usage:#define acpid_trivial_usage
//usage:       "[-d] [-c CONFDIR] [-l LOGFILE] [-a ACTIONFILE] [-M MAPFILE] [-e PROC_EVENT_FILE] [-p PIDFILE]"
//usage:#define acpid_full_usage "\n\n"
//usage:       "Listen to ACPI events and spawn specific helpers on event arrival\n"
//usage:     "\n	-c DIR	Config directory [/etc/acpi]"
//usage:     "\n	-d	Don't daemonize, (implies -f)"
//usage:     "\n	-e FILE	/proc event file [/proc/acpi/event]"
//usage:     "\n	-f	Run in foreground"
//usage:     "\n	-l FILE	Log file [/var/log/acpid.log]"
//usage:     "\n	-p FILE	Pid file [/var/run/acpid.pid]"
//usage:     "\n	-a FILE	Action file [/etc/acpid.conf]"
//usage:     "\n	-M FILE Map file [/etc/acpi.map]"
//usage:	IF_FEATURE_ACPID_COMPAT(
//usage:     "\n\nAccept and ignore compatibility options -g -m -s -S -v"
//usage:	)
//usage:
//usage:#define acpid_example_usage
//usage:       "Without -e option, acpid uses all /dev/input/event* files\n"
//usage:       "# acpid\n"
//usage:       "# acpid -l /var/log/my-acpi-log\n"
//usage:       "# acpid -e /proc/acpi/event\n"

#include "libbb.h"
#include <syslog.h>
#include <linux/input.h>

enum {
	OPT_c = (1 << 0),
	OPT_d = (1 << 1),
	OPT_e = (1 << 2),
	OPT_f = (1 << 3),
	OPT_l = (1 << 4),
	OPT_a = (1 << 5),
	OPT_M = (1 << 6),
	OPT_p = (1 << 7) * ENABLE_FEATURE_PIDFILE,
};

struct acpi_event {
	const char *s_type;
	uint16_t n_type;
	const char *s_code;
	uint16_t n_code;
	uint32_t value;
	const char *desc;
};

static const struct acpi_event f_evt_tab[] = {
	{ "EV_KEY", 0x01, "KEY_POWER", 116, 1, "button/power PWRF 00000080" },
	{ "EV_KEY", 0x01, "KEY_POWER", 116, 1, "button/power PWRB 00000080" },
};

struct acpi_action {
	const char *key;
	const char *action;
};

static const struct acpi_action f_act_tab[] = {
	{ "PWRF", "PWRF/00000080" },
	{ "LID0", "LID/00000080" },
};

struct globals {
	struct acpi_action *act_tab;
	int n_act;
	struct acpi_event *evt_tab;
	int n_evt;
} FIX_ALIASING;
#define G (*ptr_to_globals)
#define act_tab         (G.act_tab)
#define n_act           (G.n_act  )
#define evt_tab         (G.evt_tab)
#define n_evt           (G.n_evt  )
#define INIT_G() do { \
	SET_PTR_TO_GLOBALS(xzalloc(sizeof(G))); \
} while (0)

/*
 * acpid listens to ACPI events coming either in textual form
 * from /proc/acpi/event (though it is marked deprecated,
 * it is still widely used and _is_ a standard) or in binary form
 * from specified evdevs (just use /dev/input/event*).
 * It parses the event to retrieve ACTION and a possible PARAMETER.
 * It then spawns /etc/acpi/<ACTION>[/<PARAMETER>] either via run-parts
 * (if the resulting path is a directory) or directly.
 * If the resulting path does not exist it logs it via perror
 * and continues listening.
 */

static void process_event(const char *event)
{
	struct stat st;
	char *handler = xasprintf("./%s", event);
	const char *args[] = { "run-parts", handler, NULL };

	// debug info
	if (option_mask32 & OPT_d) {
		bb_error_msg("%s", event);
	}

	// spawn handler
	// N.B. run-parts would require scripts to have #!/bin/sh
	// handler is directory? -> use run-parts
	// handler is file? -> run it directly
	if (0 == stat(event, &st))
		spawn((char **)args + (0==(st.st_mode & S_IFDIR)));
	else
		bb_simple_perror_msg(event);

	free(handler);
}

static const char *find_action(struct input_event *ev, const char *buf)
{
	const char *action = NULL;
	int i;

	// map event
	for (i = 0; i < n_evt; i++) {
		if (ev) {
			if (ev->type == evt_tab[i].n_type && ev->code == evt_tab[i].n_code && ev->value == evt_tab[i].value) {
				action = evt_tab[i].desc;
				break;
			}
		}

		if (buf) {
			if (strncmp(buf, evt_tab[i].desc, strlen(buf)) == 0) {
				action = evt_tab[i].desc;
				break;
			}
		}
	}

	// get action
	if (action) {
		for (i = 0; i < n_act; i++) {
			if (strstr(action, act_tab[i].key)) {
				action = act_tab[i].action;
				break;
			}
		}
	}

	return action;
}

static void parse_conf_file(const char *filename)
{
	parser_t *parser;
	char *tokens[2];

	parser = config_open2(filename, fopen_for_read);

	if (parser) {
		while (config_read(parser, tokens, 2, 2, "# \t", PARSE_NORMAL)) {
			act_tab = xrealloc_vector(act_tab, 1, n_act);
			act_tab[n_act].key = xstrdup(tokens[0]);
			act_tab[n_act].action = xstrdup(tokens[1]);
			n_act++;
		}
		config_close(parser);
	} else {
		act_tab = (void*)f_act_tab;
		n_act = ARRAY_SIZE(f_act_tab);
	}
}

static void parse_map_file(const char *filename)
{
	parser_t *parser;
	char *tokens[6];

	parser = config_open2(filename, fopen_for_read);

	if (parser) {
		while (config_read(parser, tokens, 6, 6, "# \t", PARSE_NORMAL)) {
			evt_tab = xrealloc_vector(evt_tab, 1, n_evt);
			evt_tab[n_evt].s_type = xstrdup(tokens[0]);
			evt_tab[n_evt].n_type = xstrtou(tokens[1], 16);
			evt_tab[n_evt].s_code = xstrdup(tokens[2]);
			evt_tab[n_evt].n_code = xatou16(tokens[3]);
			evt_tab[n_evt].value = xatoi_positive(tokens[4]);
			evt_tab[n_evt].desc = xstrdup(tokens[5]);
			n_evt++;
		}
		config_close(parser);
	} else {
		evt_tab = (void*)f_evt_tab;
		n_evt = ARRAY_SIZE(f_evt_tab);
	}
}

/*
 * acpid [-c conf_dir] [-r conf_file ] [-a map_file ] [-l log_file] [-e proc_event_file]
 */

int acpid_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int acpid_main(int argc UNUSED_PARAM, char **argv)
{
	struct input_event ev;
	int nfd;
	int opts;
	struct pollfd *pfd;
	const char *opt_dir = "/etc/acpi";
	const char *opt_input = "/dev/input/event";
	const char *opt_logfile = "/var/log/acpid.log";
	const char *opt_action = "/etc/acpid.conf";
	const char *opt_map = "/etc/acpi.map";
#if ENABLE_FEATURE_PIDFILE
	const char *opt_pidfile = "/var/run/acpid.pid";
#endif

	INIT_G();

	opt_complementary = "df:e--e";
	opts = getopt32(argv, "c:de:fl:a:M:" IF_FEATURE_PIDFILE("p:") IF_FEATURE_ACPID_COMPAT("g:m:s:S:v"),
		&opt_dir, &opt_input, &opt_logfile, &opt_action, &opt_map
		IF_FEATURE_PIDFILE(, &opt_pidfile)
		IF_FEATURE_ACPID_COMPAT(, NULL, NULL, NULL, NULL)
	);

	if (!(opts & OPT_f)) {
		bb_daemonize_or_rexec(DAEMON_CLOSE_EXTRA_FDS, argv);
	}

	if (!(opts & OPT_d)) {
		openlog(applet_name, LOG_PID, LOG_DAEMON);
		logmode = LOGMODE_SYSLOG | LOGMODE_STDIO;
	} else {
		xmove_fd(xopen(opt_logfile, O_WRONLY | O_CREAT | O_TRUNC), STDOUT_FILENO);
	}

	parse_conf_file(opt_action);
	parse_map_file(opt_map);

	xchdir(opt_dir);

	bb_signals((1 << SIGCHLD), SIG_IGN);
	bb_signals(BB_FATAL_SIGS, record_signo);

	pfd = NULL;
	nfd = 0;
	while (1) {
		int fd;
		char *dev_event;

		dev_event = xasprintf((option_mask32 & OPT_e) ? "%s" : "%s%u", opt_input, nfd);
		fd = open(dev_event, O_RDONLY | O_NONBLOCK);
		if (fd < 0) {
			if (nfd == 0)
				bb_simple_perror_msg_and_die(dev_event);
			break;
		}
		pfd = xrealloc_vector(pfd, 1, nfd);
		pfd[nfd].fd = fd;
		pfd[nfd].events = POLLIN;
		nfd++;
	}

	write_pidfile(opt_pidfile);

	while (poll(pfd, nfd, -1) > 0) {
		int i;
		for (i = 0; i < nfd; i++) {
			const char *event = NULL;

			memset(&ev, 0, sizeof(ev));

			if (!(pfd[i].revents & POLLIN))
				continue;

			if (option_mask32 & OPT_e) {
				char *buf;
				int len;

				buf = xmalloc_reads(pfd[i].fd, NULL);
				/* buf = "button/power PWRB 00000080 00000000" */
				len = strlen(buf) - 9;
				if (len >= 0)
					buf[len] = '\0';
				event = find_action(NULL, buf);
			} else {
				if (sizeof(ev) != full_read(pfd[i].fd, &ev, sizeof(ev)))
					continue;

				if (ev.value != 1 && ev.value != 0)
					continue;

				event = find_action(&ev, NULL);
			}
			if (!event)
				continue;
			// spawn event handler
			process_event(event);
		}
	}

	if (ENABLE_FEATURE_CLEAN_UP) {
		while (nfd--) {
			if (pfd[nfd].fd) {
				close(pfd[nfd].fd);
			}
		}
		free(pfd);
	}
	remove_pidfile(opt_pidfile);

	return EXIT_SUCCESS;
}
