/*
Copyright (C) 2024-2025 Kabluk Engine Multithreading Examples

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.
*/

// mt_examples.c - Examples of using the multithreading system

#include "quakedef.h"
#include "mt_system.h"

//=============================================================================
// Пример 1: Асинхронная загрузка текстур
//=============================================================================

typedef struct
{
	char texture_name[MAX_QPATH];
	int texnum;
	qboolean loaded;
} texture_load_result_t;

static void GL_LoadTexture_Worker(void *data)
{
	texture_load_result_t *result = (texture_load_result_t *)data;
	
	// Эта функция будет выполняться в воркер потоке
	// Загружаем текстуру без блокировки основного потока
	
	// В реальной реализации здесь будет вызов GL_LoadTexture
	// result->texnum = GL_LoadTexture(result->texture_name, ...);
	
	result->loaded = true;
	result->texnum = 0; // Заглушка
	
	Con_DPrintf("Loaded texture: %s (thread)\n", result->texture_name);
}

static void GL_LoadTexture_Callback(void *result_data, void *user_data)
{
	texture_load_result_t *result = (texture_load_result_t *)result_data;
	int *out_texnum = (int *)user_data;
	
	if (result && result->loaded)
	{
		*out_texnum = result->texnum;
		Con_DPrintf("Texture callback: %s loaded successfully\n", result->texture_name);
	}
}

int MT_LoadTexture_Example(const char *name, int *texnum_out)
{
	texture_load_result_t *load_data;
	int job_id;
	
	load_data = (texture_load_result_t *)Hunk_Alloc(sizeof(texture_load_result_t), TEMP_LOW);
	strlcpy(load_data->texture_name, name, sizeof(load_data->texture_name));
	load_data->loaded = false;
	load_data->texnum = 0;
	
	job_id = MT_SubmitJob(
		JOB_LOAD_TEXTURE,
		GL_LoadTexture_Worker,
		load_data,
		GL_LoadTexture_Callback,
		texnum_out,
		2, // priority
		name
	);
	
	return job_id;
}

//=============================================================================
// Пример 2: Асинхронная загрузка моделей
//=============================================================================

typedef struct
{
	char model_name[MAX_QPATH];
	model_t *model;
	qboolean loaded;
} model_load_result_t;

static void Mod_LoadModel_Worker(void *data)
{
	model_load_result_t *result = (model_load_result_t *)data;
	
	// Загружаем модель в отдельном потоке
	// В реальности: result->model = Mod_ForName(result->model_name, false, true);
	
	result->loaded = true;
	result->model = NULL; // Заглушка
	
	Con_DPrintf("Loaded model: %s (thread)\n", result->model_name);
}

static void Mod_LoadModel_Callback(void *result_data, void *user_data)
{
	model_load_result_t *result = (model_load_result_t *)result_data;
	model_t **out_model = (model_t **)user_data;
	
	if (result && result->loaded)
	{
		*out_model = result->model;
		Con_DPrintf("Model callback: %s ready\n", result->model_name);
	}
}

int MT_LoadModel_Example(const char *name, model_t **model_out)
{
	model_load_result_t *load_data;
	
	load_data = (model_load_result_t *)Hunk_Alloc(sizeof(model_load_result_t), TEMP_LOW);
	strlcpy(load_data->model_name, name, sizeof(load_data->model_name));
	load_data->loaded = false;
	load_data->model = NULL;
	
	return MT_SubmitJob(
		JOB_LOAD_MODEL,
		Mod_LoadModel_Worker,
		load_data,
		Mod_LoadModel_Callback,
		model_out,
		2,
		name
	);
}

//=============================================================================
// Пример 3: Параллельное вычисление освещения
//=============================================================================

typedef struct
{
	int start_light;
	int end_light;
	float *output;
	void *dlight_data;
} light_calc_data_t;

static void R_CalculateLights_Worker(void *data)
{
	light_calc_data_t *calc = (light_calc_data_t *)data;
	int i;
	
	// Вычисляем освещение для диапазона источников света
	for (i = calc->start_light; i < calc->end_light; i++)
	{
		// Здесь была бы логика из gl_rlight.c
		// R_AnimateLight(), R_DynamicLight() и т.д.
		
		if (calc->output)
			calc->output[i] = 1.0f; // Заглушка
	}
	
	Con_DPrintf("Calculated lights %d-%d (thread)\n", calc->start_light, calc->end_light);
}

