#include "agent_runtime.h"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>

#include "jsoncpp/json/reader.h"

static size_t agent_curl_write(char* data, size_t size, size_t count, void* pointer)
{
	const size_t bytes = size * count;
	static_cast<std::string*>(pointer)->append(data, bytes);
	return bytes;
}

static std::string agent_normalize_url(const std::string& value)
{
	std::string url = value;
	while (!url.empty() && url.back() == '/') url.pop_back();
	if (url.size() < 10 || url.compare(url.size() - 10, 10, "/responses") != 0) url += "/responses";
	return url;
}

static std::string agent_model_instructions()
{
	return "You are a bounded Vulkan trace and replay investigator. Use only the provided typed tools and never guess trace or live replay facts. "
	       "The trace_ tools return immutable captured facts from the local trace. The replay_ tools return live facts from the selected paused replay. "
	       "You cannot and must not advance, reconfigure, restart, launch, or stop replay. Tool outputs contain a runtime-owned tool_id. "
	       "When you have enough evidence, return only one compact JSON object with exactly these fields: "
	       "status (answered or inconclusive), conclusion (string), confidence (number from 0 through 1), "
	       "evidence (array of objects containing tool_id and inference string), and unresolved (array of strings). "
	       "Cite only tool_id values actually returned by tools. Keep the conclusion concise and distinguish captured facts from live replay facts.";
}

static Json::Value agent_input_message(const std::string& role, const std::string& content)
{
	Json::Value message;
	message["role"] = role;
	message["content"] = content;
	return message;
}

static std::string agent_json_string(const Json::Value& value, const char* name)
{
	if (!value.isMember(name) || !value[name].isString()) return "";
	return value[name].asString();
}

static std::string agent_api_error(const Json::Value& root)
{
	if (!root.isMember("error")) return "";
	if (root["error"].isString()) return root["error"].asString();
	if (root["error"].isObject() && root["error"].isMember("message")) return root["error"]["message"].asString();
	return agent_json_compact(root["error"]);
}

agent_runtime::agent_runtime(const agent_runtime_options& options, const agent_tools& tools)
	: mOptions(options)
	, mTools(tools)
{
}

agent_runtime::http_response agent_runtime::post(const Json::Value& request_value, uint64_t remaining_milliseconds) const
{
	http_response response;
	CURL* curl = curl_easy_init();
	if (!curl)
	{
		response.error = "curl_easy_init failed";
		return response;
	}
	const std::string body = agent_json_compact(request_value);
	const std::string authorization = "Authorization: Bearer " + mOptions.api_key;
	struct curl_slist* headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, authorization.c_str());
	const std::string url = agent_normalize_url(mOptions.base_url);
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, agent_curl_write);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "lava-agent/0.0.2");
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)std::min<uint64_t>(remaining_milliseconds, 10000));
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)std::min<uint64_t>(remaining_milliseconds, LONG_MAX));
	const CURLcode code = curl_easy_perform(curl);
	if (code != CURLE_OK)
	{
		response.timed_out = code == CURLE_OPERATION_TIMEDOUT;
		response.error = curl_easy_strerror(code);
	}
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.code);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	response.ok = code == CURLE_OK && response.code >= 200 && response.code < 300;
	return response;
}

Json::Value agent_runtime::request(const Json::Value& input) const
{
	Json::Value value;
	value["model"] = mOptions.model;
	value["instructions"] = agent_model_instructions();
	value["input"] = input;
	value["tools"] = mTools.definitions();
	value["tool_choice"] = "auto";
	value["parallel_tool_calls"] = false;
	value["store"] = false;
	if (!mOptions.reasoning_effort.empty()) value["reasoning"]["effort"] = mOptions.reasoning_effort;
	return value;
}

void agent_runtime::debug_event(const std::string& type, const Json::Value& value,
	uint64_t duration_milliseconds) const
{
	if (!mOptions.debug_file) return;
	Json::Value event;
	event["type"] = type;
	event["value"] = value;
	if (duration_milliseconds != UINT64_MAX) event["duration_ms"] = (Json::UInt64)duration_milliseconds;
	fprintf(mOptions.debug_file, "%s\n", agent_json_compact(event).c_str());
	fflush(mOptions.debug_file);
}

