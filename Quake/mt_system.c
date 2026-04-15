/*
Copyright (C) 2024-2025 Kabluk Engine Multithreading System

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.
*/

// mt_system.c - Multithreading System Implementation

#include "quakedef.h"
#include "mt_system.h"
#include <SDL.h>

#ifdef _WIN32
#include <windows.h>
#endif

// Глобальный менеджер
mt_manager_t g_mt_manager;

// Локальные функции
static int MT_WorkerThread(void *data);
static int MT_PhysicsThread(void *data);
static int MT_LoaderThread(void *data);
static qboolean MT_PopJob(job_queue_t *queue, job_t *job);
static void MT_PushJob(job_queue_t *queue, job_t *job);
static void MT_ExecuteJob(job_t *job);
static double MT_GetTime(void);

//=============================================================================
// Инициализация и деинициализация
//=============================================================================

qboolean MT_Init(void)
{
	int i, num_threads;
	const char *override;
	
	if (g_mt_manager.initialized)
		return true;
	
	memset(&g_mt_manager, 0, sizeof(g_mt_manager));
	
	// Определяем количество потоков
	override = Sys_GetString("mt_threads");
	if (override && Q_atoi(override) > 0)
		num_threads = Q_atoi(override);
	else
		num_threads = SDL_GetCPUCount();
	
	// Ограничиваем максимум
	if (num_threads > MAX_WORKER_THREADS)
		num_threads = MAX_WORKER_THREADS;
	if (num_threads < 1)
		num_threads = 1;
	
	g_mt_manager.num_workers = num_threads;
	Con_Printf("MT_Init: Creating %d worker threads\n", num_threads);
	
	// Создаем мьютексы и кондиции
	g_mt_manager.manager_mutex = SDL_CreateMutex();
	if (!g_mt_manager.manager_mutex)
	{
		Con_Printf("MT_Init: Failed to create manager mutex!\n");
		return false;
	}
	
	g_mt_manager.completion_cond = SDL_CreateCond();
	if (!g_mt_manager.completion_cond)
	{
		Con_Printf("MT_Init: Failed to create completion condition!\n");
		SDL_DestroyMutex(g_mt_manager.manager_mutex);
		return false;
	}
	
	g_mt_manager.frame_mutex = SDL_CreateMutex();
	g_mt_manager.frame_cond = SDL_CreateCond();
	
	// Инициализируем очереди
	g_mt_manager.job_queue.head = 0;
	g_mt_manager.job_queue.tail = 0;
	g_mt_manager.job_queue.count = 0;
	g_mt_manager.job_queue.mutex = SDL_CreateMutex();
	g_mt_manager.job_queue.cond = SDL_CreateCond();
	
	g_mt_manager.high_priority_queue.head = 0;
	g_mt_manager.high_priority_queue.tail = 0;
	g_mt_manager.high_priority_queue.count = 0;
	g_mt_manager.high_priority_queue.mutex = SDL_CreateMutex();
	g_mt_manager.high_priority_queue.cond = SDL_CreateCond();
	
	g_mt_manager.loader_queue.head = 0;
	g_mt_manager.loader_queue.tail = 0;
	g_mt_manager.loader_queue.count = 0;
	g_mt_manager.loader_queue.mutex = SDL_CreateMutex();
	g_mt_manager.loader_queue.cond = SDL_CreateCond();
	
	// Создаем воркер потоки
	for (i = 0; i < num_threads; i++)
	{
		g_mt_manager.workers[i].id = i;
		g_mt_manager.workers[i].running = true;
		g_mt_manager.workers[i].idle = true;
		g_mt_manager.workers[i].current_job = NULL;
		g_mt_manager.workers[i].jobs_completed = 0;
		g_mt_manager.workers[i].total_work_time = 0;
		
		char name[32];
		q_snprintf(name, sizeof(name), "Worker-%d", i);
		
		g_mt_manager.workers[i].thread = SDL_CreateThread(MT_WorkerThread, name, &g_mt_manager.workers[i]);
		if (!g_mt_manager.workers[i].thread)
		{
			Con_Printf("MT_Init: Failed to create worker thread %d!\n", i);
			g_mt_manager.workers[i].running = false;
		}
		else
		{
			Con_DPrintf("MT_Init: Worker %d created (thread id: %p)\n", i, (void*)g_mt_manager.workers[i].thread);
		}
	}
	
	// Запускаем поток загрузчика
	g_mt_manager.loader_running = true;
	g_mt_manager.loader_thread = SDL_CreateThread(MT_LoaderThread, "LoaderThread", NULL);
	
	g_mt_manager.initialized = true;
	
	Con_Printf("MT_Init: Multithreading system initialized successfully\n");
	return true;
}