void MT_CalculateLights_Example(int num_lights, float *output)
{
	int num_workers = MT_GetNumWorkers();
	int lights_per_worker = (num_lights + num_workers - 1) / num_workers;
	int i;
	
	if (num_lights <= 0 || !output)
		return;
	
	// Разделяем работу между воркерами
	for (i = 0; i < num_workers && i * lights_per_worker < num_lights; i++)
	{
		light_calc_data_t *data;
		int start = i * lights_per_worker;
		int end = min((i + 1) * lights_per_worker, num_lights);
		
		if (start >= num_lights)
			break;
		
		data = (light_calc_data_t *)Hunk_Alloc(sizeof(light_calc_data_t), TEMP_LOW);
		data->start_light = start;
		data->end_light = end;
		data->output = output;
		data->dlight_data = NULL;
		
		MT_SubmitJob(
			JOB_LIGHT_CALCULATION,
			R_CalculateLights_Worker,
			data,
			NULL, // no callback needed
			NULL,
			3, // высокий приоритет для рендеринга
			"CalcLights"
		);
	}
	
	// Ждем завершения всех вычислений освещения
	MT_WaitForAllJobs(0.1); // 100ms timeout
}

//=============================================================================
// Пример 4: Физика в отдельном потоке
//=============================================================================

// Эта функция должна быть интегрирована с SV_Physics()
void MT_PhysicsStep_Example(void)
{
	static qboolean physics_initialized = false;
	
	if (!physics_initialized)
	{
		MT_StartPhysicsThread();
		physics_initialized = true;
	}
	
	// Физический поток уже работает отдельно
	// Основной поток может продолжать рендеринг
	
	// При необходимости можно приостановить физику
	// MT_PausePhysics(true);
	// ... сделать что-то ...
	// MT_PausePhysics(false);
}

//=============================================================================
// Пример 5: Job System для рендеринга
//=============================================================================

typedef struct
{
	entity_t *entities;
	int start_entity;
	int end_entity;
	void *render_data;
} render_batch_data_t;

static void R_RenderEntities_Worker(void *data)
{
	render_batch_data_t *batch = (render_batch_data_t *)data;
	int i;
	
	// Рендерим диапазон сущностей
	for (i = batch->start_entity; i < batch->end_entity; i++)
	{
		entity_t *ent = &batch->entities[i];
		
		if (!ent->model)
			continue;
		
		// Здесь был бы вызов R_DrawEntitiesOnList()
		// Но в отдельном потоке нужно быть осторожным с OpenGL контекстом
		
		// Для OpenGL Core Profile можно использовать indirect drawing
		// или собирать команды рендеринга в буфер
	}
	
	Con_DPrintf("Rendered entities %d-%d (thread)\n", batch->start_entity, batch->end_entity);
}

void MT_RenderScene_Example(void)
{
	int i, num_entities;
	int num_batches;
	
	// Получаем количество сущностей из cl_entities
	num_entities = cl.num_entities;
	
	if (num_entities <= 0)
		return;
	
	// Разбиваем на батчи для параллельного рендеринга
	num_batches = MT_GetNumWorkers();
	
	for (i = 0; i < num_batches; i++)
	{
		render_batch_data_t *data;
		int start = (i * num_entities) / num_batches;
		int end = ((i + 1) * num_entities) / num_batches;
		
		if (start >= num_entities)
			break;
		
		data = (render_batch_data_t *)Hunk_Alloc(sizeof(render_batch_data_t), TEMP_LOW);
		data->entities = cl_entities;
		data->start_entity = start;
		data->end_entity = end;
		data->render_data = NULL;
		
		// Отправляем на выполнение
		MT_SubmitJob(
			JOB_CUSTOM,
			R_RenderEntities_Worker,
			data,
			NULL,
			NULL,
			3, // высокий приоритет
			"RenderEnts"
		);
	}
	
	// Ждем завершения рендеринга перед SwapBuffers
	MT_WaitForAllJobs(0.016); // ~60fps frame time
}

//=============================================================================
// Пример 6: Интеграция с основным циклом
//=============================================================================

