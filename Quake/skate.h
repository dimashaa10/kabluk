/* Engine-side prototype skating. Model vertices use the player's origin;
 * lift is applied to both visible models, never to the player's collision hull.
 */
#ifndef QUAKE_SKATE_H
#define QUAKE_SKATE_H

#define SKATE_MODEL "progs/skate.mdl"
#define SKATE_MODEL_ROOT "skate.mdl"
#define SKATE_CLASSNAME "engine_skateboard"
#define SKATE_RIDER_FRAME 0	// idle stance: do not run in place on the board
#define SKATE_FLIP_DURATION 0.5f	// one sideways 360-degree roll per jump
#define SKATE_MAX_SPEED 420.0f
#define SKATE_ACCELERATION 420.0f
#define SKATE_ROLLING_DRAG 0.65f
#define SKATE_SIDE_TRACTION 3.5f
#define SKATE_WALL_BOUNCE_MIN_SPEED 300.0f
#define SKATE_WALL_BOUNCE_MIN_IMPACT_SPEED 180.0f
#define SKATE_WALL_BOUNCE_RESTITUTION 0.75f
#define SKATE_WALL_BOUNCE_UPWARD_SPEED 220.0f
#define SKATE_WALL_BOUNCE_MAX_NORMAL_Z 0.2f
#define SKATE_SLIDE_MIN_SPEED SKATE_WALL_BOUNCE_MIN_SPEED
#define SKATE_SLIDE_MIN_IMPACT_SPEED SKATE_WALL_BOUNCE_MIN_IMPACT_SPEED
#define SKATE_SLIDE_DIRECTION_EPSILON 24.0f
#define SKATE_SLIDE_SPEED_SCALE 0.75f
#define SKATE_SLIDE_MIN_NORMAL_DOT 0.95f
#define SKATE_SLIDE_PROBE_DISTANCE 64.0f
#define SKATE_SLIDE_MAX_ADHESION_DISTANCE 24.0f
#define SKATE_SLIDE_SURFACE_GAP 1.0f
#define SKATE_SLIDE_PROBE_HEIGHT_MARGIN 4.0f
#define SKATE_BRAKING 5.0f
#define SKATE_TURN_RATE 120.0f
#define SKATE_STEER_ANGLE 35.0f
#define SKATE_INPUT_SCALE 400.0f
#define SKATE_CAMERA_BACK 140.0f
#define SKATE_CAMERA_UP 16.0f
#define SKATE_CAMERA_PITCH 10.0f
#define SKATE_CAMERA_FOCUS_DROP 16.0f
#define SKATE_GROUND_CLEARANCE 1.0f
#define SKATE_MAX_LIFT 64.0f

/* One stock-compatible stat carries both mode and camera lift (1/256 units).
 * The signature avoids treating another mod's ordinary stat 19 as skating.
 */
#define SKATE_STAT_MAGIC 0x534b0000u
#define SKATE_STAT_MASK 0xffff0000u
#define SKATE_HEIGHT_MASK 0xffffu
#define SKATE_HEIGHT_SCALE 256.0f

#endif /* QUAKE_SKATE_H */
