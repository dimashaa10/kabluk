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

/* --- one driver: the loopback ---
 *
 * This file used to register no driver at all, on the argument that a console
 * without wireless has nothing to talk to.  That is true of the Internet and
 * false of the game: Quake's single player is a listen server plus a client
 * that connects to it, and starting a map is
 *
 *     map e1m1 -> SV_SpawnServer() -> Cmd_ExecuteString ("connect local")
 *              -> CL_EstablishConnection ("local") -> NET_Connect ("local")
 *
 * NET_Connect() walks the driver list, finds none, returns NULL, and
 * CL_EstablishConnection() answers with Host_Error("connect failed") -- after
 * the map has already been loaded and precached.  That is exactly what the
 * hardware log showed:
 *
 *     mark: model maps/PSP.bsp
 *     edicts: 3401 of 1160 bytes = 3852 KB
 *     ...
 *     connecting to local:26000
 *     syntax: connect server:port (port is optional)
 *     Host_Error: connect failed
 *
 * -- the level loads, and then the client is thrown back to the menu because
 * there is no network to connect over.  No map could ever be played.
 *
 * net_loop.c is the driver that needs no network: two qsockets in this process
 * with a memcpy between them (QuakeSpasm's, unchanged, already compiled into
 * this build).  Registering it is the whole fix: `connect local` connects, the
 * server's messages arrive, and the port plays the game the way the desktop
 * plays it.  It is also the driver the engine assumes is number 0
 * (net_defs.h: IS_LOOP_DRIVER(p) == (p == 0)), so the "don't show the loopback
 * in the Internet browser" checks keep working.
 */
#include "net_loop.h"

net_driver_t net_drivers[] =
{
	{	"Loopback",
		false,
		Loop_Init,
		Loop_Listen,
		Loop_QueryAddresses,
		Loop_SearchForHosts,
		Loop_Connect,
		Loop_CheckNewConnections,
		Loop_GetAnyMessage,
		Loop_GetMessage,
		Loop_SendMessage,
		Loop_SendUnreliableMessage,
		Loop_CanSendMessage,
		Loop_CanSendUnreliableMessage,
		Loop_Close,
		Loop_Shutdown
	}
};

const int	net_numdrivers = (int)(sizeof(net_drivers) / sizeof(net_drivers[0]));

void ResetHostlist (void)
{
}

const char *ResolveHostname (const char *hostname)
{
	/* No resolver and nothing to resolve: hand the string back unchanged, which
	 * is what the menus then display. */
	return hostname;
}
