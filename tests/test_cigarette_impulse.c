/*
 * Regression tests for the real SV_ReadClientMove and cigarette think hooks.
 * The network/model/QuakeC dependencies are test doubles; the QC double keeps
 * the original impulse-9 cheat and can also read the prediction input globals.
 * No game assets, SDL libraries or QuakeC function names are needed.
 */
#include "quakedef.h"

#include <assert.h>
#include <ctype.h>

extern void SV_ReadClientMove (usercmd_t *move);
extern cvar_t sv_nqplayerphysics;

server_t sv;
client_t *host_client;
qcvm_t *qcvm;
globalvars_t *pr_global_struct;
cvar_t deathmatch = {"deathmatch", "0", CVAR_NONE};

static client_t client;
static edict_t edicts[2];
static qmodel_t cigarette_model;
static union {
	globalvars_t vars;
	float words[256];
} globals;
static float input_impulse, input_buttons;
static qboolean have_model, refresh_currentammo;
static int cheat_calls, shots, qc_calls;
static int wire_sequence;
static byte packet[64];
static size_t packet_size, packet_read;

#define CIGARETTE_MODEL 1
#define SHOTGUN_MODEL 2
#define ROCKET_MODEL 3
#define PRETHINK 1
#define POSTTHINK 2
#define RUNCOMMAND 3

/* Minimal network reader/writer for genuine clc_move field ordering. */
static void PutByte (int value)
{
	assert(packet_size < sizeof(packet));
	packet[packet_size++] = value;
}

static void PutShort (int value)
{
	PutByte(value);
	PutByte(value >> 8);
}

static void PutFloat (float value)
{
	unsigned int bits;
	memcpy(&bits, &value, sizeof(bits));
	PutByte(bits);
	PutByte(bits >> 8);
	PutByte(bits >> 16);
	PutByte(bits >> 24);
}

int MSG_ReadByte (void)
{
	assert(packet_read < packet_size);
	return packet[packet_read++];
}

int MSG_ReadShort (void)
{
	int low = MSG_ReadByte();
	return (short)(low | (MSG_ReadByte() << 8));
}

int MSG_ReadLong (void)
{
	unsigned int bits = (unsigned short)MSG_ReadShort();
	bits |= (unsigned int)(unsigned short)MSG_ReadShort() << 16;
	return (int)bits;
}

float MSG_ReadFloat (void)
{
	unsigned int bits = (unsigned int)MSG_ReadLong();
	float value;
	memcpy(&value, &bits, sizeof(value));
	return value;
}

float MSG_ReadAngle (unsigned int flags)
{
	(void)flags;
	return MSG_ReadByte() * (360.0f / 256);
}

float MSG_ReadAngle16 (unsigned int flags)
{
	(void)flags;
	return MSG_ReadShort() * (360.0f / 65536);
}

unsigned int MSG_ReadEntity (unsigned int pext2)
{
	(void)pext2;
	return (unsigned short)MSG_ReadShort();
}

void MSG_WriteByte (sizebuf_t *buf, int value)
{
	(void)buf;
	(void)value;
	assert(!"unexpected pitch correction");
}

void MSG_WriteAngle (sizebuf_t *buf, float value, unsigned int flags)
{
	(void)buf;
	(void)value;
	(void)flags;
	assert(!"unexpected pitch correction");
}

qboolean NET_QSocketGetProQuakeAngleHack (const struct qsocket_s *sock)
{
	(void)sock;
	return false;
}

/* Filesystem and model test doubles. */
qboolean COM_FileExists (const char *name, unsigned int *path_id)
{
	(void)path_id;
	assert(!strcmp(name, "progs/v_siga.mdl"));
	return have_model;
}

int SV_Precache_Model (const char *name)
{
	assert(have_model && !strcmp(name, "progs/v_siga.mdl"));
	sv.models[CIGARETTE_MODEL] = &cigarette_model;
	return CIGARETTE_MODEL;
}

int SV_ModelIndex (const char *name)
{
	assert(have_model && !strcmp(name, "progs/v_siga.mdl"));
	return CIGARETTE_MODEL;
}

