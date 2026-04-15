/*
thread_system.h - Multi-threading support for Kabluk Engine
- Parallel resource loading
- Physics thread
- Job system for rendering
*/

#ifndef THREAD_SYSTEM_H
#define THREAD_SYSTEM_H

#include "quakedef.h"

#if defined(SDL_FRAMEWORK) || defined(NO_SDL_CONFIG)
#if defined(USE_SDL2)
#include <SDL2/SDL.h>
#else
#include <SDL/SDL.h>
#endif
#else
#include "SDL.h"
#endif

//=============================================================================
// Configuration
//=============================================================================

#define MAX_LOAD_THREADS    4
#define MAX_RENDER_JOBS     64
#define PHYSICS_THREAD_STACK_SIZE (2 * 1024 * 1024)

//=============================================================================
// Thread-safe resource loading
//=============================================================================

typedef enum
{
    LOAD_MODEL,
    LOAD_SOUND,
    LOAD_TEXTURE
} load_type_t;

typedef struct load_job_s
{
    load_type_t type;
    char name[MAX_QPATH];
    void *result;
    qboolean complete;
    qboolean failed;
    struct load_job_s *next;
} load_job_t;

typedef struct
{
    SDL_Thread *threads[MAX_LOAD_THREADS];
    int num_threads;
    
    load_job_t *job_queue;
    load_job_t *free_jobs;
    
    SDL_mutex *queue_mutex;
    SDL_sem *job_semaphore;
    
    qboolean shutdown;
} loader_system_t;

extern loader_system_t loader_sys;

qboolean Loader_Init(void);
void Loader_Shutdown(void);
void Loader_QueueModel(const char *name);
void Loader_QueueSound(const char *name);
void *Loader_GetResult(const char *name);
qboolean Loader_IsComplete(const char *name);

//=============================================================================
// Physics Thread
//=============================================================================

typedef struct
{
    SDL_Thread *thread;
    SDL_mutex *mutex;
    SDL_sem *frame_semaphore;
    SDL_sem *done_semaphore;
    
    double frametime;
    qboolean running;
    qboolean has_work;
    
    // Double buffering for physics state
    double current_time;
    double next_time;
} physics_thread_t;

extern physics_thread_t phys_thread;

qboolean PhysThread_Init(void);
void PhysThread_Shutdown(void);
void PhysThread_StartFrame(double frametime);
void PhysThread_WaitComplete(void);
double PhysThread_GetCurrentTime(void);

//=============================================================================
// Job System for Rendering
//=============================================================================

typedef enum
{
    JOB_DRAW_WORLD,
    JOB_DRAW_BRUSHES,
    JOB_DRAW_ALIAS,
    JOB_DRAW_SPRITES,
    JOB_DRAW_PARTICLES,
    JOB_DRAW_TRANSPARENT,
    JOB_SHADOWS,
    JOB_LIGHTING,
    JOB_POST_PROCESS
} job_type_t;

typedef void (*job_func_t)(void *data);

typedef struct render_job_s
{
    job_type_t type;
    job_func_t func;
    void *data;
    qboolean complete;
    int priority;
    struct render_job_s *next;
} render_job_t;

typedef struct
{
    render_job_t jobs[MAX_RENDER_JOBS];
    render_job_t *free_list;
    render_job_t *pending_queue;
    render_job_t *completed_queue;
    
    SDL_mutex *queue_mutex;
    SDL_cond *job_condition;
    
    SDL_Thread *worker_threads[4];
    int num_workers;
    
    qboolean shutdown;
    qboolean frame_active;
    
    int jobs_submitted;
    int jobs_completed;
} job_system_t;

extern job_system_t render_jobs;

qboolean JobSystem_Init(void);
void JobSystem_Shutdown(void);
void JobSystem_BeginFrame(void);
render_job_t *JobSystem_AllocJob(job_type_t type, job_func_t func, void *data);
void JobSystem_SubmitJob(render_job_t *job);
void JobSystem_WaitFrame(void);
qboolean JobSystem_IsFrameComplete(void);

//=============================================================================
// Thread-safe counters and synchronization
//=============================================================================

typedef struct
{
    SDL_atomic_t counter;
} atomic_counter_t;

static inline void AtomicCounter_Init(atomic_counter_t *counter, int value)
{
    SDL_AtomicSet(&counter->counter, value);
}

static inline int AtomicCounter_Inc(atomic_counter_t *counter)
{
    return SDL_AtomicIncRef(&counter->counter);
}

static inline int AtomicCounter_Dec(atomic_counter_t *counter)
{
    return SDL_AtomicDecRef(&counter->counter);
}

static inline int AtomicCounter_Get(atomic_counter_t *counter)
{
    return SDL_AtomicGet(&counter->counter);
}

//=============================================================================
// Utility functions
//=============================================================================

int GetNumCPUThreads(void);
double GetThreadTime(void);

#endif // THREAD_SYSTEM_H
