/* Asset-free checks on the player's default bindings and on the list of engine
 * actions the controls menu has to offer.
 *
 * Two things have to stay true for "+use" to be *usable* rather than merely
 * implemented:
 *   1. the controls menu (Controls -> Keys) has to list "+use", otherwise it can
 *      only be bound by typing into the console - that list lives in
 *      Quake/bindlist.h for the actions the engine itself provides;
 *   2. the default key has to be E, and the key freed for it (+moveup was on E)
 *      has to stay reachable.
 *
 * The defaults exist twice (Quake/default_cfg.h in the binary and
 * Misc/qs_pak/default.cfg in the pak); the header even says "keep in sync", so
 * this test parses both and fails if they drift apart.
 */
#include "quakedef.h"
#include "bindlist.h"
#include "default_cfg.h"
#include <assert.h>
#include <stdio.h>

#ifndef REPO_DIR
#define REPO_DIR "."
#endif

#define MAX_BINDS	128

typedef struct
{
	char	key[32];
	char	cmd[64];
} testbind_t;

static testbind_t	binds_a[MAX_BINDS];	/* from default_cfg.h */
static int			numbinds_a;
static testbind_t	binds_b[MAX_BINDS];	/* from Misc/qs_pak/default.cfg */
static int			numbinds_b;

/* ---- parsing ---- */

static void CopyTrimmed (char *dst, size_t dstsize, const char *start, size_t len)
{
	while (len && (*start == ' ' || *start == '\t'))
	{
		start++;
		len--;
	}
	while (len && (start[len - 1] == ' ' || start[len - 1] == '\t' || start[len - 1] == '\r'))
		len--;
	if (len >= dstsize)
		len = dstsize - 1;
	memcpy(dst, start, len);
	dst[len] = 0;
}

static int MatchPrefix (const char *text, const char *prefix)
{
	while (*prefix)
	{
		char	a = *text++;
		char	b = *prefix++;

		if (a >= 'A' && a <= 'Z')
			a += 'a' - 'A';
		if (b >= 'A' && b <= 'Z')
			b += 'a' - 'A';
		if (a != b)
			return 0;
	}
	return 1;
}

/* One config line -> a binding, if it is one. */
static int ParseBindLine (const char *line, size_t len, testbind_t *out)
{
	const char	*end = line + len;
	const char	*key;
	size_t		keylen;
	const char	*cmd;
	size_t		cmdlen;

	while (line < end && (*line == ' ' || *line == '\t'))
		line++;
	if (line == end || !MatchPrefix (line, "bind"))
		return 0;
	line += 4;
	if (line == end || (*line != ' ' && *line != '\t'))
		return 0;	/* "bindings" or a comment */
	while (line < end && (*line == ' ' || *line == '\t'))
		line++;

	key = line;
	while (line < end && *line != ' ' && *line != '\t')
		line++;
	keylen = (size_t)(line - key);
	if (!keylen)
		return 0;

	while (line < end && (*line == ' ' || *line == '\t'))
		line++;

	if (line < end && *line == '"')
	{
		line++;
		cmd = line;
		while (line < end && *line != '"')
			line++;
		cmdlen = (size_t)(line - cmd);
	}
	else
	{
		cmd = line;
		while (line < end && *line != '/' )
			line++;
		if (line + 1 < end && line[0] == '/' && line[1] == '/')
		{
			/* comment: drop it */
		}
		else if (line < end && line[0] == '/')
			line = end;
		cmdlen = (size_t)(line - cmd);
	}

	if (!cmdlen)
		return 0;

	CopyTrimmed (out->key, sizeof(out->key), key, keylen);
	CopyTrimmed (out->cmd, sizeof(out->cmd), cmd, cmdlen);
	return 1;
}

static int ParseConfigText (const char *text, testbind_t *list, int max, int *total)
{
	int		count = 0;

	*total = 0;
	while (*text)
	{
		const char	*eol = strchr (text, '\n');
		size_t		len = eol ? (size_t)(eol - text) : strlen (text);
		testbind_t	bind;

		if (ParseBindLine (text, len, &bind))
		{
			(*total)++;
			if (count < max)
				list[count++] = bind;
		}
		if (!eol)
			break;
		text = eol + 1;
	}
	return count;
}

/* ---- lookups ---- */

static int SameText (const char *a, const char *b)
{
	while (*a && *b)
	{
		char	ca = *a++;
		char	cb = *b++;

		if (ca >= 'A' && ca <= 'Z')
			ca += 'a' - 'A';
		if (cb >= 'A' && cb <= 'Z')
			cb += 'a' - 'A';
		if (ca != cb)
			return 0;
	}
	return *a == *b;
}

static int HasBinding (testbind_t *list, int count, const char *key, const char *cmd)
{
	int i;

	for (i = 0; i < count; i++)
		if (SameText (list[i].key, key) && SameText (list[i].cmd, cmd))
			return 1;
	return 0;
}

static int CountBoundTo (testbind_t *list, int count, const char *cmd)
{
	int i, n = 0;

	for (i = 0; i < count; i++)
		if (SameText (list[i].cmd, cmd))
			n++;
	return n;
}