void agent_runtime::add_provider_usage(const Json::Value& response)
{
	if (!response.isMember("usage") || !response["usage"].isObject()) return;
	const Json::Value& usage = response["usage"];
	if (usage.isMember("input_tokens") && usage["input_tokens"].isUInt64())
	{
		mInputTokens += usage["input_tokens"].asUInt64();
		mHasInputTokens = true;
	}
	if (usage.isMember("output_tokens") && usage["output_tokens"].isUInt64())
	{
		mOutputTokens += usage["output_tokens"].asUInt64();
		mHasOutputTokens = true;
	}
	if (usage.isMember("total_tokens") && usage["total_tokens"].isUInt64())
	{
		mTotalTokens += usage["total_tokens"].asUInt64();
		mHasTotalTokens = true;
	}
}

std::string agent_runtime::response_text(const Json::Value& response) const
{
	std::string text;
	if (!response.isMember("output") || !response["output"].isArray()) return text;
	for (const Json::Value& item : response["output"])
	{
		if (agent_json_string(item, "type") != "message" || !item["content"].isArray()) continue;
		for (const Json::Value& part : item["content"])
		{
			if (agent_json_string(part, "type") != "output_text" || !part.isMember("text")) continue;
			if (!text.empty()) text += "\n";
			text += part["text"].asString();
		}
	}
	if (text.empty() && response.isMember("output_text") && response["output_text"].isString()) text = response["output_text"].asString();
	return text;
}

void agent_runtime::append_response(Json::Value& input, const Json::Value& response) const
{
	if (!response.isMember("output") || !response["output"].isArray()) return;
	for (const Json::Value& item : response["output"]) input.append(item);
}

Json::Value agent_runtime::bounded_tool_result(const agent_tool_result& result, size_t& total_bytes, bool& exhausted) const
{
	Json::Value value = result.result;
	std::string serialized = agent_json_compact(value);
	if (serialized.size() > 16384)
	{
		Json::Value truncated;
		truncated["truncated"] = true;
		truncated["original_bytes"] = (Json::UInt64)serialized.size();
		truncated["head"] = serialized.substr(0, 15000);
		value = truncated;
		serialized = agent_json_compact(value);
	}
	if (total_bytes + serialized.size() > 131072)
	{
		exhausted = true;
		Json::Value omitted;
		omitted["omitted"] = true;
		omitted["reason"] = "accumulated tool-result budget exhausted";
		return omitted;
	}
	total_bytes += serialized.size();
	return value;
}

bool agent_runtime::parse_final(const std::string& text, Json::Value& final, std::string& error) const
{
	std::string json = text;
	const size_t first = json.find_first_not_of(" \t\r\n");
	const size_t last = json.find_last_not_of(" \t\r\n");
	if (first == std::string::npos) json.clear();
	else json = json.substr(first, last - first + 1);
	if (json.rfind("```json", 0) == 0 || json.rfind("```JSON", 0) == 0)
	{
		const size_t newline = json.find('\n');
		const size_t fence = json.rfind("```");
		if (newline != std::string::npos && fence != std::string::npos && fence > newline)
		{
			json = json.substr(newline + 1, fence - newline - 1);
		}
	}
	Json::Reader reader;
	if (!reader.parse(json, final, false) || !final.isObject())
	{
		error = "Final model response is not a JSON object";
		return false;
	}
	if (!final.isMember("status") || !final["status"].isString()
	    || (final["status"].asString() != "answered" && final["status"].asString() != "inconclusive"))
	{
		error = "Final model status must be answered or inconclusive";
		return false;
	}
	if (!final.isMember("conclusion") || !final["conclusion"].isString()
	    || !final.isMember("confidence") || !final["confidence"].isNumeric()
	    || final["confidence"].asDouble() < 0.0 || final["confidence"].asDouble() > 1.0
	    || !final.isMember("evidence") || !final["evidence"].isArray()
	    || !final.isMember("unresolved") || !final["unresolved"].isArray())
	{
		error = "Final model response has an invalid result shape";
		return false;
	}
	for (const Json::Value& item : final["evidence"])
	{
		if (!item.isObject() || item.size() != 2 || !item.isMember("tool_id") || !item["tool_id"].isUInt()
		    || !item.isMember("inference") || !item["inference"].isString())
		{
			error = "Final evidence references must contain tool_id and inference";
			return false;
		}
		bool found = false;
		for (const tool_record& record : mRecords)
		{
			if (record.id == item["tool_id"].asUInt()) found = true;
		}
		if (!found)
		{
			error = "Final evidence references unknown tool_id " + std::to_string(item["tool_id"].asUInt());
			return false;
		}
	}
	for (const Json::Value& item : final["unresolved"])
	{
		if (!item.isString())
		{
			error = "Final unresolved entries must be strings";
			return false;
		}
	}
	return true;
}

