#pragma once
#include <string>
#include <vector>

namespace msg_box
{
	enum class Icon
	{
		None,
		Info,
		Warning,
		Error,
		Question
	};

	struct Result
	{
		bool closed = false;
		int button_index = -1;
		std::string button_label;
	};

	struct Config
	{
		std::string title = "notice";
		std::string message;
		Icon icon = Icon::Info;

		std::vector<std::string> buttons = {"OK"};

		int default_button = 0;

		int escape_button = -1;

		float width = 0.0f;
	};

	int show(Config cfg);

	int show_info(const std::string &title, const std::string &message);
	int show_warning(const std::string &title, const std::string &message);
	int show_error(const std::string &title, const std::string &message);
	int show_confirm(const std::string &title, const std::string &message,
	                 const std::string &confirm_label = "OK",
	                 const std::string &cancel_label = "CANCEL");

	void draw();

	Result poll(int handle);

	bool active();
}  // namespace msg_box
