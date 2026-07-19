#pragma once
#include "gl_ctx.hpp"

namespace crt_post
{
	bool init();
	void shutdown();

	void begin(int width, int height);

	void end(int width, int height, float time_sec);
}  // namespace crt_post
