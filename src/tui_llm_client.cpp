#include "tui_llm_client.h"

#include <curl/curl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "jsoncpp/json/reader.h"

static const long DEFAULT_LLM_CONNECT_TIMEOUT_SECONDS = 10;
static const long DEFAULT_LLM_REQUEST_TIMEOUT_SECONDS = 60;

static size_t curl_write_string(char* ptr, size_t size, size_t nmemb, void* userdata)
{
	const size_t total = size * nmemb;
	std::string* out = static_cast<std::string*>(userdata);
	out->append(ptr, total);
	return total;
}

static std::string llm_instructions()
{
	return "You are a lavatube trace investigator. Answer questions about the loaded Vulkan trace or replay service. "
	       "Use the provided tools for trace and replay facts instead of guessing. Some tools can control the replay service; "
	       "only continue, step, goto, or stop replay when the user asks for that action. Ask for a specific thread, frame, "
	       "object type, object index, command name, or packet number when a request cannot be answered without it. Keep answers "
	       "concise and explain which trace or replay metadata you inspected.";
}

static Json::Value input_message(const std::string& role, const std::string& content)
{
	Json::Value item;
	item["role"] = role;
	item["content"] = content;
	return item;
}

static std::string json_string_field(const Json::Value& value, const char* name)
{
	if (!value.isMember(name) || !value[name].isString()) return "";
	return value[name].asString();
}

static std::string response_error_message(const Json::Value& root)
{
	if (root.isMember("error"))
	{
		if (root["error"].isString()) return root["error"].asString();
		if (root["error"].isObject() && root["error"].isMember("message")) return root["error"]["message"].asString();
		return tui_json_compact(root["error"]);
	}
	return "";
}

static std::string normalize_base_url(const std::string& base_url)
{
	std::string out = base_url.empty() ? "https://api.openai.com/v1" : base_url;
	while (!out.empty() && out.back() == '/')
	{
		out.pop_back();
	}
	const std::string legacy_suffix = "/responses";
	if (out.size() >= legacy_suffix.size() && out.compare(out.size() - legacy_suffix.size(), legacy_suffix.size(), legacy_suffix) == 0)
	{
		out.erase(out.size() - legacy_suffix.size());
		while (!out.empty() && out.back() == '/')
		{
			out.pop_back();
		}
	}
	const std::string completions_suffix = "/chat/completions";
	if (out.size() >= completions_suffix.size() && out.compare(out.size() - completions_suffix.size(), completions_suffix.size(), completions_suffix) == 0)
	{
		out.erase(out.size() - completions_suffix.size());
		while (!out.empty() && out.back() == '/')
		{
			out.pop_back();
		}
	}
	return out;
}

static std::string endpoint_url(const std::string& base_url)
{
	std::string out = normalize_base_url(base_url);
	out += "/chat/completions";
	return out;
}

