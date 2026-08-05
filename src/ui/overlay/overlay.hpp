#pragma once

namespace sol
{
	class state;
}

namespace overlay
{
	void init();
	void shutdown();
	bool try_claim_backend(const char *name);

	bool visible();
	void set_visible(bool v);
	void toggle();

	void render();

	void on_backend_ready();

	bool backend_ready();

	void clear_state();

	void bind_lua(sol::state &lua);
}  // namespace overlay
