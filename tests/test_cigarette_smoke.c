/*
 * Asset-free tests for the real, private cigarette smoke emitter. Including
 * view.c keeps it private in production; function-section GC strips unrelated
 * view/renderer code. Only the particle backend and camera inputs are doubles.
 */
#include "../Quake/view.c"

#include <assert.h>
#include <ctype.h>

client_state_t cl;
kbutton_t in_attack;
refdef_t r_refdef;
cvar_t r_drawviewmodel = {"r_drawviewmodel", "1", CVAR_NONE};
cvar_t chase_active = {"chase_active", "0", CVAR_NONE};
cvar_t scr_viewsize = {"viewsize", "100", CVAR_NONE};

static entity_t view;
static qmodel_t model;
static int smoke_calls, fallback_calls, smoke_particles, fallback_particles;
static qboolean missing_effect;

int q_strcasecmp (const char *a, const char *b)
{
	while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b))
	{
		a++;
		b++;
	}
	return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

void AngleVectors (vec3_t angles, vec3_t forward, vec3_t right, vec3_t up)
{
	assert(angles[0] == 0 && angles[1] == 0 && angles[2] == 0);
	forward[0] = 1;
	forward[1] = forward[2] = 0;
	right[1] = -1;
	right[0] = right[2] = 0;
	up[2] = 1;
	up[0] = up[1] = 0;
}

int PScript_RunParticleEffectTypeString (vec3_t org, vec3_t dir, float count, const char *name)
{
	assert(!strcmp(name, "qssm.cigarette_smoke"));
	assert(count == 8);
	assert(org[0] == 16 && org[1] == -8 && org[2] == -7);
	assert(dir[0] == 0 && dir[1] == 0 && dir[2] == 1);
	smoke_calls++;
	smoke_particles += (int)count;
	return missing_effect;
}

void R_RunParticleEffect (vec3_t org, vec3_t dir, int color, int count)
{
	(void)org;
	(void)dir;
	assert(missing_effect && color == 7 && count == 16);
	fallback_calls++;
	fallback_particles += count;
}

static void Reset (void)
{
	memset(&cl, 0, sizeof(cl));
	memset(&in_attack, 0, sizeof(in_attack));
	memset(&r_refdef, 0, sizeof(r_refdef));
	memset(&view, 0, sizeof(view));
	memset(&model, 0, sizeof(model));
	/* Clear the emitter's timer before starting another test. */
	V_EmitCigaretteSmoke(&view);
	strcpy(model.name, "progs/v_siga.mdl");
	model.numframes = 20;
	view.model = &model;
	cl.stats[STAT_HEALTH] = 100;
	cl.time = 10;
	r_drawviewmodel.value = 1;
	chase_active.value = 0;
	scr_viewsize.value = 100;
	smoke_calls = fallback_calls = smoke_particles = fallback_particles = 0;
	missing_effect = false;
}

static void Emit (int frame, qboolean attack_down, double elapsed)
{
	view.frame = frame;
	in_attack.state = attack_down ? 1 : 0;
	cl.time += elapsed;
	V_EmitCigaretteSmoke(&view);
}

static void TestReleaseOnly (void)
{
	int frame, i;
	Reset();
	Emit(0, false, 0);
	assert(smoke_calls == 0);

	/* No particles while drawing or holding, even when the frame is non-idle. */
	for (frame = 1; frame <= 10; frame++)
		Emit(frame, true, 0.2);
	for (i = 0; i < 20; i++)
		Emit(10, true, 0.2);
	assert(smoke_calls == 0);

	/* Release is allowed to emit immediately, even before the next snapshot. */
	Emit(10, false, 0);
	assert(smoke_calls == 1);
	for (frame = 11; frame < model.numframes; frame++)
		Emit(frame, false, 0.2);
	assert(smoke_calls == 10);
	Emit(0, false, 0.2);
	Emit(0, false, 1);
	assert(smoke_calls == 10);
	assert(fallback_calls == 0);

	/* A short tap releases before reaching frame 10, but still emits smoke. */
	Reset();
	Emit(1, true, 0);
	Emit(2, false, 0.01);
	assert(smoke_calls == 1);
	Emit(3, false, 0.01);
	assert(smoke_calls == 1); // emission interval still applies
	Emit(3, false, 0.04);
	assert(smoke_calls == 2);
	Emit(4, true, 0.2);
	assert(smoke_calls == 2); // another hold must never emit new particles
	Emit(5, false, 0);
	assert(smoke_calls == 3);
}

static void TestGatesAndFallback (void)
{
	Reset();
	view.frame = 11;
	view.model = NULL;
	V_EmitCigaretteSmoke(&view);
	view.model = &model;
	strcpy(model.name, "progs/v_shot.mdl");
	V_EmitCigaretteSmoke(&view);
	strcpy(model.name, "progs/v_siga.mdl");
	cl.stats[STAT_HEALTH] = 0;
	V_EmitCigaretteSmoke(&view);
	cl.stats[STAT_HEALTH] = 100;
	r_drawviewmodel.value = 0;
	V_EmitCigaretteSmoke(&view);
	r_drawviewmodel.value = 1;
	chase_active.value = 1;
	V_EmitCigaretteSmoke(&view);
	chase_active.value = 0;
	scr_viewsize.value = 130;
	V_EmitCigaretteSmoke(&view);
	scr_viewsize.value = 100;
	assert(smoke_calls == 0);

	missing_effect = true;
	V_EmitCigaretteSmoke(&view);
	assert(smoke_calls == 1 && fallback_calls == 1);
	Emit(12, true, 0.2);
	assert(smoke_calls == 1 && fallback_calls == 1);
}

static void TestFixedIdle (void)
{
	Reset();
	Emit(0, false, 0);
	assert(smoke_calls == 0);
	Emit(1, false, 0);
	assert(smoke_calls == 1);
	Emit(0, false, 0.2);
	assert(smoke_calls == 1);
	model.numframes = 1;
	Emit(0, false, 0.2);
	assert(smoke_calls == 1);
}

static void TestDenseSmoke (void)
{
	int i;
	Reset();
	Emit(11, false, 0);
	assert(smoke_calls == 1 && smoke_particles == 8);
	Emit(12, false, 0.039);
	assert(smoke_calls == 1);
	Emit(12, false, 0.002);
	assert(smoke_calls == 2 && smoke_particles == 16);

	// Pin the hardcoded rate: roughly 25 bursts/second, not the old 8.
	Reset();
	Emit(11, false, 0);
	for (i = 0; i < 1000; i++)
		Emit(11, false, 0.001);
	assert(smoke_calls >= 24 && smoke_calls <= 26);
	assert(smoke_particles == smoke_calls * 8);
	assert(fallback_particles == 0);

	Reset();
	missing_effect = true;
	Emit(11, false, 0);
	assert(fallback_calls == 1 && fallback_particles == 16);
	for (i = 0; i < 1000; i++)
		Emit(11, false, 0.001);
	assert(fallback_calls >= 24 && fallback_calls <= 26);
	assert(fallback_particles == fallback_calls * 16);
}

int main (void)
{
	TestReleaseOnly();
	TestGatesAndFallback();
	TestFixedIdle();
	TestDenseSmoke();
	puts("PASS: no cigarette smoke on held attack; smoke follows release until idle.");
	puts("PASS: fixed smoke settings, 8-particle bursts every 0.04s, dense classic fallback.");
	puts("PASS: early release, fixed idle frame and visibility/health gates.");
	return 0;
}