static long timeout_seconds_from_env(const char* name, long fallback)
{
	const char* text = getenv(name);
	if (!text || text[0] == '\0') return fallback;

	char* end = nullptr;
	errno = 0;
	const long value = strtol(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' || value <= 0) return fallback;
	return value;
}

tui_llm_client::tui_llm_client(const std::string& api_key, const std::string& model, const std::string& base_url, const std::string& reasoning_effort)
	: mApiKey(api_key)
	, mModel(model)
	, mBaseUrl(normalize_base_url(base_url))
	, mReasoningEffort(reasoning_effort)
{
}

tui_assistant_result tui_llm_client::ask(const std::vector<tui_chat_message>& history, const tui_trace_tools& tools) const
{
	tui_assistant_result result;
	if (!configured())
	{
		result.error = "The selected LLM API key is not set";
		return result;
	}

	Json::Value tool_definitions = tools.tool_definitions();
	Json::Value request = build_initial_request(history, tool_definitions);
	Json::Value messages = request["messages"];

	for (unsigned round = 0; round < 6; round++)
	{
		response_data response = post_json(request);
		Json::Value root;
		if (!parse_response_json(response, root, result)) return result;

		const Json::Value& choice = root["choices"][0];
		const std::string finish_reason = choice.isMember("finish_reason") && choice["finish_reason"].isString()
			? choice["finish_reason"].asString()
			: "";

		std::vector<tui_tool_notice> calls;
		if (!collect_tool_calls(root, calls))
		{
			if (finish_reason == "length")
			{
				result.error = "Model response was truncated during tool calls (finish_reason=length)";
			}
			else
			{
				result.error = "Failed to parse tool calls from model response";
			}
			return result;
		}

		if (calls.empty())
		{
			result.text = collect_output_text(root);
			result.usage = collect_usage(root);
			if (finish_reason == "length")
			{
				if (!result.usage.empty()) result.usage += " ";
				result.usage += "[truncated: length]";
				if (!result.text.empty()) result.text += "\n[response truncated by token limit]";
			}
			result.ok = true;
			if (result.text.empty()) result.text = "(model returned no text)";
			return result;
		}

		append_response_output(messages, root);
		for (tui_tool_notice& call : calls)
		{
			tui_tool_result tool_result = tools.execute(call.name, call.arguments);
			call.ok = tool_result.ok;
			call.output = tui_trim_tool_output(tool_result.output, 12000);
			result.tools.push_back(call);
		}

		append_tool_outputs(messages, calls);
		request = build_tool_result_request(messages, tool_definitions);
	}

	result.error = "Tool-call limit reached";
	return result;
}

tui_llm_client::response_data tui_llm_client::post_json(const Json::Value& request) const
{
	response_data out;
	CURL* curl = curl_easy_init();
	if (!curl)
	{
		out.error = "curl_easy_init failed";
		return out;
	}

	const std::string body = tui_json_compact(request);
	std::string auth = "Authorization: Bearer " + mApiKey;
	struct curl_slist* headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, auth.c_str());

	const std::string url = endpoint_url(mBaseUrl);

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_string);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "lava-tui/0.1");
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

	const long connect_timeout = timeout_seconds_from_env("LAVATUI_LLM_CONNECT_TIMEOUT", DEFAULT_LLM_CONNECT_TIMEOUT_SECONDS);
	const long request_timeout = timeout_seconds_from_env("LAVATUI_LLM_TIMEOUT", DEFAULT_LLM_REQUEST_TIMEOUT_SECONDS);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connect_timeout);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, request_timeout);

	const CURLcode code = curl_easy_perform(curl);
	if (code != CURLE_OK)
	{
		if (code == CURLE_OPERATION_TIMEDOUT) out.error = "LLM request timed out after " + std::to_string(request_timeout) + " seconds";
		else out.error = curl_easy_strerror(code);
	}
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.http_code);

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	out.ok = code == CURLE_OK && out.http_code >= 200 && out.http_code < 300;
	return out;
}

Json::Value tui_llm_client::build_initial_request(const std::vector<tui_chat_message>& history, const Json::Value& tool_definitions) const
{
	Json::Value request;
	request["model"] = mModel;
	if (!mReasoningEffort.empty() && mReasoningEffort != "none")
	{
		request["reasoning_effort"] = mReasoningEffort;
	}
	request["tools"] = tool_definitions;
	request["tool_choice"] = "auto";
	request["parallel_tool_calls"] = false;

	Json::Value messages(Json::arrayValue);
	messages.append(input_message("system", llm_instructions()));
	const size_t start = history.size() > 12 ? history.size() - 12 : 0;
	for (size_t i = start; i < history.size(); i++)
	{
		messages.append(input_message(history[i].role, history[i].content));
	}
	request["messages"] = messages;
	return request;
}

Json::Value tui_llm_client::build_tool_result_request(const Json::Value& messages, const Json::Value& tool_definitions) const
{
	Json::Value request;
	request["model"] = mModel;
	if (!mReasoningEffort.empty() && mReasoningEffort != "none")
	{
		request["reasoning_effort"] = mReasoningEffort;
	}
	request["tools"] = tool_definitions;
	request["tool_choice"] = "auto";
	request["parallel_tool_calls"] = false;
	request["messages"] = messages;
	return request;
}

bool tui_llm_client::parse_response_json(const response_data& response, Json::Value& root, tui_assistant_result& result) const
{
	if (!response.error.empty())
	{
		result.error = response.error;
		return false;
	}

	Json::Reader reader;
	if (!reader.parse(response.body, root, false))
	{
		result.error = "Failed to parse LLM response JSON: " + reader.getFormattedErrorMessages();
		return false;
	}

	if (!response.ok)
	{
		const std::string api_error = response_error_message(root);
		if (!api_error.empty()) result.error = api_error;
		else result.error = "LLM HTTP error " + std::to_string(response.http_code);
		return false;
	}

	if (!root.isMember("choices") || !root["choices"].isArray() || root["choices"].empty()
	    || !root["choices"][0].isMember("message") || !root["choices"][0]["message"].isObject())
	{
		result.error = "The model response did not contain choices[0].message";
		return false;
	}

	return true;
}