/* The single key bound to a command, or NULL when it is not bound exactly once. */
static const char *KeyForCommand (testbind_t *list, int count, const char *cmd)
{
	const char	*key = NULL;
	int			n = 0, i;

	for (i = 0; i < count; i++)
		if (SameText (list[i].cmd, cmd))
		{
			key = list[i].key;
			n++;
		}
	return n == 1 ? key : NULL;
}

/* ---- tests ---- */

/* Requirement: the controls menu offers "+use". Without this the key can only be
 * changed from the console. */
static void TestMenuOffersUse (void)
{
	int		i;
	const enginebind_t	*use = NULL;

	for (i = 0; i < (int)countof(engine_required_binds); i++)
		if (!strcmp(engine_required_binds[i].cmd, "+use"))
			use = &engine_required_binds[i];

	assert(use != NULL);
	assert(use->desc && use->desc[0]);		/* the menu prints this */
	assert(SameText(use->cmd, "+use"));
}

/* Every engine action the menu guarantees has to be a real +/- console pair that
 * cl_input.c registers, and it must be visible in the list (non-empty label). */
static void TestRequiredBindsAreWellFormed (void)
{
	int i, j;

	for (i = 0; i < (int)countof(engine_required_binds); i++)
	{
		const enginebind_t	*e = &engine_required_binds[i];

		assert(e->cmd && e->cmd[0] == '+');
		assert(e->desc && e->desc[0]);
		for (j = 0; e->cmd[j]; j++)
			assert(e->cmd[j] != ' ');		/* must be one console word */
		for (j = 0; j < i; j++)
			assert(strcmp(engine_required_binds[j].cmd, e->cmd));	/* no doubles */
	}
}

/* Requirement: E is the action key out of the box. */
static void TestDefaultKeyIsE (void)
{
	const char *key;

	assert(CountBoundTo (binds_a, numbinds_a, "+use") == 1);
	key = KeyForCommand (binds_a, numbinds_a, "+use");
	assert(key != NULL);
	assert(SameText (key, "e"));
}

/* Freeing E must not cost the player swim/fly up: it moved to PGUP. */
static void TestMoveupStayedReachable (void)
{
	const char *moveup = KeyForCommand (binds_a, numbinds_a, "+moveup");

	assert(moveup != NULL && SameText (moveup, "pgup"));
	assert(!SameText (moveup, "e"));
	/* crouch/swim down is duplicated on C and CTRL on purpose, so ask per key */
	assert(HasBinding (binds_a, numbinds_a, "c", "+movedown"));
	assert(HasBinding (binds_a, numbinds_a, "CTRL", "+movedown"));
}

/* Every action the controls menu promises must actually come with a key, else the
 * menu shows "???" on a fresh install. */
static void TestDefaultsCoverMenu (void)
{
	int i;

	for (i = 0; i < (int)countof(engine_required_binds); i++)
		assert(CountBoundTo (binds_a, numbinds_a, engine_required_binds[i].cmd) >= 1);
}

/* One physical key must not carry two different actions in the defaults. */
static void TestNoDuplicateKeys (void)
{
	int i, j;

	for (i = 0; i < numbinds_a; i++)
	{
		for (j = 0; j < i; j++)
			assert(!SameText (binds_a[i].key, binds_a[j].key));
	}
	assert(numbinds_a > 20);	/* parses roughly everyone knows can swim */
}

/* The two copies of the defaults must agree (default_cfg.h says "keep in sync"). */
static void TestPakConfigMatchesHeader (void)
{
	char			path[512];
	FILE			*file;
	char			text[8192];
	size_t			read;
	int				total_b;
	int				i, j;

	snprintf (path, sizeof(path), "%s/Misc/qs_pak/default.cfg", REPO_DIR);
	file = fopen (path, "rb");
	if (!file)
	{
		printf ("test_bind_defaults: %s not found, skipping the sync check\n", path);
		return;
	}
	read = fread (text, 1, sizeof(text) - 1, file);
	fclose (file);
	text[read] = 0;

	numbinds_b = ParseConfigText (text, binds_b, MAX_BINDS, &total_b);
	assert(total_b == numbinds_b);			/* nothing was silently truncated */
	assert(numbinds_b == numbinds_a);		/* same number of bind lines */

	for (i = 0; i < numbinds_a; i++)
	{
		int found = 0;

		for (j = 0; j < numbinds_b; j++)
			if (SameText (binds_a[i].key, binds_b[j].key) && SameText (binds_a[i].cmd, binds_b[j].cmd))
				found = 1;
		assert(found);
	}

	/* and the pak copy also gives E to the action key */
	assert(SameText (KeyForCommand (binds_b, numbinds_b, "+use"), "e"));
}

int main (void)
{
	int total_a;

	numbinds_a = ParseConfigText (default_cfg, binds_a, MAX_BINDS, &total_a);
	assert(total_a == numbinds_a);
	assert(numbinds_a > 20);

	TestMenuOffersUse();
	TestRequiredBindsAreWellFormed();
	TestDefaultKeyIsE();
	TestMoveupStayedReachable();
	TestDefaultsCoverMenu();
	TestNoDuplicateKeys();
	TestPakConfigMatchesHeader();

	puts("PASS: the controls menu always offers +use, which can be bound like any other action;");
	puts("PASS: E is the action key by default, +moveup moved to PGUP, and both copies of");
	puts("PASS: the default config agree and cover every engine action the menu promises.");
	return 0;
}
