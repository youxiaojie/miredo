/*
 * conf.cpp - Configuration text file parsing definition
 * $Id$
 */

/***********************************************************************
 *  Copyright © 2004-2006 Rémi Denis-Courmont.                         *
 *  This program is free software; you can redistribute and/or modify  *
 *  it under the terms of the GNU General Public License as published  *
 *  by the Free Software Foundation; version 2 of the license.         *
 *                                                                     *
 *  This program is distributed in the hope that it will be useful,    *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of     *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.               *
 *  See the GNU General Public License for more details.               *
 *                                                                     *
 *  You should have received a copy of the GNU General Public License  *
 *  along with this program; if not, you can get it from:              *
 *  http://www.gnu.org/copyleft/gpl.html                               *
 ***********************************************************************/

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <gettext.h>
#include <assert.h>

#include <stdio.h>
#include <stdlib.h> // malloc(), free()
#include <stdarg.h>
#if HAVE_STDINT_H
# include <stdint.h>
#elif HAVE_INTTYPES_H
# include <inttypes.h>
#endif
#include <string.h>

#include <errno.h>
#include <syslog.h>

#include <sys/types.h>
#include <sys/socket.h> // AF_INET, SOCK_DGRAM
#include <netinet/in.h>
#include <netdb.h>
#include <libteredo/teredo.h>

#include "conf.h"

MiredoConf::MiredoConf (void) : head (NULL), tail (NULL)
{
}


MiredoConf::~MiredoConf (void)
{
	miredo_conf_clear (this, 0);
}


void
MiredoConf::LogError (const char *fmt, ...)
{
	va_list ap;

	va_start (ap, fmt);
	Log (true, fmt, ap);
	va_end (ap);
}


void
MiredoConf::LogWarning (const char *fmt, ...)
{
	va_list ap;

	va_start (ap, fmt);
	Log (false, fmt, ap);
	va_end (ap);
}


void
MiredoConf::Log (bool, const char *, va_list)
{
}


extern "C"
void miredo_conf_clear (miredo_conf *conf, int show)
{
	/* lock here */
	struct setting *ptr = conf->head;

	conf->head = NULL;
	/* unlock here */

	while (ptr != NULL)
	{
		struct setting *buf = ptr->next;
		if (show > 0)
		{
			conf->LogWarning (_("Superfluous directive %s at line %u"),
			                  ptr->name, ptr->line);
			show--;
		}
		free (ptr->name);
		free (ptr->value);
		free (ptr);
		ptr = buf;
	}
}


/**
 * Adds a setting.
 * @return false if memory is missing.
 */
static bool
miredo_conf_set (miredo_conf *conf, const char *name, const char *value,
                 unsigned line)
{
	assert (conf != NULL);
	assert (name != NULL);
	assert (value != NULL);

	struct setting *parm =
		(struct setting *)malloc (sizeof (struct setting));

	if (parm != NULL)
	{
		parm->name = strdup (name);
		if (parm->name != NULL)
		{
			parm->value = strdup (value);
			if (parm->value != NULL)
			{
				parm->line = line;
				parm->next = NULL;

				/* lock here */
				if (conf->head == NULL)
					conf->head = parm;
				else
				{
					assert (conf->tail != NULL);
					conf->tail->next = parm;
				}
				conf->tail = parm;
				/* unlock here */

				return true;
			}
			free (parm->name);
		}
		free (parm);
	}

	conf->LogError (_("Error (%s): %s"), "strdup", strerror (errno));
	return false;
}


/*
 * Looks up a setting by name.
 * @return NULL if not found.
 * Otherwise, return value must be free()d by caller.
 */
extern "C"
char *miredo_conf_get (miredo_conf *conf, const char *name, unsigned *line)
{
	for (struct setting *p = conf->head, *prev = NULL; p != NULL; p = p->next)
	{
		if (strcasecmp (p->name, name) == 0)
		{
			char *buf = p->value;

			if (line != NULL)
				*line = p->line;

			if (prev != NULL)
				prev->next = p->next;
			else
				conf->head = p->next;

			free (p->name);
			free (p);
			return buf;
		}
		prev = p;
	}

	return NULL;
}