void MT_Shutdown(void)
{
	int i;
	
	if (!g_mt_manager.initialized)
		return;
	
	Con_Printf("MT_Shutdown: Shutting down multithreading system...\n");
	
	// Останавливаем воркеры
	for (i = 0; i < g_mt_manager.num_workers; i++)
	{
		g_mt_manager.workers[i].running = false;
	}
	
	// Будим все потоки
	SDL_CondBroadcast(g_mt_manager.job_queue.cond);
	SDL_CondBroadcast(g_mt_manager.high_priority_queue.cond);
	
	// Ждем завершения воркеров
	for (i = 0; i < g_mt_manager.num_workers; i++)
	{
		if (g_mt_manager.workers[i].thread)
		{
			SDL_WaitThread(g_mt_manager.workers[i].thread, NULL);
			g_mt_manager.workers[i].thread = NULL;
		}
	}
	
	// Останавливаем загрузчик
	g_mt_manager.loader_running = false;
	SDL_CondBroadcast(g_mt_manager.loader_queue.cond);
	
	if (g_mt_manager.loader_thread)
	{
		SDL_WaitThread(g_mt_manager.loader_thread, NULL);
		g_mt_manager.loader_thread = NULL;
	}
	
	// Останавливаем физику
	MT_StopPhysicsThread();
	
	// Освобождаем ресурсы
	SDL_DestroyCond(g_mt_manager.completion_cond);
	SDL_DestroyMutex(g_mt_manager.manager_mutex);
	SDL_DestroyCond(g_mt_manager.job_queue.cond);
	SDL_DestroyMutex(g_mt_manager.job_queue.mutex);
	SDL_DestroyCond(g_mt_manager.high_priority_queue.cond);
	SDL_DestroyMutex(g_mt_manager.high_priority_queue.mutex);
	SDL_DestroyCond(g_mt_manager.loader_queue.cond);
	SDL_DestroyMutex(g_mt_manager.loader_queue.mutex);
	SDL_DestroyCond(g_mt_manager.frame_cond);
	SDL_DestroyMutex(g_mt_manager.frame_mutex);
	
	memset(&g_mt_manager, 0, sizeof(g_mt_manager));
	
	Con_Printf("MT_Shutdown: Complete\n");
}

//=============================================================================
// Воркер поток
//=============================================================================

static int MT_WorkerThread(void *data)
{
	worker_thread_t *worker = (worker_thread_t *)data;
	job_t job;
	double start_time, end_time;
	
	Con_DPrintf("Worker %d started\n", worker->id);
	
	while (worker->running)
	{
		worker->idle = true;
		
		// Пробуем получить джоб из высокоприоритетной очереди
		if (!MT_PopJob(&g_mt_manager.high_priority_queue, &job))
		{
			// Если нет, берем из обычной очереди
			if (!MT_PopJob(&g_mt_manager.job_queue, &job))
			{
				// Ждем уведомления
				SDL_mutex *wait_mutex = g_mt_manager.high_priority_queue.mutex;
				SDL_cond *wait_cond = g_mt_manager.high_priority_queue.cond;
				
				SDL_LockMutex(wait_mutex);
				SDL_CondWaitTimeout(wait_cond, wait_mutex, 100); // 100ms timeout
				SDL_UnlockMutex(wait_mutex);
				continue;
			}
		}
		
		if (job.cancelled)
			continue;
		
		worker->idle = false;
		worker->current_job = &job;
		
		start_time = MT_GetTime();
		job.start_time = start_time;
		
		// Выполняем джоб
		MT_ExecuteJob(&job);
		
		end_time = MT_GetTime();
		job.end_time = end_time;
		
		worker->jobs_completed++;
		worker->total_work_time += (end_time - start_time);
		
		// Обновляем статистику
		SDL_LockMutex(g_mt_manager.manager_mutex);
		g_mt_manager.total_jobs_completed++;
		SDL_CondSignal(g_mt_manager.completion_cond);
		SDL_UnlockMutex(g_mt_manager.manager_mutex);
		
		// Вызываем callback если есть
		if (job.callback && job.result)
		{
			job.callback(job.result, job.user_data);
		}
		
		worker->current_job = NULL;
	}
	
	Con_DPrintf("Worker %d shutting down (completed %d jobs)\n", worker->id, worker->jobs_completed);
	return 0;
}

