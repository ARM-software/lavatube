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

		std::vector<size_t> widths(headers.size(), 0);
		for (size_t i = 0; i < headers.size(); ++i)
		{
			widths[i] = headers[i].size();
		}
		for (const auto& row : rows)
		{
			for (size_t i = 0; i < row.size() && i < widths.size(); ++i)
			{
				if (row[i].size() > widths[i])
				{
					widths[i] = row[i].size();
				}
			}
		}

		// Headers
		ss << "|";
		for (size_t i = 0; i < headers.size(); ++i)
		{
			write_markdown_cell(ss, headers[i], widths[i]);
		}
		ss << "\n";

		// Separator
		ss << "|";
		for (size_t i = 0; i < headers.size(); ++i)
		{
			size_t separator_width = widths[i] + 2;
			if (separator_width < 3)
			{
				separator_width = 3;
			}
			ss << std::string(separator_width, '-') << "|";
		}
		ss << "\n";

		// Rows
		for (const auto& row : rows)
		{
			ss << "|";
			for (size_t i = 0; i < headers.size(); ++i)
			{
				write_markdown_cell(ss, i < row.size() ? row[i] : "", widths[i]);
			}
			ss << "\n";
		}
		return ss.str();
	}

private:
	static void write_markdown_cell(std::stringstream& ss, const std::string& cell, size_t width)
	{
		ss << " " << cell;
		for (size_t i = cell.size(); i < width; ++i)
		{
			ss << " ";
		}
		ss << " |";
	}

	std::vector<std::string> headers;
	std::vector<std::vector<std::string>> rows;
};
