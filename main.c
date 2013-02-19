/*
 * firewall3 - 3rd OpenWrt UCI firewall implementation
 *
 *   Copyright (C) 2013 Jo-Philipp Wich <jow@openwrt.org>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <unistd.h>

#include "options.h"
#include "defaults.h"
#include "zones.h"
#include "rules.h"
#include "redirects.h"
#include "forwards.h"
#include "ipsets.h"
#include "ubus.h"


static bool print_rules = false;
static enum fw3_family use_family = FW3_FAMILY_ANY;

static const char *families[] = {
	"(bug)",
	"IPv4",
	"IPv6",
};

static const char *tables[] = {
	"filter",
	"nat",
	"mangle",
	"raw",
};


static struct fw3_state *
build_state(void)
{
	struct fw3_state *state = NULL;
	struct uci_package *p = NULL;

	state = malloc(sizeof(*state));

	if (!state)
		error("Out of memory");

	memset(state, 0, sizeof(*state));
	state->uci = uci_alloc_context();

	if (!state->uci)
		error("Out of memory");

	if (uci_load(state->uci, "firewall", &p))
	{
		uci_perror(state->uci, NULL);
		error("Failed to load /etc/config/firewall");
	}

	if (!fw3_find_command("ipset"))
	{
		warn("Unable to locate ipset utility, disabling ipset support");
		state->disable_ipsets = true;
	}

	fw3_load_defaults(state, p);
	fw3_load_ipsets(state, p);
	fw3_load_zones(state, p);
	fw3_load_rules(state, p);
	fw3_load_redirects(state, p);
	fw3_load_forwards(state, p);

	return state;
}

static void
free_state(struct fw3_state *state)
{
	struct list_head *cur, *tmp;

	list_for_each_safe(cur, tmp, &state->zones)
		fw3_free_zone((struct fw3_zone *)cur);

	list_for_each_safe(cur, tmp, &state->rules)
		fw3_free_rule((struct fw3_rule *)cur);

	list_for_each_safe(cur, tmp, &state->redirects)
		fw3_free_redirect((struct fw3_redirect *)cur);

	list_for_each_safe(cur, tmp, &state->forwards)
		fw3_free_forward((struct fw3_forward *)cur);

	uci_free_context(state->uci);

	free(state);

	fw3_ubus_disconnect();
}


static bool
restore_pipe(enum fw3_family family, bool silent)
{
	const char *cmd[] = {
		"(bug)",
		"iptables-restore",
		"ip6tables-restore",
	};

	if (print_rules)
		return fw3_stdout_pipe();

	if (!fw3_command_pipe(silent, cmd[family], "--lenient", "--noflush"))
	{
		warn("Unable to execute %s", cmd[family]);
		return false;
	}

	return true;
}

#define family_flag(f) \
	(f == FW3_FAMILY_V4 ? FW3_DEFAULT_IPV4_LOADED : FW3_DEFAULT_IPV6_LOADED)

static bool
family_running(struct list_head *statefile, enum fw3_family family)
{
	struct fw3_statefile_entry *e;

	if (statefile)
	{
		list_for_each_entry(e, statefile, list)
		{
			if (e->type != FW3_TYPE_DEFAULTS)
				continue;

			return hasbit(e->flags[0], family_flag(family));
		}
	}

	return false;
}

static bool
family_used(enum fw3_family family)
{
	return (use_family == FW3_FAMILY_ANY) || (use_family == family);
}

static bool
family_loaded(struct fw3_state *state, enum fw3_family family)
{
	return hasbit(state->defaults.has_flag, family_flag(family));
}

static void
family_set(struct fw3_state *state, enum fw3_family family, bool set)
{
	if (set)
		setbit(state->defaults.has_flag, family_flag(family));
	else
		delbit(state->defaults.has_flag, family_flag(family));
}

static int
stop(struct fw3_state *state, bool complete, bool restart)
{
	int rv = 1;
	enum fw3_family family;
	enum fw3_table table;

	struct list_head *statefile = fw3_read_statefile();

	if (!complete && !statefile)
	{
		if (!restart)
			warn("The firewall appears to be stopped. "
				 "Use the 'flush' command to forcefully purge all rules.");

		return rv;
	}

	for (family = FW3_FAMILY_V4; family <= FW3_FAMILY_V6; family++)
	{
		if (!complete && !family_running(statefile, family))
			continue;

		if (!family_used(family) || !restore_pipe(family, true))
			continue;

		info("Removing %s rules ...", families[family]);

		for (table = FW3_TABLE_FILTER; table <= FW3_TABLE_RAW; table++)
		{
			if (!fw3_has_table(family == FW3_FAMILY_V6, tables[table]))
				continue;

			info(" * %sing %s table",
			     complete ? "Flush" : "Clear", tables[table]);

			fw3_pr("*%s\n", tables[table]);

			if (complete)
			{
				fw3_flush_all(table);
			}
			else
			{
				/* pass 1 */
				fw3_flush_rules(table, family, false, statefile);
				fw3_flush_zones(table, family, false, statefile);

				/* pass 2 */
				fw3_flush_rules(table, family, true, statefile);
				fw3_flush_zones(table, family, true, statefile);
			}

			fw3_pr("COMMIT\n");
		}

		fw3_command_close();

		if (!restart)
			family_set(state, family, false);

		rv = 0;
	}

	if (!restart &&
	    !family_loaded(state, FW3_FAMILY_V4) &&
	    !family_loaded(state, FW3_FAMILY_V6) &&
	    fw3_command_pipe(false, "ipset", "-exist", "-"))
	{
		fw3_destroy_ipsets(statefile);
		fw3_command_close();
	}

	fw3_free_statefile(statefile);

	if (!rv)
		fw3_write_statefile(state);

	return rv;
}