//=============================================================================
// Поток загрузчика ресурсов
//=============================================================================

static int MT_LoaderThread(void *data)
{
	job_t job;
	
	Con_Printf("Loader thread started\n");
	
	while (g_mt_manager.loader_running)
	{
		if (!MT_PopJob(&g_mt_manager.loader_queue, &job))
		{
			SDL_LockMutex(g_mt_manager.loader_queue.mutex);
			SDL_CondWaitTimeout(g_mt_manager.loader_queue.cond, 
			                    g_mt_manager.loader_queue.mutex, 100);
			SDL_UnlockMutex(g_mt_manager.loader_queue.mutex);
			continue;
		}
		
		if (job.cancelled)
			continue;
		
		job.start_time = MT_GetTime();
		MT_ExecuteJob(&job);
		job.end_time = MT_GetTime();
		
		if (job.callback && job.result)
		{
			job.callback(job.result, job.user_data);
		}
	}
	
	Con_Printf("Loader thread shutting down\n");
	return 0;
}

//=============================================================================
// Физический поток
//=============================================================================

static int MT_PhysicsThread(void *data)
{
	double last_time, current_time, delta;
	const double fixed_timestep = 1.0 / 60.0; // 60 Hz физика
	
	Con_Printf("Physics thread started (60 Hz)\n");
	
	last_time = MT_GetTime();
	
	while (g_mt_manager.physics_running)
	{
		current_time = MT_GetTime();
		delta = current_time - last_time;
		
		if (!g_mt_manager.physics_paused && delta >= fixed_timestep)
		{
			// Выполняем шаг физики
			// SV_Physics() будет вызван здесь в отдельном потоке
			// Это требует синхронизации с основным потоком
			
			// Временно блокируем доступ к физическим объектам
			// TODO: Интеграция с SV_Physics()
			
			last_time = current_time;
		}
		
		// Небольшой сон чтобы не грузить CPU
		SDL_Delay(1);
	}
	
	Con_Printf("Physics thread shutting down\n");
	return 0;
}

void MT_StartPhysicsThread(void)
{
	if (g_mt_manager.physics_running)
		return;
	
	g_mt_manager.physics_running = true;
	g_mt_manager.physics_paused = false;
	g_mt_manager.physics_thread = SDL_CreateThread(MT_PhysicsThread, "PhysicsThread", NULL);
	
	if (!g_mt_manager.physics_thread)
	{
		Con_Printf("MT_StartPhysicsThread: Failed to create physics thread!\n");
		g_mt_manager.physics_running = false;
	}
	else
	{
		Con_Printf("MT_StartPhysicsThread: Physics thread started\n");
	}
}

void MT_StopPhysicsThread(void)
{
	if (!g_mt_manager.physics_running)
		return;
	
	g_mt_manager.physics_running = false;
	
	if (g_mt_manager.physics_thread)
	{
		SDL_WaitThread(g_mt_manager.physics_thread, NULL);
		g_mt_manager.physics_thread = NULL;
	}
	
	Con_Printf("MT_StopPhysicsThread: Physics thread stopped\n");
}

void MT_PausePhysics(qboolean pause)
{
	g_mt_manager.physics_paused = pause;
}

qboolean MT_IsPhysicsRunning(void)
{
	return g_mt_manager.physics_running && !g_mt_manager.physics_paused;
}

//=============================================================================
// Управление джобами
//=============================================================================

int MT_SubmitJob(job_type_t type, void (*func)(void *), void *data,
                 void (*callback)(void *, void *), void *user_data,
                 int priority, const char *name)
{
	job_t job;
	job_queue_t *queue;
	int job_id;
	
	if (!g_mt_manager.initialized)
	{
		// Если многопоточность не инициализирована, выполняем сразу
		if (func)
			func(data);
		if (callback && data)
			callback(data, user_data);
		return -1;
	}
	
	memset(&job, 0, sizeof(job));
	job.type = type;
	job.func = func;
	job.data = data;
	job.callback = callback;
	job.user_data = user_data;
	job.completed = false;
	job.cancelled = false;
	job.priority = priority;
	job.name = name ? name : "unnamed";
	job.start_time = 0;
	job.end_time = 0;
	
	// Выбираем очередь
	queue = (priority >= 3) ? &g_mt_manager.high_priority_queue : &g_mt_manager.job_queue;
	
	// Добавляем в очередь
	MT_PushJob(queue, &job);
	
	// Обновляем статистику
	SDL_LockMutex(g_mt_manager.manager_mutex);
	g_mt_manager.total_jobs_submitted++;
	job_id = g_mt_manager.total_jobs_submitted;
	SDL_UnlockMutex(g_mt_manager.manager_mutex);
	
	// Будим один поток
	SDL_CondSignal(queue->cond);
	
	return job_id;
}

