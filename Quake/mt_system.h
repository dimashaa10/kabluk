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

// mt_system.h - Multithreading System Header

#ifndef __MT_SYSTEM_H__
#define __MT_SYSTEM_H__

#include "qcommon.h"
#include <SDL.h>

#define MAX_WORKER_THREADS 16
#define MAX_JOBS_PER_FRAME 256
#define JOB_QUEUE_SIZE 1024

// Типы джобов
typedef enum
{
	JOB_NONE = 0,
	JOB_LOAD_TEXTURE,
	JOB_LOAD_MODEL,
	JOB_LOAD_SOUND,
	JOB_LIGHT_CALCULATION,
	JOB_PHYSICS_STEP,
	JOB_PARTICLE_UPDATE,
	JOB_DEMO_RECORDING,
	JOB_CUSTOM
} job_type_t;

// Структура джоба
typedef struct job_s
{
	job_type_t type;
	void (*func)(void *data);
	void *data;
	void *user_data;
	void (*callback)(void *result, void *user_data);
	void *result;
	qboolean completed;
	qboolean cancelled;
	int priority; // 0 = низкий, 3 = высокий
	const char *name;
	double start_time;
	double end_time;
} job_t;

// Очередь джобов
typedef struct
{
	job_t jobs[JOB_QUEUE_SIZE];
	int head;
	int tail;
	int count;
	SDL_mutex *mutex;
	SDL_cond *cond;
} job_queue_t;

// Воркер поток
typedef struct
{
	int id;
	SDL_Thread *thread;
	qboolean running;
	qboolean idle;
	job_t *current_job;
	int jobs_completed;
	double total_work_time;
} worker_thread_t;

// Менеджер потоков
typedef struct
{
	qboolean initialized;
	
	// Воркеры
	worker_thread_t workers[MAX_WORKER_THREADS];
	int num_workers;
	
	// Очереди
	job_queue_t job_queue;
	job_queue_t high_priority_queue;
	
	// Синхронизация
	SDL_mutex *manager_mutex;
	SDL_cond *completion_cond;
	
	// Статистика
	int total_jobs_submitted;
	int total_jobs_completed;
	double total_wait_time;
	
	// Физический поток
	SDL_Thread *physics_thread;
	qboolean physics_running;
	qboolean physics_paused;
	
	// Загрузочный поток
	SDL_Thread *loader_thread;
	qboolean loader_running;
	job_queue_t loader_queue;
	
	// Frame synchronization
	volatile int frame_number;
	volatile qboolean frame_complete;
	SDL_mutex *frame_mutex;
	SDL_cond *frame_cond;
	
} mt_manager_t;

// Глобальный менеджер
extern mt_manager_t g_mt_manager;

// Инициализация/деинициализация
qboolean MT_Init(void);
void MT_Shutdown(void);

// Отправка джобов
int MT_SubmitJob(job_type_t type, void (*func)(void *), void *data, 
                 void (*callback)(void *, void *), void *user_data, 
                 int priority, const char *name);
int MT_SubmitJobImmediate(job_type_t type, void (*func)(void *), void *data,
                          const char *name);

// Ожидание завершения
qboolean MT_WaitForJob(int job_id, double timeout);
qboolean MT_WaitForAllJobs(double timeout);
void MT_WaitForFrame(void);

// Управление физическим потоком
void MT_StartPhysicsThread(void);
void MT_StopPhysicsThread(void);
void MT_PausePhysics(qboolean pause);
qboolean MT_IsPhysicsRunning(void);

// Асинхронная загрузка
int MT_LoadTextureAsync(const char *name, void (*callback)(void *, void *), void *user_data);
int MT_LoadModelAsync(const char *name, void (*callback)(void *, void *), void *user_data);
int MT_LoadSoundAsync(const char *name, void (*callback)(void *, void *), void *user_data);

// Утилиты
int MT_GetNumWorkers(void);
int MT_GetActiveJobs(void);
int MT_GetCompletedJobs(void);
double MT_GetAverageJobTime(void);
void MT_GetStats(char *buffer, int size);

// Макросы для удобства
#define MT_SUBMIT_JOB(func, data, name) \
	MT_SubmitJob(JOB_CUSTOM, func, data, NULL, NULL, 1, name)

#define MT_SUBMIT_HIGH_PRIORITY(func, data, name) \
	MT_SubmitJob(JOB_CUSTOM, func, data, NULL, NULL, 3, name)

#endif // __MT_SYSTEM_H__
