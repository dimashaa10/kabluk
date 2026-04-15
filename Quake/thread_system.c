/*
thread_system.c - Multi-threading implementation for Kabluk Engine
- Parallel resource loading
- Physics thread  
- Job system for rendering
*/

#include "quakedef.h"
#include "thread_system.h"

//=============================================================================
// Global instances
//=============================================================================

loader_system_t loader_sys;
physics_thread_t phys_thread;
job_system_t render_jobs;

//=============================================================================
// Utility functions
//=============================================================================

int GetNumCPUThreads(void)
{
    int num = SDL_GetCPUCount();
    return (num > 0) ? num : 1;
}

double GetThreadTime(void)
{
    return Sys_DoubleTime();
}

//=============================================================================
// Loader System Implementation
//=============================================================================

static int LoadWorkerThread(void *data)
{
    int thread_id = *(int*)data;
    free(data);
    
    while (!loader_sys.shutdown)
    {
        // Wait for a job
        SDL_SemWait(loader_sys.job_semaphore);
        
        if (loader_sys.shutdown)
            break;
        
        // Get job from queue
        SDL_LockMutex(loader_sys.queue_mutex);
        load_job_t *job = loader_sys.job_queue;
        if (job)
            loader_sys.job_queue = job->next;
        SDL_UnlockMutex(loader_sys.queue_mutex);
        
        if (!job)
            continue;
        
        // Execute the load operation
        job->failed = false;
        
        switch (job->type)
        {
            case LOAD_MODEL:
                job->result = Mod_ForName(job->name, false);
                job->failed = (job->result == NULL);
                break;
                
            case LOAD_SOUND:
                job->result = S_PrecacheSound(job->name);
                job->failed = (job->result == NULL);
                break;
                
            case LOAD_TEXTURE:
                // Texture loading would go here
                job->result = NULL;
                break;
        }
        
        job->complete = true;
        
        // Return job to free list
        SDL_LockMutex(loader_sys.queue_mutex);
        job->next = loader_sys.free_jobs;
        loader_sys.free_jobs = job;
        SDL_UnlockMutex(loader_sys.queue_mutex);
    }
    
    return 0;
}

qboolean Loader_Init(void)
{
    memset(&loader_sys, 0, sizeof(loader_sys));
    
    loader_sys.num_threads = q_min(MAX_LOAD_THREADS, GetNumCPUThreads());
    if (loader_sys.num_threads < 1)
        loader_sys.num_threads = 1;
    
    loader_sys.queue_mutex = SDL_CreateMutex();
    if (!loader_sys.queue_mutex)
        return false;
    
    loader_sys.job_semaphore = SDL_CreateSemaphore(0);
    if (!loader_sys.job_semaphore)
    {
        SDL_DestroyMutex(loader_sys.queue_mutex);
        return false;
    }
    
    // Pre-allocate job structures
    for (int i = 0; i < 64; i++)
    {
        load_job_t *job = (load_job_t *)Z_Malloc(sizeof(load_job_t));
        job->next = loader_sys.free_jobs;
        loader_sys.free_jobs = job;
    }
    
    // Create worker threads
    for (int i = 0; i < loader_sys.num_threads; i++)
    {
        int *thread_id = (int *)malloc(sizeof(int));
        *thread_id = i;
        
        char name[32];
        q_snprintf(name, sizeof(name), "Loader%d", i);
        
        loader_sys.threads[i] = SDL_CreateThread(LoadWorkerThread, name, thread_id);
        if (!loader_sys.threads[i])
        {
            Con_Printf("Warning: Could not create loader thread %d\n", i);
            free(thread_id);
        }
    }
    
    Con_Printf("Loader system initialized with %d threads\n", loader_sys.num_threads);
    return true;
}

void Loader_Shutdown(void)
{
    loader_sys.shutdown = true;
    
    // Wake up all threads
    for (int i = 0; i < loader_sys.num_threads; i++)
    {
        SDL_SemPost(loader_sys.job_semaphore);
    }
    
    // Wait for threads to finish
    for (int i = 0; i < loader_sys.num_threads; i++)
    {
        if (loader_sys.threads[i])
        {
            SDL_WaitThread(loader_sys.threads[i], NULL);
            loader_sys.threads[i] = NULL;
        }
    }
    
    // Free jobs
    SDL_LockMutex(loader_sys.queue_mutex);
    load_job_t *job = loader_sys.free_jobs;
    while (job)
    {
        load_job_t *next = job->next;
        Z_Free(job);
        job = next;
    }
    loader_sys.free_jobs = NULL;
    SDL_UnlockMutex(loader_sys.queue_mutex);
    
    if (loader_sys.job_semaphore)
        SDL_DestroySemaphore(loader_sys.job_semaphore);
    if (loader_sys.queue_mutex)
        SDL_DestroyMutex(loader_sys.queue_mutex);
    
    memset(&loader_sys, 0, sizeof(loader_sys));
}

