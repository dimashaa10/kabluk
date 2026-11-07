/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

*/

/*
	Engine-provided player actions that the controls menu must always offer.

	The controls menu (Controls -> Keys) normally builds its list from
	`bindlist.lst` when the game ships one, and only falls back to the built-in
	table otherwise. An external list cannot know about actions this engine adds,
	so anything listed here is appended to the menu when the file did not mention
	it. Without that, `+use` would work from the console but could not be bound
	from the menu at all.

	Keys and their defaults live in default_cfg.h / Misc/qs_pak/default.cfg.
*/

#ifndef BINDLIST_H
#define BINDLIST_H

typedef struct
{
	const char	*cmd;	// console command the key is bound to ("+use")
	const char	*desc;	// what the menu shows for it
} enginebind_t;

static const enginebind_t	engine_required_binds[] =
{
	{ "+jump",			"Jump / swim up" },
	{ "+use",			"Use / press button" },
	{ "+skate_slide",	"Skate slide" },
	{ "+moveup",		"Swim up / fly up" },
};

#endif	/* BINDLIST_H */
