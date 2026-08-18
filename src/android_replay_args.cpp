#include "android_replay_args.h"

#include <cctype>

bool parse_android_replay_arguments(const std::string& command_line, std::vector<std::string>& arguments, std::string& error)
{
	arguments.clear();
	error.clear();
	std::string argument;
	bool argument_started = false;
	bool escaped = false;
	char quote = '\0';

	for (char value : command_line)
	{
		if (escaped)
		{
			argument += value;
			argument_started = true;
			escaped = false;
			continue;
		}

		if (quote == '\'')
		{
			if (value == '\'') quote = '\0';
			else argument += value;
			argument_started = true;
			continue;
		}

		if (value == '\\')
		{
			escaped = true;
			argument_started = true;
			continue;
		}

		if (quote == '"')
		{
			if (value == '"') quote = '\0';
			else argument += value;
			argument_started = true;
			continue;
		}

		if (value == '\'' || value == '"')
		{
			quote = value;
			argument_started = true;
		}
		else if (std::isspace(static_cast<unsigned char>(value)))
		{
			if (argument_started)
			{
				arguments.push_back(argument);
				argument.clear();
				argument_started = false;
			}
		}
		else
		{
			argument += value;
			argument_started = true;
		}
	}

	if (escaped)
	{
		error = "trailing backslash";
		return false;
	}
	if (quote != '\0')
	{
		error = "unterminated quote";
		return false;
	}
	if (argument_started) arguments.push_back(argument);
	return true;
}