void Loader_QueueModel(const char *name)
{
    SDL_LockMutex(loader_sys.queue_mutex);
    
    if (!loader_sys.free_jobs)
    {
        SDL_UnlockMutex(loader_sys.queue_mutex);
        return;
    }
    
    load_job_t *job = loader_sys.free_jobs;
    loader_sys.free_jobs = job->next;
    
    job->type = LOAD_MODEL;
    q_strlcpy(job->name, name, sizeof(job->name));
    job->result = NULL;
    job->complete = false;
    job->failed = false;
    job->next = NULL;
    
    // Add to queue
    if (!loader_sys.job_queue)
        loader_sys.job_queue = job;
    else
    {
        load_job_t *tail = loader_sys.job_queue;
        while (tail->next)
            tail = tail->next;
        tail->next = job;
    }
    
    SDL_UnlockMutex(loader_sys.queue_mutex);
    
    // Signal that we have work
    SDL_SemPost(loader_sys.job_semaphore);
}

void Loader_QueueSound(const char *name)
{
    SDL_LockMutex(loader_sys.queue_mutex);
    
    if (!loader_sys.free_jobs)
    {
        SDL_UnlockMutex(loader_sys.queue_mutex);
        return;
    }
    
    load_job_t *job = loader_sys.free_jobs;
    loader_sys.free_jobs = job->next;
    
    job->type = LOAD_SOUND;
    q_strlcpy(job->name, name, sizeof(job->name));
    job->result = NULL;
    job->complete = false;
    job->failed = false;
    job->next = NULL;
    
    // Add to queue
    if (!loader_sys.job_queue)
        loader_sys.job_queue = job;
    else
    {
        load_job_t *tail = loader_sys.job_queue;
        while (tail->next)
            tail = tail->next;
        tail->next = job;
    }
    
    SDL_UnlockMutex(loader_sys.queue_mutex);
    
    // Signal that we have work
    SDL_SemPost(loader_sys.job_semaphore);
}

void *Loader_GetResult(const char *name)
{
    void *result = NULL;
    
    SDL_LockMutex(loader_sys.queue_mutex);
    
    // Search through queued jobs
    load_job_t *job = loader_sys.job_queue;
    while (job)
    {
        if (!Q_strcmp(job->name, name) && job->complete)
        {
            result = job->result;
            break;
        }
        job = job->next;
    }
    
    SDL_UnlockMutex(loader_sys.queue_mutex);
    
    return result;
}

qboolean Loader_IsComplete(const char *name)
{
    qboolean complete = false;
    
    SDL_LockMutex(loader_sys.queue_mutex);
    
    load_job_t *job = loader_sys.job_queue;
    while (job)
    {
        if (!Q_strcmp(job->name, name))
        {
            complete = job->complete;
            break;
        }
        job = job->next;
    }
    
    SDL_UnlockMutex(loader_sys.queue_mutex);
    
    return complete;
}

//=============================================================================
// Physics Thread Implementation
//=============================================================================

static int PhysicsThreadFunc(void *data)
{
    (void)data;
    
    while (phys_thread.running)
    {
        // Wait for frame signal
        SDL_SemWait(phys_thread.frame_semaphore);
        
        if (!phys_thread.running)
            break;
        
        if (phys_thread.has_work)
        {
            // Lock and run physics
            SDL_LockMutex(phys_thread.mutex);
            
            // Run physics simulation
            // This would call SV_Physics or similar
            // For now, just update time
            phys_thread.current_time = phys_thread.next_time;
            phys_thread.next_time += phys_thread.frametime;
            
            SDL_UnlockMutex(phys_thread.mutex);
            
            // Signal completion
            SDL_SemPost(phys_thread.done_semaphore);
        }
    }
    
    return 0;
}