const char *PR_GetString (int index)
{
	switch (index)
	{
	case 0: return "";
	case CIGARETTE_MODEL: return "progs/v_siga.mdl";
	case SHOTGUN_MODEL: return "progs/v_shot.mdl";
	case ROCKET_MODEL: return "progs/v_rock.mdl";
	default: assert(!"invalid engine string"); return "";
	}
}

int PR_SetEngineString (const char *name)
{
	assert(!strcmp(name, "progs/v_siga.mdl"));
	return CIGARETTE_MODEL;
}

int q_strcasecmp (const char *a, const char *b)
{
	while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b))
	{
		a++;
		b++;
	}
	return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

eval_t *GetEdictFieldValue (edict_t *ent, int field)
{
	(void)ent;
	(void)field;
	return NULL;
}

/* Emulate vanilla weaponframe resets and QC input handling, not the fix. */
qboolean SV_RunThink (edict_t *ent)
{
	ent->v.weaponframe = 0;
	return true;
}

void PF_sv_pmove (void)
{
}

void PR_ExecuteProgram (func_t function)
{
	edict_t *ent = host_client->edict;
	qc_calls++;
	assert(function == PRETHINK || function == POSTTHINK || function == RUNCOMMAND);

	if (function == RUNCOMMAND)
	{
		/* Mods using prediction can copy input_impulse back into self.impulse. */
		ent->v.impulse = input_impulse;
		if ((int)input_buttons & 1)
			shots++;
	}
	else if (function == POSTTHINK)
	{
		if (ent->v.impulse == 9 && !deathmatch.value && ent->v.health > 0 && !ent->v.deadflag)
		{
			cheat_calls++;
			ent->v.items = (int)ent->v.items | IT_ROCKET_LAUNCHER;
			ent->v.ammo_rockets = 100;
			ent->v.weaponmodel = ROCKET_MODEL;
			ent->v.weapon = IT_ROCKET_LAUNCHER;
			ent->v.currentammo = ent->v.ammo_rockets;
		}
		else if (ent->v.impulse >= 1 && ent->v.impulse <= 8)
		{
			ent->v.weaponmodel = SHOTGUN_MODEL;
			ent->v.weapon = IT_SHOTGUN;
			ent->v.currentammo = ent->v.ammo_shells;
		}
		ent->v.impulse = 0;
		// Emulate W_SetCurrentAmmo after a pickup/refresh, independently of
		// the engine fix. It reads the real QC weapon's ammunition pool.
		if (refresh_currentammo)
			ent->v.currentammo = (ent->v.weapon == IT_ROCKET_LAUNCHER)
				? ent->v.ammo_rockets : ent->v.ammo_shells;
		if (ent->v.button0 && ent->v.health > 0 && !ent->v.deadflag)
			shots++;
	}
}

static void Reset (qboolean model_available, int mode)
{
	memset(&sv, 0, sizeof(sv));
	memset(&client, 0, sizeof(client));
	memset(edicts, 0, sizeof(edicts));
	memset(&globals, 0, sizeof(globals));
	host_client = &client;
	qcvm = &sv.qcvm;
	qcvm->globals = globals.words;
	qcvm->edicts = edicts;
	qcvm->time = 10;
	pr_global_struct = &globals.vars;
	pr_global_struct->PlayerPreThink = PRETHINK;
	pr_global_struct->PlayerPostThink = POSTTHINK;
	client.edict = &edicts[1];
	client.knowntoqc = client.active = true;
	client.edict->v.health = 100;
	client.edict->v.items = IT_AXE | IT_SHOTGUN;
	client.edict->v.weaponmodel = SHOTGUN_MODEL;
	client.edict->v.weapon = IT_SHOTGUN;
	client.edict->v.ammo_shells = client.edict->v.currentammo = 25;
	have_model = model_available;
	refresh_currentammo = false;
	cigarette_model.numframes = 20;
	deathmatch.value = 0;
	cheat_calls = shots = qc_calls = wire_sequence = 0;
	input_impulse = input_buttons = 0;
	sv.protocol = PROTOCOL_NETQUAKE;
	sv_nqplayerphysics.value = 1;
	if (mode)
	{
		/* Exercise both engine pmove and the SV_RunClientCommand entry point. */
		sv_nqplayerphysics.value = 0;
		qcvm->extglobals.input_impulse = &input_impulse;
		qcvm->extglobals.input_buttons = &input_buttons;
		client.protocol_pext2 = PEXT2_PREDINFO;
		if (mode >= 2)
			qcvm->extfuncs.SV_RunClientCommand = RUNCOMMAND;
		if (mode == 3)
			client.protocol_pext2 |= PEXT2_PRYDONCURSOR;
	}
}