std::string tui_llm_client::collect_output_text(const Json::Value& root) const
{
	std::string text;
	if (!root.isMember("choices") || !root["choices"].isArray() || root["choices"].empty()) return text;
	const Json::Value& choice = root["choices"][0];
	if (!choice.isMember("message") || !choice["message"].isObject()) return text;
	const Json::Value& message = choice["message"];
	if (message.isMember("content"))
	{
		if (message["content"].isString())
		{
			text = message["content"].asString();
		}
		else if (message["content"].isArray())
		{
			for (const Json::Value& part : message["content"])
			{
				if (part.isObject() && part.isMember("text") && part["text"].isString())
				{
					if (!text.empty()) text += "\n";
					text += part["text"].asString();
				}
			}
		}
	}
	return text;
}

std::string tui_llm_client::collect_usage(const Json::Value& root) const
{
	if (!root.isMember("usage") || !root["usage"].isObject()) return "";
	const Json::Value& usage = root["usage"];
	std::string out;
	uint64_t in_tokens = 0;
	bool has_in = false;
	if (usage.isMember("prompt_tokens") && usage["prompt_tokens"].isUInt64())
	{
		in_tokens = usage["prompt_tokens"].asUInt64();
		has_in = true;
	}
	else if (usage.isMember("input_tokens") && usage["input_tokens"].isUInt64())
	{
		in_tokens = usage["input_tokens"].asUInt64();
		has_in = true;
	}
	if (has_in) out += "in=" + std::to_string(in_tokens);

	uint64_t out_tokens = 0;
	bool has_out = false;
	if (usage.isMember("completion_tokens") && usage["completion_tokens"].isUInt64())
	{
		out_tokens = usage["completion_tokens"].asUInt64();
		has_out = true;
	}
	else if (usage.isMember("output_tokens") && usage["output_tokens"].isUInt64())
	{
		out_tokens = usage["output_tokens"].asUInt64();
		has_out = true;
	}
	if (has_out)
	{
		if (!out.empty()) out += " ";
		out += "out=" + std::to_string(out_tokens);
	}

	if (usage.isMember("total_tokens") && usage["total_tokens"].isUInt64())
	{
		if (!out.empty()) out += " ";
		out += "total=" + std::to_string(usage["total_tokens"].asUInt64());
	}
	return out;
}

bool tui_llm_client::collect_tool_calls(const Json::Value& root, std::vector<tui_tool_notice>& calls) const
{
	if (!root.isMember("choices") || !root["choices"].isArray() || root["choices"].empty()) return true;
	const Json::Value& choice = root["choices"][0];
	if (!choice.isMember("message") || !choice["message"].isObject()) return true;
	const Json::Value& message = choice["message"];
	if (!message.isMember("tool_calls") || !message["tool_calls"].isArray()) return true;

	for (const Json::Value& item : message["tool_calls"])
	{
		tui_tool_notice notice;
		notice.call_id = json_string_field(item, "id");
		if (item.isMember("function") && item["function"].isObject())
		{
			notice.name = json_string_field(item["function"], "name");
			if (item["function"].isMember("arguments"))
			{
				if (item["function"]["arguments"].isString())
				{
					notice.arguments = item["function"]["arguments"].asString();
				}
				else if (item["function"]["arguments"].isObject())
				{
					notice.arguments = tui_json_compact(item["function"]["arguments"]);
				}
			}
		}
		if (notice.name.empty() || notice.call_id.empty()) return false;
		if (notice.arguments.empty()) notice.arguments = "{}";
		calls.push_back(notice);
	}

	return true;
}

void tui_llm_client::append_response_output(Json::Value& messages, const Json::Value& root) const
{
	if (!root.isMember("choices") || !root["choices"].isArray() || root["choices"].empty()) return;
	const Json::Value& choice = root["choices"][0];
	if (!choice.isMember("message") || !choice["message"].isObject()) return;
	const Json::Value& message = choice["message"];
	Json::Value assistant_message;
	assistant_message["role"] = "assistant";
	if (message.isMember("content") && !message["content"].isNull())
	{
		assistant_message["content"] = message["content"];
	}
	else
	{
		assistant_message["content"] = Json::Value(Json::nullValue);
	}
	if (message.isMember("tool_calls") && message["tool_calls"].isArray())
	{
		assistant_message["tool_calls"] = message["tool_calls"];
	}
	messages.append(assistant_message);
}

void tui_llm_client::append_tool_outputs(Json::Value& messages, const std::vector<tui_tool_notice>& notices) const
{
	for (const tui_tool_notice& notice : notices)
	{
		Json::Value item;
		item["role"] = "tool";
		item["tool_call_id"] = notice.call_id;
		item["content"] = notice.output;
		messages.append(item);
	}
}