int MT_SubmitJobImmediate(job_type_t type, void (*func)(void *), void *data,
                          const char *name)
{
	return MT_SubmitJob(type, func, data, NULL, NULL, 3, name);
}

static qboolean MT_PopJob(job_queue_t *queue, job_t *job)
{
	qboolean result = false;
	
	SDL_LockMutex(queue->mutex);
	
	if (queue->count > 0)
	{
		*job = queue->jobs[queue->head];
		queue->head = (queue->head + 1) % JOB_QUEUE_SIZE;
		queue->count--;
		result = true;
	}
	
	SDL_UnlockMutex(queue->mutex);
	return result;
}

static void MT_PushJob(job_queue_t *queue, job_t *job)
{
	SDL_LockMutex(queue->mutex);
	
	if (queue->count < JOB_QUEUE_SIZE)
	{
		queue->jobs[queue->tail] = *job;
		queue->tail = (queue->tail + 1) % JOB_QUEUE_SIZE;
		queue->count++;
	}
	else
	{
		Con_Printf("MT_Warning: Job queue full!\n");
	}
	
	SDL_UnlockMutex(queue->mutex);
}

static void MT_ExecuteJob(job_t *job)
{
	if (!job || !job->func)
		return;
	
	// Выполняем функцию джоба
	job->func(job->data);
	job->completed = true;
}

//=============================================================================
// Ожидание завершения
//=============================================================================

qboolean MT_WaitForJob(int job_id, double timeout)
{
	// TODO: Реализовать отслеживание конкретных джобов
	// Пока просто ждем немного
	if (timeout > 0)
	{
		SDL_Delay((Uint32)(timeout * 1000));
	}
	return true;
}

qboolean MT_WaitForAllJobs(double timeout)
{
	double start_time = MT_GetTime();
	int pending_jobs;
	
	if (!g_mt_manager.initialized)
		return true;
	
	SDL_LockMutex(g_mt_manager.manager_mutex);
	
	while (true)
	{
		pending_jobs = g_mt_manager.job_queue.count + 
		               g_mt_manager.high_priority_queue.count +
		               g_mt_manager.loader_queue.count;
		
		if (pending_jobs == 0)
		{
			SDL_UnlockMutex(g_mt_manager.manager_mutex);
			return true;
		}
		
		if (timeout > 0 && (MT_GetTime() - start_time) >= timeout)
		{
			SDL_UnlockMutex(g_mt_manager.manager_mutex);
			return false;
		}
		
		// Ждем сигнала о завершении
		SDL_CondWaitTimeout(g_mt_manager.completion_cond, 
		                    g_mt_manager.manager_mutex, 10);
	}
}

void MT_WaitForFrame(void)
{
	if (!g_mt_manager.initialized)
		return;
	
	SDL_LockMutex(g_mt_manager.frame_mutex);
	
	while (!g_mt_manager.frame_complete)
	{
		SDL_CondWaitTimeout(g_mt_manager.frame_cond,
		                    g_mt_manager.frame_mutex, 16); // ~60fps
	}
	
	g_mt_manager.frame_complete = false;
	SDL_UnlockMutex(g_mt_manager.frame_mutex);
}

//=============================================================================
// Асинхронная загрузка ресурсов
//=============================================================================

typedef struct
{
	char name[MAX_QPATH];
	void *result;
	qboolean success;
} async_load_data_t;

static void MT_LoadTextureFunc(void *data)
{
	async_load_data_t *load_data = (async_load_data_t *)data;
	// TODO: Интеграция с GL_LoadTexture
	load_data->success = true; // Заглушка
}

