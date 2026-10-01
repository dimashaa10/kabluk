/* Fixed cigarette-specific behavior shared by server animation and client
 * smoke. These values deliberately are not console variables.
 */
#ifndef QUAKE_CIGARETTE_H
#define QUAKE_CIGARETTE_H

#define SIGA_MODEL                  "progs/v_siga.mdl"
#define SIGA_IDLE_FRAME             0
#define SIGA_START_FRAME            1
#define SIGA_HOLD_FRAME             10
/* The finish always ends at the last real frame of the model. */
#define SIGA_FRAME_INTERVAL         0.1
#define SIGA_AMMO                   1

/* Camera-relative offsets in world units: forward, screen-right, screen-up. */
#define SIGA_SMOKE_FORWARD          16.0f
#define SIGA_SMOKE_RIGHT            8.0f
#define SIGA_SMOKE_UP               -7.0f
#define SIGA_SMOKE_INTERVAL         0.04
#define SIGA_SMOKE_COUNT            8
#define SIGA_SMOKE_CLASSIC_COUNT    16
#define SIGA_SMOKE_COLOR            7
#define SIGA_SMOKE_EFFECT           "qssm.cigarette_smoke"

#endif /* QUAKE_CIGARETTE_H */