static void ReadMove (int impulse, int buttons)
{
	int i;
	packet_size = packet_read = 0;
	if (client.protocol_pext2 & PEXT2_PREDINFO)
		PutShort(++wire_sequence);
	PutFloat(qcvm->time);
	for (i = 0; i < 3; i++)
	{
		if (client.protocol_pext2 & PEXT2_PREDINFO)
			PutShort(0);
		else
			PutByte(0);
	}
	for (i = 0; i < 3; i++)
		PutShort(0);
	if (client.protocol_pext2 & PEXT2_PRYDONCURSOR)
	{
		PutShort(buttons);
		PutShort(buttons >> 16);
	}
	else
		PutByte(buttons);
	PutByte(impulse);
	SV_ReadClientMove(&client.cmd);
	assert(packet_read == packet_size);
}

static void StandardThink (void)
{
	float saved_button0;
	qboolean equip_cigarette;
	SV_CigarettePreThink(host_client, client.edict, &saved_button0, &equip_cigarette);
	PR_ExecuteProgram(PRETHINK);
	SV_RunThink(client.edict);
	PR_ExecuteProgram(POSTTHINK);
	SV_CigarettePostThink(host_client, client.edict, saved_button0, equip_cigarette);
	if (client.siga_active)
		assert(client.edict->v.currentammo == 1);
}

static void Move (int impulse, int buttons)
{
	ReadMove(impulse, buttons);
	if (!client.usingpmove)
		StandardThink();
	if (client.siga_active)
		assert(client.edict->v.currentammo == 1);
}

static void CheckNoCheat (void)
{
	assert(cheat_calls == 0);
	assert((int)client.edict->v.items == (IT_AXE | IT_SHOTGUN));
	assert(client.edict->v.ammo_shells == 25);
	assert(client.edict->v.ammo_nails == 0);
	assert(client.edict->v.ammo_rockets == 0);
	assert(client.edict->v.ammo_cells == 0);
}

static void TestCigarette (int mode, qboolean model_available)
{
	Reset(model_available, mode);
	ReadMove(9, 0);
	/* The raw request must be gone even before standard physics/StartFrame. */
	assert(client.edict->v.impulse == 0);
	assert(input_impulse == 0);
	if (!client.usingpmove)
		StandardThink();
	CheckNoCheat();
	assert(client.siga_active == model_available);
	assert(client.edict->v.weaponmodel == (model_available ? CIGARETTE_MODEL : SHOTGUN_MODEL));
	assert(client.edict->v.currentammo == (model_available ? 1 : 25));
	Move(0, 0);
	CheckNoCheat();
}

static void TestCheat101 (int mode, qboolean model_available)
{
	Reset(model_available, mode);
	Move(101, 0);
	assert(cheat_calls == 1);
	assert((int)client.edict->v.items & IT_ROCKET_LAUNCHER);
	assert(client.edict->v.ammo_rockets == 100);
	assert(client.edict->v.weaponmodel == ROCKET_MODEL);
	assert(!client.siga_active);
	Move(0, 0);
	assert(cheat_calls == 1);

	Reset(model_available, mode);
	deathmatch.value = 1;
	Move(101, 0);
	CheckNoCheat();
}