void MT_MainFrame_Example(void)
{
	static qboolean mt_initialized = false;
	char stats[512];
	
	// Инициализируем при первом запуске
	if (!mt_initialized)
	{
		if (MT_Init())
		{
			mt_initialized = true;
			Con_Printf("Multithreading enabled\n");
		}
	}
	
	if (!mt_initialized)
		return;
	
	// === Начало кадра ===
	
	// 1. Отправляем загрузку ресурсов асинхронно
	// MT_LoadTexture_Example("textures/wall1", &texnum);
	// MT_LoadModel_Example("maps/start.bsp", &worldmodel);
	
	// 2. Вычисляем освещение параллельно
	// MT_CalculateLights_Example(num_lights, light_data);
	
	// 3. Обновляем частицы в фоне
	// MT_SubmitJob(JOB_PARTICLE_UPDATE, CL_UpdateParticles, NULL, NULL, NULL, 1, "Particles");
	
	// 4. Физика работает в отдельном потоке постоянно
	// MT_PhysicsStep_Example();
	
	// 5. Рендерим сцены параллельно
	// MT_RenderScene_Example();
	
	// 6. Ждем завершения критических джобов перед презентацией
	// MT_WaitForFrame();
	
	// === Конец кадра ===
	
	// Печатаем статистику по F4
	if (key_dest == key_console)
	{
		MT_GetStats(stats, sizeof(stats));
		Con_DPrintf("%s", stats);
	}
}

//=============================================================================
// Пример 7: Консольные команды для отладки
//=============================================================================

static void MT_Stats_f(void)
{
	char stats[1024];
	MT_GetStats(stats, sizeof(stats));
	Con_Printf("%s", stats);
}

static void MT_Benchmark_f(void)
{
	int i, num_jobs = 100;
	double start, end;
	
	Con_Printf("Running multithreading benchmark (%d jobs)...\n", num_jobs);
	
	start = Sys_DoubleTime();
	
	for (i = 0; i < num_jobs; i++)
	{
		MT_SubmitJob(JOB_CUSTOM, NULL, NULL, NULL, NULL, 1, "Benchmark");
	}
	
	MT_WaitForAllJobs(5.0); // 5 second timeout
	
	end = Sys_DoubleTime();
	
	Con_Printf("Completed %d jobs in %.3f seconds (%.1f jobs/sec)\n",
	           num_jobs, end - start, num_jobs / (end - start));
	Con_Printf("Average job time: %.4f ms\n", MT_GetAverageJobTime() * 1000);
}

static void MT_TestLoad_f(void)
{
	const char *test_texture = "gfx/palette.lmp";
	int texnum = -1;
	
	Con_Printf("Testing async texture load: %s\n", test_texture);
	
	MT_LoadTexture_Example(test_texture, &texnum);
	
	// Ждем загрузки
	MT_WaitForAllJobs(1.0);
	
	if (texnum >= 0)
		Con_Printf("Texture loaded successfully: %d\n", texnum);
	else
		Con_Printf("Texture load pending (async)\n");
}

void MT_InitCommands_Example(void)
{
	Cmd_AddCommand("mt_stats", MT_Stats_f, "Show multithreading statistics");
	Cmd_AddCommand("mt_benchmark", MT_Benchmark_f, "Run multithreading benchmark");
	Cmd_AddCommand("mt_testload", MT_TestLoad_f, "Test async resource loading");
	Cmd_AddCommand("mt_workers", MT_Stats_f, "Show worker thread status");
}

//=============================================================================
// Пример 8: Демонстрация использования в разных модулях
//=============================================================================

// В cl_main.c - загрузка карты
void CL_BeginLevelLoading_f(void)
{
	// Вместо блокирующей загрузки:
	// Mod_ForName(cl.model_name, false, true);
	
	// Используем асинхронную загрузку:
	// MT_LoadModel_Example(cl.model_name, &cl.model_precache[0]);
	
	Con_Printf("Level loading started (async)...\n");
}

// В r_main.c - рендеринг кадра
void R_RenderView(void)
{
	// Перед рендерингом отправляем джобы
	// MT_CalculateLights_Example(...);
	// MT_RenderScene_Example();
	
	// Ждем только если нужно (для SwapBuffers)
	// MT_WaitForFrame();
	
	// Продолжаем рендеринг...
}

// В sv_phys.c - физика
void SV_Physics(void)
{
	// Если физика в отдельном потоке:
	if (MT_IsPhysicsRunning())
	{
		// Физика уже считается в фоне
		return;
	}
	
	// Иначе делаем синхронно (fallback)
	// SV_RunPhysics();
}

// В snd_main.c - загрузка звуков
sfx_t *S_PrecacheSound(const char *name)
{
	sfx_t *sfx;
	
	// Стандартная загрузка
	sfx = S_PrecacheSound_Original(name);
	
	// ИЛИ асинхронная версия:
	// MT_LoadSoundAsync(name, NULL, NULL);
	
	return sfx;
}