static bool miredo_conf_read_FILE (miredo_conf *conf, FILE *stream)
{
	char lbuf[1056];
	unsigned line = 0;

	while (fgets (lbuf, sizeof (lbuf), stream) != NULL)
	{
		size_t len = strlen (lbuf) - 1;
		line++;

		if (lbuf[len] != '\n')
		{
			while (fgetc (stream) != '\n')
				if (feof (stream) || ferror (stream))
					break;

			conf->LogWarning (_("Skipped overly long line %u"), line);
			continue;
		}

		lbuf[len] = '\0';
		char nbuf[32], vbuf[1024];

		switch (sscanf (lbuf, " %31s %1023s", nbuf, vbuf))
		{
			case 2:
				if ((*nbuf != '#') // comment
				 && !miredo_conf_set (conf, nbuf, vbuf, line))
					return false;
				break;

			case 1:
				if (*nbuf != '#')
					conf->LogWarning (_("Ignoring line %u: %s"),
					                  line, nbuf);
				break;
		}
	}

	if (ferror (stream))
	{
		conf->LogError (_("Error reading configuration file: %s"),
		                strerror (errno));
		return false;
	}
	return true;
}


/* Parses a file.
 *
 * @return false on I/O error, true on success.
 */
extern "C"
bool miredo_conf_read_file (miredo_conf *conf, const char *path)
{
	assert (path != NULL);

	FILE *stream = fopen (path, "r");
	if (stream != NULL)
	{
		bool ret = miredo_conf_read_FILE (conf, stream);
		fclose (stream);
		return ret;
	}

	conf->LogError (_("Error opening configuration file %s: %s"), path,
	                strerror (errno));
	return false;
}


/**
 * Looks up an unsigned 16-bits integer. Returns false if the
 * setting was found but incorrectly formatted.
 *
 * If the setting was not found value, returns true and leave
 * *value unchanged.
 */
extern "C"
bool miredo_conf_get_int16 (miredo_conf *conf, const char *name,
                            uint16_t *value, unsigned *line)
{
	char *val = miredo_conf_get (conf, name, line);

	if (val == NULL)
		return true;

	char *end;
	unsigned long l;

	l = strtoul (val, &end, 0);
	
	if ((*end) || (l > 65535))
	{
		conf->LogError (_("Invalid integer value \"%s\" for %s: %s"),
		                val, name, strerror (errno));
		free (val);
		return false;
	}
	*value = (uint16_t)l;
	free (val);
	return true;
}



static const char *true_strings[] = { "yes", "true", "on", "enabled", NULL };
static const char *false_strings[] =
	{ "no", "false", "off", "disabled", NULL };

extern "C"
bool miredo_conf_get_bool (miredo_conf *conf, const char *name,
                           bool *value, unsigned *line)
{
	char *val = miredo_conf_get (conf, name, line);

	if (val == NULL)
		return true;
	else
	{
		// check if value is a number
		long l;
		char *end;

		l = strtol (val, &end, 0);

		if (*end == '\0') // success
		{
			*value = (l != 0);
			free (val);
			return true;
		}
	}

	for (const char **ptr = true_strings; *ptr != NULL; ptr++)
		if (!strcasecmp (val, *ptr))
		{
			*value = true;
			free (val);
			return true;
		}

	for (const char **ptr = false_strings; *ptr != NULL; ptr++)
		if (!strcasecmp (val, *ptr))
		{
			*value = false;
			free (val);
			return true;
		}

	conf->LogError (_("Invalid boolean value \"%s\" for %s"), val, name);
	free (val);
	return false;
}


/* Utilities function */

/**
 * Looks up an IPv4 address (network byte order) associated with hostname.
 */
