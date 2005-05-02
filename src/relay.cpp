/*
 * relay.cpp - Linux Teredo relay implementation
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

#if HAVE_STDINT_H
# include <stdint.h>
#elif HAVE_INTTYPES_H
# include <inttypes.h>
#endif

#include <libtun6/ipv6-tunnel.h>
#include "privproc.h"
#include "relay.h"

MiredoRelay::MiredoRelay (const IPv6Tunnel *tun, uint32_t prefix,
                          uint16_t port, uint32_t ipv4, bool cone)
	: TeredoRelay (prefix, port, ipv4, cone), tunnel (tun), priv_fd (-1)
{
}


int MiredoRelay::SendIPv6Packet (const void *packet, size_t length)
{
	return tunnel->SendPacket (packet, length);
}


#ifdef MIREDO_TEREDO_CLIENT
MiredoRelay::MiredoRelay (int fd, const IPv6Tunnel *tun, uint32_t server_ip,
                          uint32_t server_ip2, uint16_t port, uint32_t ipv4)
	: TeredoRelay (server_ip, server_ip2, port, ipv4), tunnel (tun),
	  priv_fd (fd)
{
}


int MiredoRelay::NotifyUp (const struct in6_addr *addr, uint16_t mtu)
{
	return miredo_configure_tunnel (priv_fd, addr, mtu);
}


int MiredoRelay::NotifyDown (void)
{
	return NotifyUp (&in6addr_any);
}
#endif /* ifdef MIREDO_TEREDO_CLIENT */