qboolean PhysThread_Init(void)
{
    memset(&phys_thread, 0, sizeof(phys_thread));
    
    phys_thread.mutex = SDL_CreateMutex();
    if (!phys_thread.mutex)
        return false;
    
    phys_thread.frame_semaphore = SDL_CreateSemaphore(0);
    if (!phys_thread.frame_semaphore)
    {
        SDL_DestroyMutex(phys_thread.mutex);
        return false;
    }
    
    phys_thread.done_semaphore = SDL_CreateSemaphore(0);
    if (!phys_thread.done_semaphore)
    {
        SDL_DestroySemaphore(phys_thread.frame_semaphore);
        SDL_DestroyMutex(phys_thread.mutex);
        return false;
    }
    
    phys_thread.running = true;
    phys_thread.has_work = false;
    phys_thread.frametime = 0.05; // Default 20Hz
    
    phys_thread.thread = SDL_CreateThread(PhysicsThreadFunc, "PhysicsThread", NULL);
    if (!phys_thread.thread)
    {
        SDL_DestroySemaphore(phys_thread.done_semaphore);
        SDL_DestroySemaphore(phys_thread.frame_semaphore);
        SDL_DestroyMutex(phys_thread.mutex);
        return false;
    }
    
    Con_Printf("Physics thread initialized\n");
    return true;
}

void PhysThread_Shutdown(void)
{
    phys_thread.running = false;
    
    // Wake up thread
    SDL_SemPost(phys_thread.frame_semaphore);
    
    // Wait for thread to finish
    if (phys_thread.thread)
    {
        SDL_WaitThread(phys_thread.thread, NULL);
        phys_thread.thread = NULL;
    }
    
    if (phys_thread.done_semaphore)
        SDL_DestroySemaphore(phys_thread.done_semaphore);
    if (phys_thread.frame_semaphore)
        SDL_DestroySemaphore(phys_thread.frame_semaphore);
    if (phys_thread.mutex)
        SDL_DestroyMutex(phys_thread.mutex);
    
    memset(&phys_thread, 0, sizeof(phys_thread));
}

void PhysThread_StartFrame(double frametime)
{
    SDL_LockMutex(phys_thread.mutex);
    phys_thread.frametime = frametime;
    phys_thread.has_work = true;
    SDL_UnlockMutex(phys_thread.mutex);
    
    // Signal physics thread to start
    SDL_SemPost(phys_thread.frame_semaphore);
}

void PhysThread_WaitComplete(void)
{
    SDL_SemWait(phys_thread.done_semaphore);
    
    SDL_LockMutex(phys_thread.mutex);
    phys_thread.has_work = false;
    SDL_UnlockMutex(phys_thread.mutex);
}

double PhysThread_GetCurrentTime(void)
{
    double time;
    SDL_LockMutex(phys_thread.mutex);
    time = phys_thread.current_time;
    SDL_UnlockMutex(phys_thread.mutex);
    return time;
}

//=============================================================================
// Job System Implementation
//=============================================================================

static int RenderJobWorker(void *data)
{
    int thread_id = *(int*)data;
    free(data);
    (void)thread_id;
    
    while (!render_jobs.shutdown)
    {
        SDL_LockMutex(render_jobs.queue_mutex);
        
        // Wait for work
        while (!render_jobs.pending_queue && !render_jobs.shutdown && render_jobs.frame_active)
        {
            SDL_CondWait(render_jobs.job_condition, render_jobs.queue_mutex);
        }
        
        if (render_jobs.shutdown || !render_jobs.frame_active)
        {
            SDL_UnlockMutex(render_jobs.queue_mutex);
            break;
        }
        
        // Get job from pending queue
        render_job_t *job = render_jobs.pending_queue;
        if (job)
            render_jobs.pending_queue = job->next;
        
        SDL_UnlockMutex(render_jobs.queue_mutex);
        
        if (!job)
            continue;
        
        // Execute the job
        if (job->func)
        {
            job->func(job->data);
        }
        
        job->complete = true;
        
        // Move to completed queue
        SDL_LockMutex(render_jobs.queue_mutex);
        job->next = render_jobs.completed_queue;
        render_jobs.completed_queue = job;
        render_jobs.jobs_completed++;
        SDL_UnlockMutex(render_jobs.queue_mutex);
    }
    
    return 0;
}