extern "C"
int GetIPv4ByName (const char *hostname, uint32_t *ipv4)
{
	struct addrinfo help, *res;

	memset (&help, 0, sizeof (help));
	help.ai_family = AF_INET;
	help.ai_socktype = SOCK_DGRAM;
	help.ai_protocol = IPPROTO_UDP;

	int check = getaddrinfo (hostname, NULL, &help, &res);
	if (check)
		return check;

	*ipv4 = ((const struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
	freeaddrinfo (res);
	return 0;
}


extern "C"
bool miredo_conf_parse_IPv4 (miredo_conf *conf, const char *name,
                             uint32_t *ipv4)
{
	unsigned line;
	char *val = miredo_conf_get (conf, name, &line);

	if (val == NULL)
		return true;

	int check = GetIPv4ByName (val, ipv4);

	if (check)
	{
		conf->LogError (_("Invalid hostname \"%s\" at line %u: %s"),
		                val, line, gai_strerror (check));
		free (val);
		return false;
	}

	free (val);
	return true;
}


extern "C"
bool miredo_conf_parse_IPv6 (miredo_conf *conf, const char *name,
                             struct in6_addr *value)
{
	unsigned line;
	char *val = miredo_conf_get (conf, name, &line);

	if (val == NULL)
		return true;

	struct addrinfo help, *res;

	memset (&help, 0, sizeof (help));
	help.ai_family = AF_INET6;
	help.ai_socktype = SOCK_DGRAM;
	help.ai_protocol = IPPROTO_UDP;

	int check = getaddrinfo (val, NULL, &help, &res);

	if (check)
	{
		conf->LogError (_("Invalid hostname \"%s\" at line %u: %s"),
		                val, line, gai_strerror (check));
		free (val);
		return false;
	}

	memcpy (value, &((const struct sockaddr_in6*)(res->ai_addr))->sin6_addr,
	        sizeof (struct in6_addr));

	freeaddrinfo (res);
	free (val);
	return true;
}


extern "C"
bool miredo_conf_parse_teredo_prefix (miredo_conf *conf, const char *name,
                                      uint32_t *value)
{
	union teredo_addr addr;

	if (miredo_conf_parse_IPv6 (conf, name, &addr.ip6))
	{
		if (!is_valid_teredo_prefix (addr.teredo.prefix))
		{
			conf->LogError (_("Invalid Teredo IPv6 prefix: %x::/32"),
			                addr.teredo.prefix);
			return false;
		}

		*value = addr.teredo.prefix;
		return true;
	}
	return false;
}


static const struct miredo_conf_syslog_facility
{
	const char *str;
	int facility;
} facilities[] =
{
#ifdef LOG_AUTH
	{ "auth",	LOG_AUTH },
#endif
#ifdef LOG_AUTHPRIV
	{ "authpriv",	LOG_AUTHPRIV },
#endif
#ifdef LOG_CRON
	{ "cron",	LOG_CRON },
#endif
#ifdef LOG_DAEMON
	{ "daemon",	LOG_DAEMON },
#endif
#ifdef LOG_FTP
	{ "ftp",	LOG_FTP },
#endif
#ifdef LOG_KERN
	{ "kern",	LOG_KERN },
#endif
	{ "local0",	LOG_LOCAL0 },
	{ "local1",	LOG_LOCAL1 },
	{ "local2",	LOG_LOCAL2 },
	{ "local3",	LOG_LOCAL3 },
	{ "local4",	LOG_LOCAL4 },
	{ "local5",	LOG_LOCAL5 },
	{ "local6",	LOG_LOCAL6 },
	{ "local7",	LOG_LOCAL7 },
#ifdef LOG_LPR
	{ "lpr",	LOG_LPR },
#endif
#ifdef LOG_MAIL
	{ "mail",	LOG_MAIL },
#endif
#ifdef LOG_NEWS
	{ "news",	LOG_NEWS },
#endif
#ifdef LOG_SYSLOG
	{ "syslog",	LOG_SYSLOG },
#endif
	{ "user",	LOG_USER },
#ifdef LOG_UUCP
	{ "uucp",	LOG_UUCP },
#endif
	{ NULL,		0 }
};


extern "C"
bool miredo_conf_parse_syslog_facility (miredo_conf *conf, const char *name,
                                        int *facility)
{
	unsigned line;
	char *str = miredo_conf_get (conf, name, &line);

	if (str == NULL)
		return true;

	for (const struct miredo_conf_syslog_facility *ptr = facilities;
	     ptr->str != NULL; ptr++)
	{
		if (!strcasecmp (str, ptr->str))
		{
			*facility = ptr->facility;
			free (str);
			return true;
		}
	}

	conf->LogError (_("Unknown syslog facility \"%s\" at line %u"),
	                str, line);
	free (str);
	return false;
}