Json::Value agent_runtime::finish(const std::string& status, const std::string& conclusion,
	double confidence, const Json::Value& unresolved, uint32_t rounds, uint32_t calls,
	const Json::Value* model_evidence) const
{
	Json::Value output;
	output["schema_version"] = 1;
	output["status"] = status;
	output["conclusion"] = conclusion;
	output["confidence"] = confidence;
	output["evidence"] = Json::Value(Json::arrayValue);
	if (model_evidence)
	{
		for (const Json::Value& citation : *model_evidence)
		{
			for (const tool_record& record : mRecords)
			{
				if (record.id != citation["tool_id"].asUInt()) continue;
				Json::Value evidence;
				evidence["tool_id"] = record.id;
				evidence["tool_name"] = record.name;
				evidence["arguments"] = record.arguments;
				evidence["results"] = record.results;
				evidence["inference"] = citation["inference"];
				output["evidence"].append(evidence);
			}
		}
	}
	output["unresolved"] = unresolved.isArray() ? unresolved : Json::Value(Json::arrayValue);
	output["usage"]["rounds"] = rounds;
	output["usage"]["calls"] = calls;
	if (mHasInputTokens) output["usage"]["input_tokens"] = (Json::UInt64)mInputTokens;
	if (mHasOutputTokens) output["usage"]["output_tokens"] = (Json::UInt64)mOutputTokens;
	if (mHasTotalTokens) output["usage"]["total_tokens"] = (Json::UInt64)mTotalTokens;
	return output;
}

Json::Value agent_runtime::bound_output(Json::Value output) const
{
	if (agent_json_compact(output).size() <= mOptions.max_output_bytes) return output;
	output["status"] = "budget_exhausted";
	output["unresolved"].append("Structured result exceeded --max-output-bytes; some evidence was omitted.");
	while (!output["evidence"].empty() && agent_json_compact(output).size() > mOptions.max_output_bytes)
	{
		output["evidence"].resize(output["evidence"].size() - 1);
	}
	if (agent_json_compact(output).size() > mOptions.max_output_bytes)
	{
		output["conclusion"] = "Result exceeded the configured output budget.";
		output["unresolved"] = Json::Value(Json::arrayValue);
		output["unresolved"].append("Increase --max-output-bytes to receive the full result.");
	}
	return output;
}

