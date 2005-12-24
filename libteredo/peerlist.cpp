/*
 * peerlist.cpp - Teredo relay internal peers list manipulation
 * $Id$
 *
 * See "Teredo: Tunneling IPv6 over UDP through NATs"
 * for more information
 */

/***********************************************************************
 *  Copyright (C) 2004-2005 Remi Denis-Courmont.                       *
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

#include <string.h>
#include <time.h>
#include <stdlib.h> /* malloc() / free() */

#if HAVE_STDINT_H
# include <stdint.h>
#elif HAVE_INTTYPES_H
# include <inttypes.h>
#endif

#include <sys/types.h>
#include <netinet/in.h>

#include "teredo.h"
#include "teredo-udp.h"
#include "relay.h"
#include "peerlist.h"


/*
 * Big TODO:
 * - suppress the replied flag which is non-standard,
 * - check expiry time in relay code rather than peer list code,
 * - replace expiry (4 bytes) with last_rx and last_tx
 *   (both could be one byte),
 * - implement garbage collector (needed if expiry is suppressed)
 * - do not recycle peer (needs GC),
 * - remove Reset() (consequence of recycling removal)
 */

/*
 * Packets queueing
 */
typedef struct teredo_peer::packet
{
	packet *next;
	size_t length;
	bool incoming;
	uint8_t data[];
} packet;

unsigned TeredoRelay::MaxQueueBytes = 1280;

void teredo_peer::Reset (void)
{
	packet *ptr;

	/* lock peer */
	ptr = queue;
	queue = NULL;
	queue_left = TeredoRelay::MaxQueueBytes;
	/* unlock */

	while (ptr != NULL)
	{
		packet *buf;

		buf = ptr->next;
		free (ptr);
		ptr = buf;
	}
}


void teredo_peer::Queue (const void *data, size_t len, bool incoming)
{
	packet *p;

	if (len > queue_left)
		return;
	queue_left -= len;

	p = (packet *)malloc (sizeof (*p) + len);
	p->length = len;
	memcpy (p->data, data, len);
	p->incoming = incoming;

	p->next = queue;
	queue = p;
}


void teredo_peer::Dequeue (TeredoRelay *r)
{
	packet *ptr;

	/* lock peer */
	ptr = queue;
	queue = NULL;
	queue_left = TeredoRelay::MaxQueueBytes;
	/* unlock */

	while (ptr != NULL)
	{
		packet *buf;

		buf = ptr->next;
		if (ptr->incoming)
			r->SendIPv6Packet (ptr->data, ptr->length);
		else
			teredo_send (r->fd, ptr->data, ptr->length,
			             mapped_addr, mapped_port);
		free (ptr);
		ptr = buf;
	}
}


/*** Peer list handling ***/
struct teredo_peerlist
{
	teredo_peer *head;
	unsigned left;
};


/**
 * Creates an empty peer list.
 *
 * @return NULL on error.
 */
extern "C"
teredo_peerlist *teredo_list_create (unsigned max)
{
	teredo_peerlist *l = (teredo_peerlist *)malloc (sizeof (*l));
	if (l == NULL)
		return NULL;

	l->head = NULL;
	l->left = max;
	return l;
}

/**
 * Empties and destroys an existing list.
 */
extern "C"
void teredo_list_destroy (teredo_peerlist *l)
{
	teredo_peer *p = l->head;

	while (p != NULL)
	{
		teredo_peer *buf = p->next;
		delete p;
		p = buf;
	}
}

/**
 * Locks the list and looks up a peer in a list.
 * On success, the list must be unlocked with teredo_list_release(), otherwise
 * the next call to teredo_list_lookup will deadlock. Unlocking the list after
 * a failure is not defined.
 *
 * @param create if not NULL, the peer will be added to the list if it is not
 * present already, and *create will be true on return. If create is not NULL
 * but the peer was already present, *create will be false on return.
 * *create is undefined on return in case of error.
 *
 * @return The peer if found or created. NULL on error (when create is not
 * NULL), or if the peer was not found (when create is NULL).
 */
extern "C"
teredo_peer *teredo_list_lookup (teredo_peerlist *list,
                                 const struct in6_addr *addr, bool *create)
{
	/* FIXME: all this code is highly suboptimal, but it works */
	teredo_peer *p, *recycle = NULL;
	time_t now;

	time (&now);

	/* Slow O(n) simplistic peer lookup */
	for (p = list->head; p != NULL; p = p->next)
		if (t6cmp (&p->addr, (const union teredo_addr *)addr) == 0)
		{
			if (!p->IsExpired (now))
			{
				if (create != NULL)
					*create = false;
				return p;
			}
			if (recycle == NULL)
				recycle = p;
			break;
		}
		else
		if ((recycle == NULL) && (p->IsExpired (now)))
			recycle = p;

	if (create == NULL)
		return NULL;

	*create = true;

	/* Tries to recycle a timed-out peer entry */
	if (recycle != NULL)
	{
		recycle->Reset ();
	}
	else
	{
		if (list->left == 0)
			return NULL;

		/* Otherwise allocates a new peer entry */
		try
		{
			recycle = new teredo_peer;
		}
		catch (...)
		{
			return NULL;
		}

		/* Puts new entry at the head of the list */
		recycle->next = list->head;
		list->head = recycle;
		list->left--;
	}

	memcpy (&recycle->addr.ip6, addr, sizeof (struct in6_addr));
	return recycle;
}


/**
 * Unlocks a list that was locked by teredo_list_lookup().
 */
extern "C"
void teredo_list_release (teredo_peerlist *l)
{
}