static void TestAttackAndSwitch (int mode)
{
	int i;
	Reset(true, mode);
	Move(9, 3); // attack + jump: only attack should be masked
	assert(client.edict->v.button2 == 1);
	if (mode)
		assert(input_buttons == 2);
	CheckNoCheat();
	assert(shots == 0);
	assert(client.siga_smoking && !client.siga_releasing);
	assert(client.edict->v.button0 == 1); // keep held input between packets
	assert(client.edict->v.weaponframe == 1);
	qcvm->time += 0.11;
	Move(0, 1);
	assert(shots == 0);
	assert(client.edict->v.weaponframe == 2);
	qcvm->time += 0.11;
	Move(0, 0); // early release must finish the full animation, without a hold
	assert(client.siga_releasing);
	assert(client.edict->v.weaponframe == 3);
	for (i = 4; i < cigarette_model.numframes; i++)
	{
		qcvm->time += 0.11;
		Move(0, 0);
		assert(client.siga_smoking && client.siga_releasing);
		assert(client.edict->v.weaponframe == i);
	}
	qcvm->time += 0.11;
	Move(0, 0);
	assert(!client.siga_smoking && !client.siga_releasing);
	assert(client.edict->v.weaponframe == 0);
	assert(shots == 0);

	/* Ordinary weapon impulses must still reach QC and leave cigarette mode. */
	for (i = 1; i <= 8; i++)
	{
		Move(9, 0);
		assert(client.siga_active);
		Move(i, 0);
		assert(!client.siga_active);
		assert(client.edict->v.weaponmodel == SHOTGUN_MODEL);
	}
	Move(9, 0);
	Move(101, 0);
	assert(cheat_calls == 1 && !client.siga_active);
}

static void TestHoldAndRelease (int mode)
{
	int frame, i;
	Reset(true, mode);
	Move(9, 1);
	assert(client.edict->v.weaponframe == 1);
	qcvm->time += 0.099;
	Move(0, 1);
	assert(client.edict->v.weaponframe == 1); // fixed 0.1s, not a configurable rate
	qcvm->time += 0.002;
	Move(0, 1);
	assert(client.edict->v.weaponframe == 2);
	for (frame = 3; frame <= 10; frame++)
	{
		qcvm->time += 0.11;
		Move(0, 1);
		assert(client.siga_smoking && !client.siga_releasing);
		assert(client.edict->v.weaponframe == frame);
	}
	for (i = 0; i < 30; i++)
	{
		qcvm->time += 1;
		Move(0, 1);
		assert(client.siga_smoking && !client.siga_releasing);
		assert(client.edict->v.weaponframe == 10); // never loop while held
	}

	Move(0, 0);
	assert(client.siga_releasing);
	assert(client.edict->v.weaponframe == 11); // promptly resume on release
	for (frame = 12; frame < cigarette_model.numframes; frame++)
	{
		qcvm->time += 0.11;
		Move(0, 0);
		assert(client.edict->v.weaponframe == frame);
	}
	qcvm->time += 0.11;
	Move(0, 0);
	assert(!client.siga_smoking && !client.siga_releasing);
	assert(client.edict->v.weaponframe == 0);
	assert(shots == 0);
	CheckNoCheat();

	Move(0, 1);
	assert(client.siga_smoking && !client.siga_releasing);
	assert(client.edict->v.weaponframe == 1); // another distinct press can smoke
}

static void TestRepressDuringRelease (int mode)
{
	int frame;
	Reset(true, mode);
	Move(9, 1);
	Move(0, 0); // release a short tap at frame 1
	assert(client.siga_releasing && client.edict->v.weaponframe == 2);
	for (frame = 3; frame < cigarette_model.numframes; frame++)
	{
		qcvm->time += 0.11;
		Move(0, 1);
		assert(client.siga_releasing);
		assert(client.edict->v.weaponframe == frame); // new press cannot rewind
	}
	qcvm->time += 0.11;
	Move(0, 1);
	assert(!client.siga_smoking && !client.siga_releasing);
	assert(client.edict->v.weaponframe == 0);
	qcvm->time += 1;
	Move(0, 1);
	assert(client.edict->v.weaponframe == 0); // require a fresh press after idle
	Move(0, 0);
	Move(0, 1);
	assert(client.edict->v.weaponframe == 1);
	assert(shots == 0);
	CheckNoCheat();
}

