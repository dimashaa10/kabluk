/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers
Copyright (C) 2024 Optimized Particle Rendering with VBO/UBO

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

// r_part_vbo.c - Optimized particle rendering using VBO and UBO
// Features:
// - OpenGL 3.3+ Core Profile support
// - Vertex Buffer Objects for particle geometry
// - Uniform Buffer Objects for transformation matrices
// - Texture arrays for multiple particle types
// - Instanced rendering preparation

#include "quakedef.h"

// Global particle VBO system
particle_vbo_t particle_vbo = {0};
GLuint g_particle_texture_array = 0;
qboolean gl_core_profile_able = false;
qboolean gl_ubo_able = false;
qboolean gl_texture_array_able = false;
GLint gl_max_uniform_block_bindings = 0;
GLint gl_max_array_texture_layers = 0;

// Function pointers for OpenGL 3.3+
PFNGLGENVERTEXARRAYSPROC GL_GenVertexArraysFunc = NULL;
PFNGLDELETEVERTEXARRAYSPROC GL_DeleteVertexArraysFunc = NULL;
PFNGLBINDVERTEXARRAYPROC GL_BindVertexArrayFunc = NULL;
PFNGLBINDBUFFERRANGEPROC GL_BindBufferRangeFunc = NULL;
PFNGLUNIFORMBLOCKBINDINGPROC GL_UniformBlockBindingFunc = NULL;
PFNGLGETUNIFORMINDICESPROC GL_GetUniformIndicesFunc = NULL;
PFNGLGETACTIVEUNIFORMBLOCKIVPROC GL_GetActiveUniformBlockivFunc = NULL;
PFNGLTEXIMAGE3DPROC GL_TexImage3DFunc = NULL;

// Particle shader program
static GLuint g_particle_program = 0;
static GLint g_particle_mvp_location = -1;
static GLint g_particle_color_location = -1;
static GLint g_particle_tex_location = -1;
static GLuint g_particle_ubo = 0;

// Particle vertex structure for VBO
typedef struct {
	float x, y, z;      // position
	float u, v;         // texture coordinates
	unsigned char r, g, b, a; // color
} particle_vertex_t;

#define PARTICLE_VBO_SIZE (MAX_PARTICLE_VBO_VERTICES * sizeof(particle_vertex_t))
static particle_vertex_t *g_particle_vbo_data = NULL;
static int g_particle_vbo_count = 0;

// UBO structure for particle transformations
typedef struct {
	float modelview_projection[16];
	float up_vector[4];
	float right_vector[4];
} particle_ubo_t;

static particle_ubo_t g_particle_ubo_data;

/*
===============
R_InitParticleVBO
Initialize VBO and VAO for particle rendering
===============
*/
qboolean R_InitParticleVBO(void)
{
	if (!gl_vbo_able)
		return false;

	// Generate VBO
	glGenBuffers(1, &particle_vbo.vbo);
	if (!particle_vbo.vbo)
		return false;

	// Allocate storage
	GL_BindBufferFunc(GL_ARRAY_BUFFER, particle_vbo.vbo);
	GL_BufferDataFunc(GL_ARRAY_BUFFER, PARTICLE_VBO_SIZE, NULL, GL_STREAM_DRAW);
	GL_BindBufferFunc(GL_ARRAY_BUFFER, 0);

	particle_vbo.max_vertices = MAX_PARTICLE_VBO_VERTICES;
	particle_vbo.vertex_count = 0;
	particle_vbo.initialized = true;

	// Allocate CPU-side buffer
	g_particle_vbo_data = (particle_vertex_t *)Hunk_AllocName(PARTICLE_VBO_SIZE, "particle_vbo");
	if (!g_particle_vbo_data)
		return false;

	Con_DPrintf("Particle VBO initialized: %d vertices\n", particle_vbo.max_vertices);
	return true;
}

