/*
 * port/psp/gl_stub.c -- OpenGL entry points that do nothing.
 * * Stage M3: the engine has to link and boot before anything is drawn. * These definitions keep the renderer valid at link time; the real * implementations arrive with the sceGu shim (stage M4).  The generated * signatures are copied from SDL_opengl.h, so they match the prototype the * engine compiles against.
 */
#include "quakedef.h"
/* ---- gl* functions (71) ---- */
GLAPI void GLAPIENTRY glAlphaFunc (GLenum func, GLclampf ref) {  }
GLAPI void GLAPIENTRY glBegin (GLenum mode) {  }
GLAPI void GLAPIENTRY glBindTexture (GLenum target, GLuint texture) {  }
GLAPI void GLAPIENTRY glBlendFunc (GLenum sfactor, GLenum dfactor) {  }
GLAPI void GLAPIENTRY glClear (GLbitfield mask) {  }
GLAPI void GLAPIENTRY glClearColor (GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) {  }
GLAPI void GLAPIENTRY glColor3f (GLfloat red, GLfloat green, GLfloat blue) {  }
GLAPI void GLAPIENTRY glColor3fv (const GLfloat *v) {  }
GLAPI void GLAPIENTRY glColor4f (GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {  }
GLAPI void GLAPIENTRY glColor4fv (const GLfloat *v) {  }
GLAPI void GLAPIENTRY glColor4ubv (const GLubyte *v) {  }
GLAPI void GLAPIENTRY glColorMask (GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) {  }
GLAPI void GLAPIENTRY glColorPointer (GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {  }
GLAPI void GLAPIENTRY glCopyTexSubImage2D (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height) {  }
GLAPI void GLAPIENTRY glCullFace (GLenum mode) {  }
GLAPI void GLAPIENTRY glDeleteTextures (GLsizei n, const GLuint *textures) {  }
GLAPI void GLAPIENTRY glDepthFunc (GLenum func) {  }
GLAPI void GLAPIENTRY glDepthMask (GLboolean flag) {  }
GLAPI void GLAPIENTRY glDepthRange (GLclampd near_val, GLclampd far_val) {  }
GLAPI void GLAPIENTRY glDisable (GLenum cap) {  }
GLAPI void GLAPIENTRY glDisableClientState (GLenum cap) {  }
GLAPI void GLAPIENTRY glDrawElements (GLenum mode, GLsizei count, GLenum type, const GLvoid *indices) {  }
GLAPI void GLAPIENTRY glEnable (GLenum cap) {  }
GLAPI void GLAPIENTRY glEnableClientState (GLenum cap) {  }
GLAPI void GLAPIENTRY glEnd (void) {  }
GLAPI void GLAPIENTRY glFinish (void) {  }
GLAPI void GLAPIENTRY glFogf (GLenum pname, GLfloat param) {  }
GLAPI void GLAPIENTRY glFogfv (GLenum pname, const GLfloat *params) {  }
GLAPI void GLAPIENTRY glFogi (GLenum pname, GLint param) {  }
GLAPI void GLAPIENTRY glGenTextures (GLsizei n, GLuint *textures) {  }
GLAPI void GLAPIENTRY glGetFloatv (GLenum pname, GLfloat *params) {  }
GLAPI void GLAPIENTRY glGetIntegerv (GLenum pname, GLint *params) {  }
GLAPI void GLAPIENTRY glGetTexImage (GLenum target, GLint level, GLenum format, GLenum type, GLvoid *pixels) {  }
GLAPI void GLAPIENTRY glHint (GLenum target, GLenum mode) {  }
GLAPI GLboolean GLAPIENTRY glIsEnabled (GLenum cap) { return 0; }
GLAPI void GLAPIENTRY glLineWidth (GLfloat width) {  }
GLAPI void GLAPIENTRY glLoadIdentity (void) {  }
GLAPI void GLAPIENTRY glLoadMatrixf (const GLfloat *m) {  }
GLAPI void GLAPIENTRY glMatrixMode (GLenum mode) {  }
GLAPI void GLAPIENTRY glMultMatrixf (const GLfloat *m) {  }
GLAPI void GLAPIENTRY glOrtho (GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_val, GLdouble far_val) {  }
GLAPI void GLAPIENTRY glPixelStorei (GLenum pname, GLint param) {  }
GLAPI void GLAPIENTRY glPointSize (GLfloat size) {  }
GLAPI void GLAPIENTRY glPolygonMode (GLenum face, GLenum mode) {  }
GLAPI void GLAPIENTRY glPolygonOffset (GLfloat factor, GLfloat units) {  }
GLAPI void GLAPIENTRY glPopAttrib (void) {  }
GLAPI void GLAPIENTRY glPopMatrix (void) {  }
GLAPI void GLAPIENTRY glPushAttrib (GLbitfield mask) {  }
GLAPI void GLAPIENTRY glPushMatrix (void) {  }
GLAPI void GLAPIENTRY glReadPixels (GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels) {  }
GLAPI void GLAPIENTRY glRotatef (GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {  }
GLAPI void GLAPIENTRY glScalef (GLfloat x, GLfloat y, GLfloat z) {  }
GLAPI void GLAPIENTRY glScissor (GLint x, GLint y, GLsizei width, GLsizei height) {  }
GLAPI void GLAPIENTRY glShadeModel (GLenum mode) {  }
GLAPI void GLAPIENTRY glStencilFunc (GLenum func, GLint ref, GLuint mask) {  }
GLAPI void GLAPIENTRY glStencilMask (GLuint mask) {  }
GLAPI void GLAPIENTRY glStencilOp (GLenum fail, GLenum zfail, GLenum zpass) {  }
GLAPI void GLAPIENTRY glTexCoord2f (GLfloat s, GLfloat t) {  }
GLAPI void GLAPIENTRY glTexCoordPointer (GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {  }
GLAPI void GLAPIENTRY glTexEnvf (GLenum target, GLenum pname, GLfloat param) {  }
GLAPI void GLAPIENTRY glTexEnvi (GLenum target, GLenum pname, GLint param) {  }
GLAPI void GLAPIENTRY glTexImage2D (GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const GLvoid *pixels) {  }
GLAPI void GLAPIENTRY glTexParameterf (GLenum target, GLenum pname, GLfloat param) {  }
GLAPI void GLAPIENTRY glTexParameteri (GLenum target, GLenum pname, GLint param) {  }
GLAPI void GLAPIENTRY glTexSubImage2D (GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const GLvoid *pixels) {  }
GLAPI void GLAPIENTRY glTranslatef (GLfloat x, GLfloat y, GLfloat z) {  }
GLAPI void GLAPIENTRY glVertex2f (GLfloat x, GLfloat y) {  }
GLAPI void GLAPIENTRY glVertex3f (GLfloat x, GLfloat y, GLfloat z) {  }
GLAPI void GLAPIENTRY glVertex3fv (const GLfloat *v) {  }
GLAPI void GLAPIENTRY glVertexPointer (GLint size, GLenum type, GLsizei stride, const GLvoid *ptr) {  }
GLAPI void GLAPIENTRY glViewport (GLint x, GLint y, GLsizei width, GLsizei height) {  }

/* ---- extension pointers (49): not loaded, so nothing calls them ---- */
QS_PFNGLATTACHSHADERPROC GL_AttachShaderFunc;
/* defined in vid_psp.c: void GL_BeginRendering (int*, int*, int*, int*); */
QS_PFNGLBINDATTRIBLOCATIONFUNC GL_BindAttribLocationFunc;
PFNGLBINDBUFFERARBPROC GL_BindBufferFunc;
PFNGLBINDFRAMEBUFFERPROC GL_BindFramebufferFunc;
PFNGLBINDRENDERBUFFERPROC GL_BindRenderbufferFunc;
PFNGLBLENDFUNCSEPARATEPROC GL_BlendFuncSeparateFunc;
PFNGLBUFFERDATAARBPROC GL_BufferDataFunc;
PFNGLBUFFERSTORAGEPROC GL_BufferStorageFunc;
PFNGLCHECKFRAMEBUFFERSTATUSPROC GL_CheckFramebufferStatusFunc;
PFNGLCLIENTACTIVETEXTUREARBPROC GL_ClientActiveTextureFunc;
QS_PFNGLCOMPILESHADERPROC GL_CompileShaderFunc;
QS_PFNGLCOMPRESSEDTEXIMAGE2DPROC GL_CompressedTexImage2D;
QS_PFNGLCREATEPROGRAMPROC GL_CreateProgramFunc;
QS_PFNGLCREATESHADERPROC GL_CreateShaderFunc;
PFNGLDELETEBUFFERSARBPROC GL_DeleteBuffersFunc;
PFNGLDELETEFRAMEBUFFERSPROC GL_DeleteFramebuffersFunc;
QS_PFNGLDELETEPROGRAMPROC GL_DeleteProgramFunc;
PFNGLDELETERENDERBUFFERSPROC GL_DeleteRenderbuffersFunc;
QS_PFNGLDELETESHADERPROC GL_DeleteShaderFunc;
QS_PFNGLDISABLEVERTEXATTRIBARRAYPROC GL_DisableVertexAttribArrayFunc;
QS_PFNGLENABLEVERTEXATTRIBARRAYPROC GL_EnableVertexAttribArrayFunc;
/* defined in vid_psp.c: void GL_EndRendering (void); */
PFNGLFRAMEBUFFERRENDERBUFFERPROC GL_FramebufferRenderbufferFunc;
PFNGLFRAMEBUFFERTEXTURE2DPROC GL_FramebufferTexture2DFunc;
PFNGLGENBUFFERSARBPROC GL_GenBuffersFunc;
PFNGLGENFRAMEBUFFERSPROC GL_GenFramebuffersFunc;
PFNGLGENRENDERBUFFERSPROC GL_GenRenderbuffersFunc;
QS_PFNGLGETPROGRAMINFOLOGPROC GL_GetProgramInfoLogFunc;
QS_PFNGLGETPROGRAMIVPROC GL_GetProgramivFunc;
QS_PFNGLGETSHADERINFOLOGPROC GL_GetShaderInfoLogFunc;
QS_PFNGLGETSHADERIVPROC GL_GetShaderivFunc;
QS_PFNGLGETUNIFORMLOCATIONPROC GL_GetUniformLocationFunc;
QS_PFNGLLINKPROGRAMPROC GL_LinkProgramFunc;
PFNGLMULTITEXCOORD2FARBPROC GL_MTexCoord2fFunc;
PFNGLMAPBUFFERARBPROC GL_MapBufferFunc;
PFNGLMAPBUFFERRANGEPROC GL_MapBufferRangeFunc;
PFNGLRENDERBUFFERSTORAGEPROC GL_RenderbufferStorageFunc;
PFNGLACTIVETEXTUREARBPROC GL_SelectTextureFunc;
QS_PFNGLSHADERSOURCEPROC GL_ShaderSourceFunc;
QS_PFNGLUNIFORM1FPROC GL_Uniform1fFunc;
QS_PFNGLUNIFORM1IPROC GL_Uniform1iFunc;
QS_PFNGLUNIFORM2FPROC GL_Uniform2fFunc;
QS_PFNGLUNIFORM3FPROC GL_Uniform3fFunc;
QS_PFNGLUNIFORM4FPROC GL_Uniform4fFunc;
QS_PFNGLUNIFORM4FVPROC GL_Uniform4fvFunc;
PFNGLUNMAPBUFFERARBPROC GL_UnmapBufferFunc;
QS_PFNGLUSEPROGRAMPROC GL_UseProgramFunc;
QS_PFNGLVERTEXATTRIBPOINTERPROC GL_VertexAttribPointerFunc;