static void TestHeldInputBetweenPackets (void)
{
	int frame, i;
	Reset(true, 0);
	Move(9, 1);
	for (frame = 2; frame <= 10; frame++)
	{
		qcvm->time += 0.11;
		StandardThink(); // no new packet: attack must remain held
		assert(client.edict->v.button0 == 1);
		assert(client.siga_smoking && !client.siga_releasing);
		assert(client.edict->v.weaponframe == frame);
	}
	for (i = 0; i < 20; i++)
	{
		qcvm->time += 0.11;
		StandardThink();
		assert(!client.siga_releasing);
		assert(client.edict->v.weaponframe == 10);
	}
	Move(0, 0);
	assert(client.siga_releasing && client.edict->v.weaponframe == 11);
	qcvm->time += 0.11;
	StandardThink();
	assert(client.edict->v.weaponframe == 12);
	assert(shots == 0);
	CheckNoCheat();
}

static void TestFrameBounds (int mode)
{
	static const int model_frames[] = {0, 1, 2, 5, 10, 11, 12, 20};
	int n, i, frames, start, end, hold, released_frame;
	for (n = 0; n < (int)(sizeof(model_frames) / sizeof(model_frames[0])); n++)
	{
		Reset(true, mode);
		cigarette_model.numframes = model_frames[n];
		frames = model_frames[n] > 0 ? model_frames[n] : 1;
		start = frames > 1 ? 1 : 0;
		end = frames - 1;
		hold = end < 10 ? end : 10;
		Move(9, 0);
		assert(client.edict->v.weaponframe == 0);
		Move(0, 1);
		assert(client.edict->v.weaponframe == start);
		for (i = 0; i < frames + 20; i++)
		{
			qcvm->time += 0.11;
			Move(0, 1);
			assert(client.edict->v.weaponframe >= start && client.edict->v.weaponframe <= hold);
			assert(client.siga_smoking && !client.siga_releasing);
		}
		assert(client.edict->v.weaponframe == hold);
		Move(0, 0);
		released_frame = hold < end ? hold + 1 : end;
		assert(client.siga_releasing && client.edict->v.weaponframe == released_frame);
		for (i = 0; i < frames + 2; i++)
		{
			qcvm->time += 0.11;
			Move(0, 0);
			assert(client.edict->v.weaponframe >= 0 && client.edict->v.weaponframe <= end);
		}
		assert(!client.siga_smoking && !client.siga_releasing);
		assert(client.edict->v.weaponframe == 0);
		assert(shots == 0);
		CheckNoCheat();
	}
}

static void TestCosmeticAmmo (int mode)
{
	int cycle, i;
	Reset(true, mode);
	refresh_currentammo = true;
	Move(9, 0);
	assert(client.edict->v.currentammo == 1);

	// QC ammo refreshes must not replace the cosmetic 1 with real shells.
	client.edict->v.currentammo = 0;
	Move(0, 0);
	assert(client.edict->v.currentammo == 1);
	for (cycle = 0; cycle < 4; cycle++)
	{
		Move(0, 1);
		for (i = 0; i < 20; i++)
		{
			qcvm->time += 0.11;
			Move(0, 1);
			assert(client.edict->v.currentammo == 1);
			CheckNoCheat();
		}
		assert(client.edict->v.weaponframe == 10);
		Move(0, 0);
		for (i = 0; i < cigarette_model.numframes; i++)
		{
			qcvm->time += 0.11;
			Move(0, 0);
			assert(client.edict->v.currentammo == 1);
			CheckNoCheat();
		}
		assert(!client.siga_smoking && client.edict->v.weaponframe == 0);
	}
	assert(shots == 0);

	// Switching must restore the QC weapon's HUD ammo, not leave it at 1.
	Move(2, 0);
	assert(!client.siga_active && client.edict->v.currentammo == 25);
	CheckNoCheat();
	Move(9, 0);
	assert(client.edict->v.currentammo == 1);
	Move(101, 0);
	assert(!client.siga_active && client.edict->v.currentammo == 100);
	assert(cheat_calls == 1);

	// The cosmetic round does not grant or consume real ammo, even when empty.
	Reset(true, mode);
	refresh_currentammo = true;
	client.edict->v.ammo_shells = client.edict->v.currentammo = 0;
	Move(9, 1);
	Move(0, 0);
	qcvm->time += 0.11;
	Move(0, 0);
	assert(client.edict->v.currentammo == 1);
	assert(client.edict->v.ammo_shells == 0 && client.edict->v.ammo_nails == 0);
	assert(client.edict->v.ammo_rockets == 0 && client.edict->v.ammo_cells == 0);
	assert(shots == 0 && cheat_calls == 0);
	Move(2, 0);
	assert(!client.siga_active && client.edict->v.currentammo == 0);
}