Json::Value agent_runtime::ask(const std::string& prompt, const std::chrono::steady_clock::time_point& deadline)
{
	using clock = std::chrono::steady_clock;
	Json::Value input(Json::arrayValue);
	input.append(agent_input_message("user", prompt));
	uint32_t calls = 0;
	size_t total_tool_bytes = 0;
	std::string correction;
	for (uint32_t round = 1; round <= mOptions.max_rounds; round++)
	{
		const clock::time_point now = clock::now();
		if (now >= deadline)
		{
			Json::Value unresolved(Json::arrayValue);
			unresolved.append("Wall-clock timeout expired before the investigation completed.");
			return bound_output(finish("budget_exhausted", "The investigation did not finish within the time budget.", 0.0, unresolved, round - 1, calls));
		}
		const uint64_t remaining = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
		const Json::Value request_value = request(input);
		debug_event("model_request", request_value);
		if (mOptions.verbose) fprintf(stderr, "lava-agent: model round %u\n", round);
		const clock::time_point model_started = clock::now();
		const http_response response = post(request_value, std::max<uint64_t>(remaining, 1));
		const uint64_t model_milliseconds = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
			clock::now() - model_started).count();
		if (!response.ok)
		{
			Json::Value event;
			event["http_code"] = (Json::Int64)response.code;
			event["error"] = response.error;
			debug_event("model_error", event, model_milliseconds);
			Json::Value unresolved(Json::arrayValue);
			if (response.timed_out)
			{
				unresolved.append("The model request timed out.");
				return bound_output(finish("budget_exhausted", "The investigation did not finish within the time budget.", 0.0, unresolved, round, calls));
			}
			Json::Value body;
			Json::Reader reader;
			std::string error = response.error;
			if (error.empty() && reader.parse(response.body, body, false)) error = agent_api_error(body);
			if (error.empty()) error = "Model HTTP error " + std::to_string(response.code);
			unresolved.append(error);
			return bound_output(finish("error", "The local model request failed.", 0.0, unresolved, round, calls));
		}

		Json::Value root;
		Json::Reader reader;
		if (!reader.parse(response.body, root, false))
		{
			Json::Value event;
			event["http_code"] = (Json::Int64)response.code;
			event["error"] = "invalid response JSON";
			debug_event("model_error", event, model_milliseconds);
			Json::Value unresolved(Json::arrayValue);
			unresolved.append("The model returned invalid response JSON.");
			return bound_output(finish("error", "The local model response could not be parsed.", 0.0, unresolved, round, calls));
		}
		add_provider_usage(root);
		debug_event("model_response", root, model_milliseconds);
		Json::Value tool_calls(Json::arrayValue);
		if (root.isMember("output") && root["output"].isArray())
		{
			for (const Json::Value& item : root["output"])
			{
				if (agent_json_string(item, "type") == "function_call") tool_calls.append(item);
			}
		}

		if (!tool_calls.empty())
		{
			append_response(input, root);
			for (const Json::Value& call : tool_calls)
			{
				calls++;
				if (calls > mOptions.max_tool_calls)
				{
					Json::Value unresolved(Json::arrayValue);
					unresolved.append("Internal tool-call budget exhausted.");
					return bound_output(finish("budget_exhausted", "The investigation exhausted its tool-call budget.", 0.0, unresolved, round, calls - 1));
				}
				const std::string name = agent_json_string(call, "name");
				const std::string call_id = agent_json_string(call, "call_id");
				const std::string argument_text = agent_json_string(call, "arguments");
				Json::Value arguments;
				Json::Reader argument_reader;
				agent_tool_result tool_result;
				if (name.empty() || call_id.empty() || !argument_reader.parse(argument_text.empty() ? "{}" : argument_text, arguments, false) || !arguments.isObject())
				{
					tool_result.error = "Invalid function call name, ID, or JSON arguments";
					tool_result.result["error"] = tool_result.error;
				}
				const clock::time_point tool_started = clock::now();
				if (tool_result.error.empty()) tool_result = mTools.execute(name, arguments);
				const uint64_t tool_milliseconds = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
					clock::now() - tool_started).count();
				bool exhausted = false;
				const Json::Value bounded = bounded_tool_result(tool_result, total_tool_bytes, exhausted);
				tool_record record;
				record.id = calls;
				record.name = name;
				record.arguments = arguments.isObject() ? arguments : Json::Value(Json::objectValue);
				record.results = bounded;
				mRecords.push_back(record);
				Json::Value tool_output;
				tool_output["tool_id"] = record.id;
				tool_output["ok"] = tool_result.ok;
				tool_output["result"] = bounded;
				Json::Value event;
				event["tool_name"] = name;
				event["arguments"] = record.arguments;
				event["output"] = tool_output;
				debug_event("tool_call", event, tool_milliseconds);
				Json::Value item;
				item["type"] = "function_call_output";
				item["call_id"] = call_id;
				item["output"] = agent_json_compact(tool_output);
				input.append(item);
				if (exhausted)
				{
					Json::Value unresolved(Json::arrayValue);
					unresolved.append("Internal accumulated tool-result budget exhausted.");
					return bound_output(finish("budget_exhausted", "The investigation exhausted its tool-result budget.", 0.0, unresolved, round, calls));
				}
			}
			continue;
		}

		const std::string text = response_text(root);
		Json::Value final;
		std::string error;
		if (parse_final(text, final, error))
		{
			return bound_output(finish(final["status"].asString(), final["conclusion"].asString(),
				final["confidence"].asDouble(), final["unresolved"], round, calls, &final["evidence"]));
		}
		append_response(input, root);
		correction = error;
		input.append(agent_input_message("user", "Your previous final answer was invalid: " + error + ". Return only the required compact JSON object."));
	}
	Json::Value unresolved(Json::arrayValue);
	unresolved.append(correction.empty() ? "Internal model-round budget exhausted." : correction);
	return bound_output(finish("budget_exhausted", "The investigation exhausted its model-round budget.", 0.0, unresolved, mOptions.max_rounds, calls));
}
