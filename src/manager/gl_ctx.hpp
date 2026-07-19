#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <GL/gl.h>

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE 0x809D
#endif

#ifndef GLchar_DEFINED
#define GLchar_DEFINED
typedef char GLchar;
#endif

namespace gl_ctx
{
	struct Window
	{
		HWND hwnd = nullptr;
		HDC hdc = nullptr;
		HGLRC hglrc = nullptr;
		int width = 0;
		int height = 0;
		bool quit = false;
	};

	bool create(Window &w, const wchar_t *title, int width, int height);
	void destroy(Window &w);

	void pump(Window &w);
	void present(Window &w);

	struct Funcs
	{
		void(APIENTRY *GenFramebuffers)(GLsizei, GLuint *);
		void(APIENTRY *BindFramebuffer)(GLenum, GLuint);
		void(APIENTRY *FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint,
		                                     GLint);
		GLenum(APIENTRY *CheckFramebufferStatus)(GLenum);
		void(APIENTRY *DeleteFramebuffers)(GLsizei, const GLuint *);

		GLuint(APIENTRY *CreateShader)(GLenum);
		void(APIENTRY *ShaderSource)(GLuint, GLsizei, const GLchar *const *,
		                             const GLint *);
		void(APIENTRY *CompileShader)(GLuint);
		void(APIENTRY *GetShaderiv)(GLuint, GLenum, GLint *);
		void(APIENTRY *GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
		void(APIENTRY *DeleteShader)(GLuint);

		GLuint(APIENTRY *CreateProgram)(void);
		void(APIENTRY *AttachShader)(GLuint, GLuint);
		void(APIENTRY *LinkProgram)(GLuint);
		void(APIENTRY *GetProgramiv)(GLuint, GLenum, GLint *);
		void(APIENTRY *GetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
		void(APIENTRY *DeleteProgram)(GLuint);
		void(APIENTRY *UseProgram)(GLuint);

		GLint(APIENTRY *GetUniformLocation)(GLuint, const GLchar *);
		void(APIENTRY *Uniform1i)(GLint, GLint);
		void(APIENTRY *Uniform1f)(GLint, GLfloat);
		void(APIENTRY *Uniform2f)(GLint, GLfloat, GLfloat);

		void(APIENTRY *GenVertexArrays)(GLsizei, GLuint *);
		void(APIENTRY *BindVertexArray)(GLuint);
		void(APIENTRY *DeleteVertexArrays)(GLsizei, const GLuint *);

		void(APIENTRY *ActiveTexture)(GLenum);
	};

	extern Funcs gl;
}  // namespace gl_ctx
