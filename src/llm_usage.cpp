#include "llm_usage.h"

void llm_usage_tracker::add(const Json::Value& root_or_response)
{
	if (!root_or_response.isMember("usage") || !root_or_response["usage"].isObject())
	{
		return;
	}
	const Json::Value& usage = root_or_response["usage"];

	if (usage.isMember("prompt_tokens") && usage["prompt_tokens"].isUInt64())
	{
		input_tokens += usage["prompt_tokens"].asUInt64();
		has_input_tokens = true;
	}
	else if (usage.isMember("input_tokens") && usage["input_tokens"].isUInt64())
	{
		input_tokens += usage["input_tokens"].asUInt64();
		has_input_tokens = true;
	}

	if (usage.isMember("prompt_tokens_details") && usage["prompt_tokens_details"].isObject()
	    && usage["prompt_tokens_details"].isMember("cached_tokens") && usage["prompt_tokens_details"]["cached_tokens"].isUInt64())
	{
		cached_tokens += usage["prompt_tokens_details"]["cached_tokens"].asUInt64();
		has_cached_tokens = true;
	}
	else if (usage.isMember("prompt_cache_hit_tokens") && usage["prompt_cache_hit_tokens"].isUInt64())
	{
		cached_tokens += usage["prompt_cache_hit_tokens"].asUInt64();
		has_cached_tokens = true;
	}
	else if (usage.isMember("cached_tokens") && usage["cached_tokens"].isUInt64())
	{
		cached_tokens += usage["cached_tokens"].asUInt64();
		has_cached_tokens = true;
	}

	if (usage.isMember("completion_tokens") && usage["completion_tokens"].isUInt64())
	{
		output_tokens += usage["completion_tokens"].asUInt64();
		has_output_tokens = true;
	}
	else if (usage.isMember("output_tokens") && usage["output_tokens"].isUInt64())
	{
		output_tokens += usage["output_tokens"].asUInt64();
		has_output_tokens = true;
	}

	if (usage.isMember("total_tokens") && usage["total_tokens"].isUInt64())
	{
		total_tokens += usage["total_tokens"].asUInt64();
		has_total_tokens = true;
	}
}

std::string llm_usage_tracker::format() const
{
	std::string out;
	if (has_input_tokens)
	{
		out += "in=" + std::to_string(input_tokens);
	}
	if (has_cached_tokens)
	{
		if (!out.empty())
		{
			out += " ";
		}
		out += "cached=" + std::to_string(cached_tokens);
	}
	if (has_output_tokens)
	{
		if (!out.empty())
		{
			out += " ";
		}
		out += "out=" + std::to_string(output_tokens);
	}
	if (has_total_tokens)
	{
		if (!out.empty())
		{
			out += " ";
		}
		out += "total=" + std::to_string(total_tokens);
	}
	return out;
}