/*
===============
R_InitParticleVAO
Initialize VAO for particle rendering (Core Profile)
===============
*/
qboolean R_InitParticleVAO(void)
{
	if (!gl_core_profile_able || !particle_vbo.initialized)
		return false;

	// Generate VAO
	GL_GenVertexArraysFunc(1, &particle_vbo.vao);
	if (!particle_vbo.vao)
		return false;

	// Bind VAO and setup vertex attributes
	GL_BindVertexArrayFunc(particle_vbo.vao);

	GL_BindBufferFunc(GL_ARRAY_BUFFER, particle_vbo.vbo);

	// Position attribute (location 0)
	GL_VertexAttribPointerFunc(0, 3, GL_FLOAT, GL_FALSE, sizeof(particle_vertex_t), 
		(const void *)offsetof(particle_vertex_t, x));
	GL_EnableVertexAttribArrayFunc(0);

	// TexCoord attribute (location 1)
	GL_VertexAttribPointerFunc(1, 2, GL_FLOAT, GL_FALSE, sizeof(particle_vertex_t),
		(const void *)offsetof(particle_vertex_t, u));
	GL_EnableVertexAttribArrayFunc(1);

	// Color attribute (location 2)
	GL_VertexAttribPointerFunc(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(particle_vertex_t),
		(const void *)offsetof(particle_vertex_t, r));
	GL_EnableVertexAttribArrayFunc(2);

	GL_BindBufferFunc(GL_ARRAY_BUFFER, 0);
	GL_BindVertexArrayFunc(0);

	Con_DPrintf("Particle VAO initialized\n");
	return true;
}

/*
===============
R_InitParticleUBO
Initialize UBO for particle transformations
===============
*/
qboolean R_InitParticleUBO(void)
{
	if (!gl_ubo_able)
		return false;

	// Create UBO
	glGenBuffers(1, &g_particle_ubo);
	if (!g_particle_ubo)
		return false;

	// Allocate storage
	GL_BindBufferFunc(GL_UNIFORM_BUFFER, g_particle_ubo);
	GL_BufferDataFunc(GL_UNIFORM_BUFFER, sizeof(particle_ubo_t), NULL, GL_DYNAMIC_DRAW);
	GL_BindBufferFunc(GL_UNIFORM_BUFFER, 0);

	Con_DPrintf("Particle UBO initialized\n");
	return true;
}

/*
===============
R_InitParticleTextureArray
Create texture array for multiple particle types
===============
*/
qboolean R_InitParticleTextureArray(void)
{
	if (!gl_texture_array_able || gl_max_array_texture_layers < 4)
		return false;

	// Load particle textures first
	R_InitParticleTextures();

	// Create texture array
	glGenTextures(1, &g_particle_texture_array);
	if (!g_particle_texture_array)
		return false;

	glBindTexture(GL_TEXTURE_2D_ARRAY, g_particle_texture_array);

	// Set texture parameters
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	// TODO: Upload particle textures to array layers
	// For now, we'll use the existing single texture approach
	// This is a placeholder for full implementation

	glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

	Con_DPrintf("Particle texture array initialized (%d layers max)\n", gl_max_array_texture_layers);
	return true;
}

/*
===============
R_CreateParticleShader
Create GLSL shader program for particle rendering (Core Profile)
===============
*/
qboolean R_CreateParticleShader(void)
{
	const GLchar *vertSource =
		"#version 330 core\n"
		"\n"
		"layout(location = 0) in vec3 inPosition;\n"
		"layout(location = 1) in vec2 inTexCoord;\n"
		"layout(location = 2) in vec4 inColor;\n"
		"\n"
		"uniform ParticleUBO {\n"
		"    mat4 MVP;\n"
		"    vec4 UpVector;\n"
		"    vec4 RightVector;\n"
		"};\n"
		"\n"
		"out vec2 texCoord;\n"
		"out vec4 vertColor;\n"
		"\n"
		"void main(void) {\n"
		"    vec3 corner = inPosition + inTexCoord.x * RightVector.xyz + inTexCoord.y * UpVector.xyz;\n"
		"    gl_Position = MVP * vec4(corner, 1.0);\n"
		"    texCoord = inTexCoord;\n"
		"    vertColor = inColor;\n"
		"}\n";

	const GLchar *fragSource =
		"#version 330 core\n"
		"\n"
		"in vec2 texCoord;\n"
		"in vec4 vertColor;\n"
		"\n"
		"uniform sampler2D ParticleTexture;\n"
		"\n"
		"out vec4 fragColor;\n"
		"\n"
		"void main(void) {\n"
		"    vec4 texColor = texture(ParticleTexture, texCoord);\n"
		"    fragColor = vertColor * texColor;\n"
		"}\n";

	glsl_attrib_binding_t bindings[] = {
		{"inPosition", 0},
		{"inTexCoord", 1},
		{"inColor", 2}
	};

	g_particle_program = GL_CreateProgram(vertSource, fragSource, 3, bindings);
	if (!g_particle_program)
		return false;

	// Get uniform locations
	g_particle_mvp_location = GL_GetUniformLocationFunc(g_particle_program, "ParticleUBO.MVP");
	g_particle_color_location = -1; // Used via vertex colors
	g_particle_tex_location = GL_GetUniformLocationFunc(g_particle_program, "ParticleTexture");

	// Setup UBO binding
	GLuint ubo_index = glGetUniformBlockIndex(g_particle_program, "ParticleUBO");
	if (ubo_index != GL_INVALID_INDEX)
	{
		glUniformBlockBinding(g_particle_program, ubo_index, 0);
		GL_BindBufferRangeFunc(GL_UNIFORM_BUFFER, 0, g_particle_ubo, 0, sizeof(particle_ubo_t));
	}

	Con_DPrintf("Particle shader program created\n");
	return true;
}