static void TestAnimationReset (int mode)
{
	int releasing;
	for (releasing = 0; releasing < 2; releasing++)
	{
		Reset(true, mode);
		Move(9, 1);
		if (releasing)
			Move(0, 0);
		assert(client.siga_smoking);
		Move(2, 0);
		assert(!client.siga_active && !client.siga_smoking);
		assert(!client.siga_releasing && !client.siga_attack_down);
		assert(client.siga_next_frame_time == 0);
		Move(9, 0);
		assert(client.siga_active && client.edict->v.weaponframe == 0);

		Move(0, 1);
		if (releasing)
			Move(0, 0);
		client.edict->v.health = 0;
		client.edict->v.deadflag = 1;
		Move(0, 0);
		assert(!client.siga_active && !client.siga_smoking);
		assert(!client.siga_releasing && !client.siga_attack_down);
		assert(client.siga_next_frame_time == 0);
		client.edict->v.health = 100;
		client.edict->v.deadflag = 0;
		Move(0, 0);
		assert(!client.siga_active);
		CheckNoCheat();
	}
}

static void TestDeadPlayer (int mode)
{
	Reset(true, mode);
	client.edict->v.health = 0;
	client.edict->v.deadflag = 1;
	Move(9, 0);
	CheckNoCheat();
	assert(!client.siga_active);
	assert(client.edict->v.impulse == 0);
	client.edict->v.health = 100;
	client.edict->v.deadflag = 0;
	Move(0, 0);
	assert(!client.siga_active);
	CheckNoCheat();
}

static void TestQueuedMoves (void)
{
	Reset(true, 0);
	ReadMove(9, 0);
	ReadMove(0, 0); // no impulse must not cancel the queued cigarette request
	StandardThink();
	CheckNoCheat();
	assert(client.siga_active);

	Reset(true, 0);
	ReadMove(9, 0);
	ReadMove(2, 0); // newest nonzero impulse wins, just as with ordinary weapons
	StandardThink();
	CheckNoCheat();
	assert(!client.siga_active);

	Reset(false, 0);
	ReadMove(101, 0);
	ReadMove(9, 0); // clear the pending cheat, even without v_siga.mdl
	StandardThink();
	CheckNoCheat();

	Reset(true, 3);
	Move(101, 0);
	{
		int calls = qc_calls;
		wire_sequence--; // resend an already-processed prediction sequence
		ReadMove(9, 0);
		assert(qc_calls == calls);
		assert(cheat_calls == 1);
		assert(!client.siga_active);
	}
}

int main (void)
{
	int mode;
	for (mode = 0; mode < 4; mode++)
	{
		TestCigarette(mode, true);
		TestCigarette(mode, false);
		TestCheat101(mode, true);
		TestCheat101(mode, false);
		TestAttackAndSwitch(mode);
		TestHoldAndRelease(mode);
		TestRepressDuringRelease(mode);
		TestFrameBounds(mode);
		TestCosmeticAmmo(mode);
		TestAnimationReset(mode);
		TestDeadPlayer(mode);
	}
	TestHeldInputBetweenPackets();
	TestQueuedMoves();
	puts("PASS: impulse 9 is cigarette-only; impulse 101 keeps the QC cheat.");
	puts("PASS: draw to frame 10, hold until release, finish once and return to idle.");
	puts("PASS: early release, repress, packet gaps, frame bounds, switching and death.");
	puts("PASS: cosmetic ammo stays at 1; real weapon ammo is unchanged and restored on switch.");
	puts("PASS: missing model, prediction, deathmatch, queued and duplicate commands.");
	return 0;
}