int MT_LoadTextureAsync(const char *name, void (*callback)(void *, void *), void *user_data)
{
	async_load_data_t *data;
	
	if (!name)
		return -1;
	
	data = (async_load_data_t *)Hunk_Alloc(sizeof(async_load_data_t), TEMP_LOW);
	strlcpy(data->name, name, sizeof(data->name));
	data->result = NULL;
	data->success = false;
	
	return MT_SubmitJob(JOB_LOAD_TEXTURE, MT_LoadTextureFunc, data, 
	                    callback, user_data, 2, "LoadTexture");
}

static void MT_LoadModelFunc(void *data)
{
	async_load_data_t *load_data = (async_load_data_t *)data;
	// TODO: Интеграция с Mod_ForName
	load_data->success = true; // Заглушка
}

int MT_LoadModelAsync(const char *name, void (*callback)(void *, void *), void *user_data)
{
	async_load_data_t *data;
	
	if (!name)
		return -1;
	
	data = (async_load_data_t *)Hunk_Alloc(sizeof(async_load_data_t), TEMP_LOW);
	strlcpy(data->name, name, sizeof(data->name));
	data->result = NULL;
	data->success = false;
	
	return MT_SubmitJob(JOB_LOAD_MODEL, MT_LoadModelFunc, data,
	                    callback, user_data, 2, "LoadModel");
}

static void MT_LoadSoundFunc(void *data)
{
	async_load_data_t *load_data = (async_load_data_t *)data;
	// TODO: Интеграция с S_PrecacheSound
	load_data->success = true; // Заглушка
}

int MT_LoadSoundAsync(const char *name, void (*callback)(void *, void *), void *user_data)
{
	async_load_data_t *data;
	
	if (!name)
		return -1;
	
	data = (async_load_data_t *)Hunk_Alloc(sizeof(async_load_data_t), TEMP_LOW);
	strlcpy(data->name, name, sizeof(data->name));
	data->result = NULL;
	data->success = false;
	
	return MT_SubmitJob(JOB_LOAD_SOUND, MT_LoadSoundFunc, data,
	                    callback, user_data, 1, "LoadSound");
}

//=============================================================================
// Статистика и утилиты
//=============================================================================

int MT_GetNumWorkers(void)
{
	return g_mt_manager.num_workers;
}

int MT_GetActiveJobs(void)
{
	if (!g_mt_manager.initialized)
		return 0;
	
	return g_mt_manager.job_queue.count + 
	       g_mt_manager.high_priority_queue.count +
	       g_mt_manager.loader_queue.count;
}

int MT_GetCompletedJobs(void)
{
	return g_mt_manager.total_jobs_completed;
}

double MT_GetAverageJobTime(void)
{
	int i;
	double total_time = 0;
	int total_jobs = 0;
	
	for (i = 0; i < g_mt_manager.num_workers; i++)
	{
		total_time += g_mt_manager.workers[i].total_work_time;
		total_jobs += g_mt_manager.workers[i].jobs_completed;
	}
	
	if (total_jobs == 0)
		return 0;
	
	return total_time / total_jobs;
}

void MT_GetStats(char *buffer, int size)
{
	int i, active_jobs, idle_workers = 0;
	
	if (!g_mt_manager.initialized)
	{
		q_snprintf(buffer, size, "Multithreading not initialized\n");
		return;
	}
	
	active_jobs = MT_GetActiveJobs();
	
	for (i = 0; i < g_mt_manager.num_workers; i++)
	{
		if (g_mt_manager.workers[i].idle)
			idle_workers++;
	}
	
	q_snprintf(buffer, size,
	           "=== Multithreading Stats ===\n"
	           "Workers: %d (%d idle)\n"
	           "Active jobs: %d\n"
	           "Total submitted: %d\n"
	           "Total completed: %d\n"
	           "Avg job time: %.4f ms\n"
	           "Physics thread: %s\n"
	           "Loader thread: %s\n",
	           g_mt_manager.num_workers, idle_workers,
	           active_jobs,
	           g_mt_manager.total_jobs_submitted,
	           g_mt_manager.total_jobs_completed,
	           MT_GetAverageJobTime() * 1000,
	           g_mt_manager.physics_running ? "running" : "stopped",
	           g_mt_manager.loader_running ? "running" : "stopped");
}

static double MT_GetTime(void)
{
	return Sys_DoubleTime();
}

//=============================================================================
// Консольные команды
//=============================================================================

void MT_InitCommands(void)
{
	// TODO: Добавить консольные команды для отладки
	// Cmd_AddCommand("mt_stats", MT_Stats_f);
	// Cmd_AddCommand("mt_benchmark", MT_Benchmark_f);
}
