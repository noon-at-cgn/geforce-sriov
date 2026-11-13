// SPDX-License-Identifier: MIT
/*
 * Run-time timeout extension for geforce-sriov-activate.service: the sd_notify(3) wire protocol
 * for the one message the CLI sends, without libsystemd. A datagram "EXTEND_TIMEOUT_USEC=<usec>"
 * to the AF_UNIX socket named by $NOTIFY_SOCKET (a path, or "@name" for an abstract socket).
 * systemd honours it for any service state that has a timeout, Type=oneshot included, provided the
 * unit has NotifyAccess=main and the sender is the main process.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "gs_lifecycle.h"

void gs_notify_extend(unsigned int ms)
{
	const char *path = getenv("NOTIFY_SOCKET");
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	char msg[64];
	socklen_t len;
	size_t n;
	int fd, m;

	if (!path || (path[0] != '/' && path[0] != '@'))
		return;
	n = strlen(path);
	if (n < 2 || n >= sizeof(sa.sun_path))
		return;
	memcpy(sa.sun_path, path, n);
	len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n);
	if (path[0] == '@')
		sa.sun_path[0] = '\0';
	else
		len++; /* the NUL of a path name */
	m = snprintf(msg, sizeof(msg), "EXTEND_TIMEOUT_USEC=%llu",
		     (unsigned long long)(ms + GS_NOTIFY_MARGIN_MS) * 1000ull);
	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;
	/* Never blocks: a full socket queue drops this message, and the next step sends its own. A
	 * failure changes nothing: without the extension the unit's own timeout applies. */
	(void)sendto(fd, msg, (size_t)m, MSG_NOSIGNAL | MSG_DONTWAIT, (const struct sockaddr *)&sa, len);
	close(fd);
}

unsigned int gs_activate_start_timeout_s(unsigned int ncards, unsigned int nvfs)
{
	unsigned long long ms = GS_ACTIVATE_BASE_MS + (unsigned long long)ncards *
				(GS_ACTIVATE_CARD_MS + (unsigned long long)nvfs * GS_ACTIVATE_VF_MS);
	unsigned int s;

	ms += ms / 10;
	s = (unsigned int)((ms + 999) / 1000);
	return s < GS_ACTIVATE_START_MIN_S ? GS_ACTIVATE_START_MIN_S : s;
}
