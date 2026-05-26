#pragma once

#include <vector>
#include <string>
#include <sstream>

class data_table
{
public:
	void set_headers(const std::vector<std::string>& h)
	{
		headers = h;
	}

	void add_row(const std::vector<std::string>& row)
	{
		rows.push_back(row);
	}

	std::string to_tsv() const
	{
		std::stringstream ss;

		// Headers
		for (size_t i = 0; i < headers.size(); ++i)
		{
			ss << headers[i] << (i == headers.size() - 1 ? "" : "\t");
		}
		ss << "\n";

		// Rows
		for (const auto& row : rows)
		{
			for (size_t i = 0; i < row.size(); ++i)
			{
				ss << row[i] << (i == row.size() - 1 ? "" : "\t");
			}
			ss << "\n";
		}
		return ss.str();
	}

	std::string to_markdown() const
	{
		std::stringstream ss;

		if (headers.empty())
		{
			return "";
		}

		// Headers
		ss << "|";
		for (const auto& h : headers)
		{
			ss << " " << h << " |";
		}
		ss << "\n";

		// Separator
		ss << "|";
		for (size_t i = 0; i < headers.size(); ++i)
		{
			ss << "---|";
		}
		ss << "\n";

		// Rows
		for (const auto& row : rows)
		{
			ss << "|";
			for (const auto& cell : row)
			{
				ss << " " << cell << " |";
			}
			ss << "\n";
		}
		return ss.str();
	}

private:
	std::vector<std::string> headers;
	std::vector<std::vector<std::string>> rows;
};