static int
start(struct fw3_state *state, bool restart)
{
	int rv = 1;
	enum fw3_family family;
	enum fw3_table table;

	struct list_head *statefile = fw3_read_statefile();

	if (!print_rules && !restart &&
	    fw3_command_pipe(false, "ipset", "-exist", "-"))
	{
		fw3_create_ipsets(state);
		fw3_command_close();
	}

	for (family = FW3_FAMILY_V4; family <= FW3_FAMILY_V6; family++)
	{
		if (!family_used(family))
			continue;

		if (!family_loaded(state, family) || !restore_pipe(family, false))
			continue;

		if (!restart && family_running(statefile, family))
		{
			warn("The %s firewall appears to be started already. "
			     "If it is indeed empty, remove the %s file and retry.",
			     families[family], FW3_STATEFILE);

			continue;
		}

		info("Constructing %s rules ...", families[family]);

		for (table = FW3_TABLE_FILTER; table <= FW3_TABLE_RAW; table++)
		{
			if (!fw3_has_table(family == FW3_FAMILY_V6, tables[table]))
				continue;

			info(" * Populating %s table", tables[table]);

			fw3_pr("*%s\n", tables[table]);
			fw3_print_default_chains(table, family, state);
			fw3_print_zone_chains(table, family, state);
			fw3_print_default_head_rules(table, family, state);
			fw3_print_rules(table, family, state);
			fw3_print_redirects(table, family, state);
			fw3_print_forwards(table, family, state);
			fw3_print_zone_rules(table, family, state);
			fw3_print_default_tail_rules(table, family, state);
			fw3_pr("COMMIT\n");
		}

		fw3_command_close();
		family_set(state, family, true);

		rv = 0;
	}

	fw3_free_statefile(statefile);

	if (!rv)
		fw3_write_statefile(state);

	return rv;
}

static int
lookup_network(struct fw3_state *state, const char *net)
{
	struct fw3_zone *z;
	struct fw3_device *d;

	list_for_each_entry(z, &state->zones, list)
	{
		list_for_each_entry(d, &z->networks, list)
		{
			if (!strcmp(d->name, net))
			{
				printf("%s\n", z->name);
				return 0;
			}
		}
	}

	return 1;
}

static int
lookup_device(struct fw3_state *state, const char *dev)
{
	struct fw3_zone *z;
	struct fw3_device *d;

	list_for_each_entry(z, &state->zones, list)
	{
		list_for_each_entry(d, &z->devices, list)
		{
			if (!strcmp(d->name, dev))
			{
				printf("%s\n", z->name);
				return 0;
			}
		}
	}

	return 1;
}

static int
usage(void)
{
	fprintf(stderr, "fw3 [-4] [-6] [-q] {start|stop|flush|restart|print}\n");
	fprintf(stderr, "fw3 [-q] network {net}\n");
	fprintf(stderr, "fw3 [-q] device {dev}\n");

	return 1;
}


int main(int argc, char **argv)
{
	int ch, rv = 1;
	struct fw3_state *state = NULL;
	struct fw3_defaults *defs = NULL;

	while ((ch = getopt(argc, argv, "46qh")) != -1)
	{
		switch (ch)
		{
		case '4':
			use_family = FW3_FAMILY_V4;
			break;

		case '6':
			use_family = FW3_FAMILY_V6;
			break;

		case 'q':
			freopen("/dev/null", "w", stderr);
			break;

		case 'h':
			rv = usage();
			goto out;
		}
	}

	if (!fw3_ubus_connect())
		error("Failed to connect to ubus");

	state = build_state();
	defs = &state->defaults;

	if (!fw3_lock())
		goto out;

	if (optind >= argc)
	{
		rv = usage();
		goto out;
	}

	if (use_family == FW3_FAMILY_V6 && defs->disable_ipv6)
		warn("IPv6 rules globally disabled in configuration");

	if (!strcmp(argv[optind], "print"))
	{
		freopen("/dev/null", "w", stderr);

		state->disable_ipsets = true;
		print_rules = true;

		rv = start(state, false);
	}
	else if (!strcmp(argv[optind], "start"))
	{
		rv = start(state, false);
	}
	else if (!strcmp(argv[optind], "stop"))
	{
		rv = stop(state, false, false);
	}
	else if (!strcmp(argv[optind], "flush"))
	{
		rv = stop(state, true, false);
	}
	else if (!strcmp(argv[optind], "restart"))
	{
		rv = stop(state, false, true);
		rv = start(state, !rv);
	}
	else if (!strcmp(argv[optind], "network") && (optind + 1) < argc)
	{
		rv = lookup_network(state, argv[optind + 1]);
	}
	else if (!strcmp(argv[optind], "device") && (optind + 1) < argc)
	{
		rv = lookup_device(state, argv[optind + 1]);
	}
	else
	{
		rv = usage();
	}

out:
	if (state)
		free_state(state);

	fw3_unlock();

	return rv;
}
