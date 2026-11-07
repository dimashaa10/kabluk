/*
 * port/psp/psp_mem.h -- the memory layout of the port, in one place.
 *
 * A PSP has one pool of RAM and no memory management unit per allocation: the
 * firmware hands out blocks from PSP_MEMORY_PARTITION_USER, and both newlib's
 * malloc() and this port's two arenas have to come out of it.  Getting that
 * split wrong is what killed three builds in a row, so the split lives in one
 * file and every other part of the port asks it rather than allocating.
 *
 * See psp_mem.c for what is taken, why, and in which order.
 */
#ifndef KABLUK_PSP_MEM_H
#define KABLUK_PSP_MEM_H

/* Take the port's blocks.  Call once, from main(), before anything else
 * allocates: the picture pool must exist before the first texture is uploaded
 * and the engine zone before Host_Init().
 *
 * zone_mb: the zone size to use instead of the measured one, or 0 for the
 * measurement (the engine's -mem switch).  Returns 1 when the engine got a
 * zone, 0 when even the minimum did not fit -- the caller then reports the
 * failure with psp_mem_report() in the message.
 */
int  psp_mem_init (int zone_mb);

/* The engine's zone: one contiguous block, owned until the module exits. */
void *psp_mem_zone (unsigned int *bytes);

/* The pictures' pool: one contiguous block, owned by the picture arena. */
void *psp_mem_pictures (unsigned int *bytes);

/* What the firmware still has free, for the log. */
void psp_mem_report (void);

#endif /* KABLUK_PSP_MEM_H */