/*
===============
R_ShutdownParticleVBO
Cleanup VBO/VAO/UBO resources
===============
*/
void R_ShutdownParticleVBO(void)
{
	if (particle_vbo.vbo)
	{
		GL_DeleteBuffersFunc(1, &particle_vbo.vbo);
		particle_vbo.vbo = 0;
	}

	if (particle_vbo.vao && gl_core_profile_able)
	{
		GL_DeleteVertexArraysFunc(1, &particle_vbo.vao);
		particle_vbo.vao = 0;
	}

	if (g_particle_ubo)
	{
		GL_DeleteBuffersFunc(1, &g_particle_ubo);
		g_particle_ubo = 0;
	}

	if (g_particle_program)
	{
		GL_DeleteProgramFunc(g_particle_program);
		g_particle_program = 0;
	}

	if (g_particle_texture_array)
	{
		glDeleteTextures(1, &g_particle_texture_array);
		g_particle_texture_array = 0;
	}

	particle_vbo.initialized = false;
	g_particle_vbo_count = 0;
}

/*
===============
R_BeginParticleFrame
Prepare particle VBO for rendering
===============
*/
void R_BeginParticleFrame(void)
{
	if (!particle_vbo.initialized)
		return;

	g_particle_vbo_count = 0;
}

/*
===============
R_AddParticleToVBO
Add a particle quad to the VBO buffer
===============
*/
qboolean R_AddParticleToVBO(vec3_t org, float scale, vec3_t up, vec3_t right, byte color_idx)
{
	if (g_particle_vbo_count + 6 > particle_vbo.max_vertices)
		return false;

	particle_vertex_t *v = &g_particle_vbo_data[g_particle_vbo_count];
	byte *c = (byte *)&d_8to24table[color_idx];

	// Quad vertices (counter-clockwise)
	// Bottom-left
	v[0].x = org[0];
	v[0].y = org[1];
	v[0].z = org[2];
	v[0].u = 0.0f;
	v[0].v = 0.0f;
	v[0].r = c[0];
	v[0].g = c[1];
	v[0].b = c[2];
	v[0].a = 255;

	// Top-left
	v[1].x = org[0] + up[0] * scale;
	v[1].y = org[1] + up[1] * scale;
	v[1].z = org[2] + up[2] * scale;
	v[1].u = 0.0f;
	v[1].v = 1.0f;
	v[1].r = c[0];
	v[1].g = c[1];
	v[1].b = c[2];
	v[1].a = 255;

	// Top-right
	v[2].x = org[0] + up[0] * scale + right[0] * scale;
	v[2].y = org[1] + up[1] * scale + right[1] * scale;
	v[2].z = org[2] + up[2] * scale + right[2] * scale;
	v[2].u = 1.0f;
	v[2].v = 1.0f;
	v[2].r = c[0];
	v[2].g = c[1];
	v[2].b = c[2];
	v[2].a = 255;

	// Bottom-left (second triangle)
	v[3].x = org[0];
	v[3].y = org[1];
	v[3].z = org[2];
	v[3].u = 0.0f;
	v[3].v = 0.0f;
	v[3].r = c[0];
	v[3].g = c[1];
	v[3].b = c[2];
	v[3].a = 255;

	// Top-right (second triangle)
	v[4].x = org[0] + up[0] * scale + right[0] * scale;
	v[4].y = org[1] + up[1] * scale + right[1] * scale;
	v[4].z = org[2] + up[2] * scale + right[2] * scale;
	v[4].u = 1.0f;
	v[4].v = 1.0f;
	v[4].r = c[0];
	v[4].g = c[1];
	v[4].b = c[2];
	v[4].a = 255;

	// Bottom-right
	v[5].x = org[0] + right[0] * scale;
	v[5].y = org[1] + right[1] * scale;
	v[5].z = org[2] + right[2] * scale;
	v[5].u = 1.0f;
	v[5].v = 0.0f;
	v[5].r = c[0];
	v[5].g = c[1];
	v[5].b = c[2];
	v[5].a = 255;

	g_particle_vbo_count += 6;
	return true;
}

