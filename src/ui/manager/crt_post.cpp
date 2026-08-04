#include "crt_post.hpp"
#include "logs.hpp"
#include <vector>

using namespace gl_ctx;

namespace
{
	GLuint g_fbo = 0, g_tex = 0, g_prog = 0, g_vao = 0;
	int g_w = 0, g_h = 0;

	GLint u_tex = -1, u_res = -1, u_flicker = -1, u_time = -1;

	const char *kVert = R"(#version 330 core
out vec2 v_uv;
void main()
{
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

	const char *kFrag = R"(#version 330 core
in vec2 v_uv;
out vec4 o_col;

uniform sampler2D u_tex;
uniform vec2  u_res;
uniform float u_flicker;
uniform float u_time;

void main()
{
    vec2 texel = 1.0 / u_res;
    vec3 c = texture(u_tex, v_uv).rgb;

    // amber phosphor bloom: cheap 4-tap, weighted toward the orange channel
    vec3 b = texture(u_tex, v_uv + vec2( 2.0, 0.0) * texel).rgb
           + texture(u_tex, v_uv + vec2(-2.0, 0.0) * texel).rgb
           + texture(u_tex, v_uv + vec2( 0.0, 2.0) * texel).rgb
           + texture(u_tex, v_uv + vec2( 0.0,-2.0) * texel).rgb;
    b *= 0.25;
    c += b * vec3(0.16, 0.07, 0.02);

    // scanlines: 2 on / 2 off in device pixels
    float row  = v_uv.y * u_res.y;
    float scan = mod(row, 4.0) < 2.0 ? 1.0 : 0.93;

    // horizontal jitter, very subtle, keeps it from looking static
    float j = sin(u_time * 0.7) * sin(u_time * 3.1) * 0.0004;
    c = mix(c, texture(u_tex, v_uv + vec2(j, 0.0)).rgb, 0.35);

    o_col = vec4(c * scan * u_flicker, 1.0);
}
)";

	GLuint compile(GLenum type, const char *src, const char *label)
	{
		GLuint s = gl.CreateShader(type);
		gl.ShaderSource(s, 1, &src, nullptr);
		gl.CompileShader(s);

		GLint ok = 0;
		gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok)
		{
			char log[1024]{};
			gl.GetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
			log_err("crt_post: %s shader compile failed: %s", label, log);
			gl.DeleteShader(s);
			return 0;
		}
		return s;
	}

	float flicker_at(float t)
	{
		float p = fmodf(t, 9.0f) / 9.0f;
		if (p < 0.90f)
			return 1.0f;
		if (p < 0.92f)
			return 1.0f + (0.93f - 1.0f) * ((p - 0.90f) / 0.02f);
		if (p < 0.96f)
			return 0.93f + (0.97f - 0.93f) * ((p - 0.92f) / 0.04f);
		return 0.97f + (1.0f - 0.97f) * ((p - 0.96f) / 0.04f);
	}

	void resize(int w, int h)
	{
		if (w == g_w && h == g_h && g_tex)
			return;
		g_w = w;
		g_h = h;

		if (!g_tex)
			glGenTextures(1, &g_tex);
		glBindTexture(GL_TEXTURE_2D, g_tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
		             GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		if (!g_fbo)
			gl.GenFramebuffers(1, &g_fbo);
		gl.BindFramebuffer(GL_FRAMEBUFFER, g_fbo);
		gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                        GL_TEXTURE_2D, g_tex, 0);

		if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) !=
		    GL_FRAMEBUFFER_COMPLETE)
			log_err("crt_post: framebuffer incomplete (%dx%d)", w, h);

		gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
	}
}  // namespace

namespace crt_post
{
	bool init()
	{
		GLuint vs = compile(GL_VERTEX_SHADER, kVert, "vertex");
		GLuint fs = compile(GL_FRAGMENT_SHADER, kFrag, "fragment");
		if (!vs || !fs)
			return false;

		g_prog = gl.CreateProgram();
		gl.AttachShader(g_prog, vs);
		gl.AttachShader(g_prog, fs);
		gl.LinkProgram(g_prog);

		GLint ok = 0;
		gl.GetProgramiv(g_prog, GL_LINK_STATUS, &ok);
		if (!ok)
		{
			char log[1024]{};
			gl.GetProgramInfoLog(g_prog, sizeof(log) - 1, nullptr, log);
			log_err("crt_post: link failed: %s", log);
			return false;
		}
		gl.DeleteShader(vs);
		gl.DeleteShader(fs);

		u_tex = gl.GetUniformLocation(g_prog, "u_tex");
		u_res = gl.GetUniformLocation(g_prog, "u_res");
		u_flicker = gl.GetUniformLocation(g_prog, "u_flicker");
		u_time = gl.GetUniformLocation(g_prog, "u_time");

		gl.GenVertexArrays(1, &g_vao);
		log_info("crt_post: ready");
		return true;
	}

	void shutdown()
	{
		if (g_vao)
			gl.DeleteVertexArrays(1, &g_vao);
		if (g_prog)
			gl.DeleteProgram(g_prog);
		if (g_fbo)
			gl.DeleteFramebuffers(1, &g_fbo);
		if (g_tex)
			glDeleteTextures(1, &g_tex);
		g_vao = g_prog = g_fbo = g_tex = 0;
		g_w = g_h = 0;
	}

	void begin(int width, int height)
	{
		if (width < 1 || height < 1)
			return;
		resize(width, height);

		gl.BindFramebuffer(GL_FRAMEBUFFER, g_fbo);
		glViewport(0, 0, width, height);
		glClearColor(0.082f, 0.051f, 0.024f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}

	void end(int width, int height, float time_sec)
	{
		if (width < 1 || height < 1)
			return;

		gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
		glViewport(0, 0, width, height);
		glDisable(GL_BLEND);
		glDisable(GL_DEPTH_TEST);

		gl.UseProgram(g_prog);
		gl.ActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, g_tex);

		gl.Uniform1i(u_tex, 0);
		gl.Uniform2f(u_res, (float)width, (float)height);
		gl.Uniform1f(u_flicker, flicker_at(time_sec));
		gl.Uniform1f(u_time, time_sec);

		gl.BindVertexArray(g_vao);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		gl.BindVertexArray(0);
		gl.UseProgram(0);
	}
}  // namespace crt_post
