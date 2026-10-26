/*
 * port/psp/net_psp.c -- networking for a device without wireless hardware.
 *
 * net_main.c builds and links fine on the PSP, but net_dgrm.c (the UDP
 * datagram driver) cannot: pspsdk's socket headers are IPv4-only and the
 * driver speaks IPv4 and IPv6 unconditionally.  A PSP-E100x has no wireless
 * module at all, so rather than fake a driver we register none and keep the
 * handful of globals the rest of the engine expects.
 *
 * With an empty driver list NET_Init succeeds and reports no protocols, and
 * the menu simply has nothing to browse.  A net_psp.c with a real
 * sceNetInet-based driver can take this file's place later.
 */

/* Same include order as net_main.c: net_sys.h needs q_stdinc.h for the
 * compile-time sockaddr check, and net_defs.h needs sys_socket_t. */
#include "q_stdinc.h"
#include "arch_def.h"
#include "net_sys.h"
#include "quakedef.h"
#include "net_defs.h"

/* --- server browser / master server settings (defined by net_dgrm.c on the
 * desktop build).  Kept so the cvars still exist and save/load from config. */
cvar_t	sv_reportheartbeats = {"sv_reportheartbeats", "0"};
cvar_t	sv_heartbeat_interval = {"sv_heartbeat_interval", "110"};
cvar_t	sv_public = {"sv_public", NULL};
cvar_t	com_protocolname = {"com_protocolname", KABLUK_PROTOCOL_NAME};
cvar_t	password = {"password", ""};
cvar_t	rcon_password = {"rcon_password", ""};

cvar_t	net_masters[] =
{
	{NULL}
};

/* --- no drivers --- */
net_driver_t	net_drivers[1];
const int	net_numdrivers = 0;

void ResetHostlist (void)
{
}

const char *ResolveHostname (const char *hostname)
{
	/* No resolver and nothing to resolve: hand the string back unchanged, which
	 * is what the menus then display. */
	return hostname;
}