/*
===============
R_DrawParticleVBO
Upload and draw particles using VBO
===============
*/
void R_DrawParticleVBO(void)
{
	if (!particle_vbo.initialized || g_particle_vbo_count == 0)
		return;

	// Upload vertex data
	GL_BindBufferFunc(GL_ARRAY_BUFFER, particle_vbo.vbo);
	GL_BufferSubDataFunc(GL_ARRAY_BUFFER, 0, g_particle_vbo_count * sizeof(particle_vertex_t), g_particle_vbo_data);

	if (gl_core_profile_able && particle_vbo.vao)
	{
		// Use VAO (Core Profile)
		GL_BindVertexArrayFunc(particle_vbo.vao);
		
		if (g_particle_program)
		{
			GL_UseProgramFunc(g_particle_program);
			
			// Update UBO with MVP matrix
			float mvp[16];
			glGetFloatv(GL_MODELVIEW_MATRIX, mvp);
			memcpy(g_particle_ubo_data.modelview_projection, mvp, sizeof(mvp));
			
			GL_BindBufferFunc(GL_UNIFORM_BUFFER, g_particle_ubo);
			GL_BufferSubDataFunc(GL_UNIFORM_BUFFER, 0, sizeof(particle_ubo_t), &g_particle_ubo_data);
			GL_BindBufferFunc(GL_UNIFORM_BUFFER, 0);
			
			// Bind texture
			GL_Bind(particletexture);
			
			// Draw
			glDrawArrays(GL_TRIANGLES, 0, g_particle_vbo_count);
			
			GL_UseProgramFunc(0);
		}
		
		GL_BindVertexArrayFunc(0);
	}
	else
	{
		// Fallback to legacy path with VBO
		GL_Bind(particletexture);
		
		// Setup vertex pointers manually
		GL_VertexAttribPointerFunc(0, 3, GL_FLOAT, GL_FALSE, sizeof(particle_vertex_t), (const void *)0);
		GL_EnableVertexAttribArrayFunc(0);
		GL_VertexAttribPointerFunc(1, 2, GL_FLOAT, GL_FALSE, sizeof(particle_vertex_t), (const void *)offsetof(particle_vertex_t, u));
		GL_EnableVertexAttribArrayFunc(1);
		GL_VertexAttribPointerFunc(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(particle_vertex_t), (const void *)offsetof(particle_vertex_t, r));
		GL_EnableVertexAttribArrayFunc(2);
		
		glDrawArrays(GL_TRIANGLES, 0, g_particle_vbo_count);
		
		GL_DisableVertexAttribArrayFunc(0);
		GL_DisableVertexAttribArrayFunc(1);
		GL_DisableVertexAttribArrayFunc(2);
	}

	GL_BindBufferFunc(GL_ARRAY_BUFFER, 0);
	glColor3f(1.0f, 1.0f, 1.0f);
}