qboolean JobSystem_Init(void)
{
    memset(&render_jobs, 0, sizeof(render_jobs));
    
    render_jobs.queue_mutex = SDL_CreateMutex();
    if (!render_jobs.queue_mutex)
        return false;
    
    render_jobs.job_condition = SDL_CreateCond();
    if (!render_jobs.job_condition)
    {
        SDL_DestroyMutex(render_jobs.queue_mutex);
        return false;
    }
    
    // Initialize job pool
    for (int i = 0; i < MAX_RENDER_JOBS; i++)
    {
        render_jobs.jobs[i].next = render_jobs.free_list;
        render_jobs.free_list = &render_jobs.jobs[i];
    }
    
    render_jobs.pending_queue = NULL;
    render_jobs.completed_queue = NULL;
    
    // Create worker threads
    int num_workers = q_min(4, GetNumCPUThreads() - 1); // Leave one for main thread
    if (num_workers < 1)
        num_workers = 1;
    
    render_jobs.num_workers = num_workers;
    
    for (int i = 0; i < num_workers; i++)
    {
        int *thread_id = (int *)malloc(sizeof(int));
        *thread_id = i;
        
        char name[32];
        q_snprintf(name, sizeof(name), "RenderJob%d", i);
        
        render_jobs.worker_threads[i] = SDL_CreateThread(RenderJobWorker, name, thread_id);
        if (!render_jobs.worker_threads[i])
        {
            Con_Printf("Warning: Could not create render job thread %d\n", i);
            free(thread_id);
        }
    }
    
    Con_Printf("Job system initialized with %d workers\n", num_workers);
    return true;
}

void JobSystem_Shutdown(void)
{
    render_jobs.shutdown = true;
    render_jobs.frame_active = false;
    
    // Wake up all workers
    SDL_LockMutex(render_jobs.queue_mutex);
    SDL_CondBroadcast(render_jobs.job_condition);
    SDL_UnlockMutex(render_jobs.queue_mutex);
    
    // Wait for workers to finish
    for (int i = 0; i < render_jobs.num_workers; i++)
    {
        if (render_jobs.worker_threads[i])
        {
            SDL_WaitThread(render_jobs.worker_threads[i], NULL);
            render_jobs.worker_threads[i] = NULL;
        }
    }
    
    if (render_jobs.job_condition)
        SDL_DestroyCond(render_jobs.job_condition);
    if (render_jobs.queue_mutex)
        SDL_DestroyMutex(render_jobs.queue_mutex);
    
    memset(&render_jobs, 0, sizeof(render_jobs));
}

void JobSystem_BeginFrame(void)
{
    SDL_LockMutex(render_jobs.queue_mutex);
    
    // Reset queues
    render_jobs.pending_queue = NULL;
    render_jobs.completed_queue = NULL;
    render_jobs.jobs_submitted = 0;
    render_jobs.jobs_completed = 0;
    
    // Return all completed jobs to free list
    for (int i = 0; i < MAX_RENDER_JOBS; i++)
    {
        render_jobs.jobs[i].complete = false;
        render_jobs.jobs[i].next = render_jobs.free_list;
        render_jobs.free_list = &render_jobs.jobs[i];
    }
    
    render_jobs.frame_active = true;
    SDL_UnlockMutex(render_jobs.queue_mutex);
}

render_job_t *JobSystem_AllocJob(job_type_t type, job_func_t func, void *data)
{
    render_job_t *job = NULL;
    
    SDL_LockMutex(render_jobs.queue_mutex);
    
    if (render_jobs.free_list)
    {
        job = render_jobs.free_list;
        render_jobs.free_list = job->next;
        
        job->type = type;
        job->func = func;
        job->data = data;
        job->complete = false;
        job->priority = 0;
        job->next = NULL;
    }
    
    SDL_UnlockMutex(render_jobs.queue_mutex);
    
    return job;
}

void JobSystem_SubmitJob(render_job_t *job)
{
    if (!job)
        return;
    
    SDL_LockMutex(render_jobs.queue_mutex);
    
    // Add to pending queue
    job->next = render_jobs.pending_queue;
    render_jobs.pending_queue = job;
    render_jobs.jobs_submitted++;
    
    // Signal workers
    SDL_CondSignal(render_jobs.job_condition);
    
    SDL_UnlockMutex(render_jobs.queue_mutex);
}

void JobSystem_WaitFrame(void)
{
    while (1)
    {
        SDL_LockMutex(render_jobs.queue_mutex);
        qboolean done = (render_jobs.jobs_completed >= render_jobs.jobs_submitted);
        SDL_UnlockMutex(render_jobs.queue_mutex);
        
        if (done)
            break;
        
        // Small sleep to avoid busy waiting
        SDL_Delay(1);
    }
    
    SDL_LockMutex(render_jobs.queue_mutex);
    render_jobs.frame_active = false;
    SDL_UnlockMutex(render_jobs.queue_mutex);
}

qboolean JobSystem_IsFrameComplete(void)
{
    SDL_LockMutex(render_jobs.queue_mutex);
    qboolean done = (render_jobs.jobs_completed >= render_jobs.jobs_submitted);
    SDL_UnlockMutex(render_jobs.queue_mutex);
    return done;
}