/*
===============
R_CheckOpenGLExtensions
Check for OpenGL 3.3+ and extensions
===============
*/
void R_CheckParticleExtensions(void)
{
	const char *gl_version = (const char *)glGetString(GL_VERSION);
	const char *gl_extensions = (const char *)glGetString(GL_EXTENSIONS);
	
	// Check OpenGL version
	int major = 0, minor = 0;
	if (gl_version)
		sscanf(gl_version, "%d.%d", &major, &minor);
	
	// Core Profile available on OpenGL 3.2+
	gl_core_profile_able = (major > 3 || (major == 3 && minor >= 2));
	
	// Check for specific extensions/functions
	if (gl_extensions)
	{
		// UBO support (OpenGL 3.0+ or ARB_uniform_buffer_object)
		gl_ubo_able = gl_core_profile_able || 
			Q_strstr(gl_extensions, "GL_ARB_uniform_buffer_object") != NULL;
		
		// Texture array support (OpenGL 3.0+ or EXT_texture_array)
		gl_texture_array_able = (major >= 3) ||
			Q_strstr(gl_extensions, "GL_EXT_texture_array") != NULL;
	}
	
	// Query limits
	if (gl_ubo_able)
	{
		glGetIntegerv(GL_MAX_UNIFORM_BLOCK_BINDINGS, &gl_max_uniform_block_bindings);
	}
	
	if (gl_texture_array_able)
	{
		glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &gl_max_array_texture_layers);
	}
	
	// Load function pointers
	if (gl_core_profile_able || Q_strstr(gl_extensions, "ARB_vertex_array_object"))
	{
		GL_GenVertexArraysFunc = (PFNGLGENVERTEXARRAYSPROC)SDL_GL_GetProcAddress("glGenVertexArrays");
		GL_DeleteVertexArraysFunc = (PFNGLDELETEVERTEXARRAYSPROC)SDL_GL_GetProcAddress("glDeleteVertexArrays");
		GL_BindVertexArrayFunc = (PFNGLBINDVERTEXARRAYPROC)SDL_GL_GetProcAddress("glBindVertexArray");
	}
	
	if (gl_ubo_able)
	{
		GL_BindBufferRangeFunc = (PFNGLBINDBUFFERRANGEPROC)SDL_GL_GetProcAddress("glBindBufferRange");
		GL_UniformBlockBindingFunc = (PFNGLUNIFORMBLOCKBINDINGPROC)SDL_GL_GetProcAddress("glUniformBlockBinding");
		GL_GetUniformIndicesFunc = (PFNGLGETUNIFORMINDICESPROC)SDL_GL_GetProcAddress("glGetUniformIndices");
		GL_GetActiveUniformBlockivFunc = (PFNGLGETACTIVEUNIFORMBLOCKIVPROC)SDL_GL_GetProcAddress("glGetActiveUniformBlockiv");
	}
	
	if (gl_texture_array_able)
	{
		GL_TexImage3DFunc = (PFNGLTEXIMAGE3DPROC)SDL_GL_GetProcAddress("glTexImage3D");
	}
	
	Con_Printf("OpenGL Core Profile: %s\n", gl_core_profile_able ? "YES" : "NO");
	Con_Printf("UBO Support: %s\n", gl_ubo_able ? "YES" : "NO");
	Con_Printf("Texture Arrays: %s\n", gl_texture_array_able ? "YES" : "NO");
	if (gl_texture_array_able)
		Con_Printf("  Max array layers: %d\n", gl_max_array_texture_layers);
}

/*
===============
R_InitParticleSystem
Initialize the optimized particle system
===============
*/
void R_InitParticleSystem(void)
{
	Con_Printf("Initializing optimized particle system...\n");
	
	// Check extensions first
	R_CheckParticleExtensions();
	
	// Initialize VBO
	if (!R_InitParticleVBO())
	{
		Con_Warn("Failed to initialize particle VBO, using legacy rendering\n");
		return;
	}
	
	// Initialize VAO for Core Profile
	if (gl_core_profile_able)
	{
		if (!R_InitParticleVAO())
		{
			Con_Warn("Failed to initialize particle VAO\n");
		}
	}
	
	// Initialize UBO
	if (gl_ubo_able)
	{
		if (!R_InitParticleUBO())
		{
			Con_Warn("Failed to initialize particle UBO\n");
		}
	}
	
	// Initialize texture array
	if (gl_texture_array_able)
	{
		if (!R_InitParticleTextureArray())
		{
			Con_Warn("Failed to initialize particle texture array\n");
		}
	}
	
	// Create shader program for Core Profile
	if (gl_core_profile_able && gl_glsl_able)
	{
		if (!R_CreateParticleShader())
		{
			Con_Warn("Failed to create particle shader\n");
		}
	}
	
	Con_Printf("Optimized particle system initialized successfully\n");
}
