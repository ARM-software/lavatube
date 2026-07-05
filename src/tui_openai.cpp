#include "tui_openai.h"

#include <curl/curl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "jsoncpp/json/reader.h"

static const long DEFAULT_OPENAI_CONNECT_TIMEOUT_SECONDS = 10;
static const long DEFAULT_OPENAI_REQUEST_TIMEOUT_SECONDS = 60;

static size_t curl_write_string(char* ptr, size_t size, size_t nmemb, void* userdata)
{
	const size_t total = size * nmemb;
	std::string* out = static_cast<std::string*>(userdata);
	out->append(ptr, total);
	return total;
}

static std::string openai_instructions()
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

static Json::Value include_fields()
{
	Json::Value include(Json::arrayValue);
	include.append("reasoning.encrypted_content");
	return include;
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
	return out;
}

static bool ends_with(const std::string& value, const char* suffix)
{
	const size_t len = strlen(suffix);
	return value.size() >= len && value.compare(value.size() - len, len, suffix) == 0;
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

tui_openai_client::tui_openai_client(const std::string& api_key, const std::string& model, const std::string& base_url, const std::string& reasoning_effort)
	: mApiKey(api_key)
	, mModel(model)
	, mBaseUrl(normalize_base_url(base_url))
	, mReasoningEffort(reasoning_effort)
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
}

tui_openai_client::~tui_openai_client()
{
	curl_global_cleanup();
}

tui_assistant_result tui_openai_client::ask(const std::vector<tui_chat_message>& history, const tui_trace_tools& tools) const
{
	tui_assistant_result result;
	if (!configured())
	{
		result.error = "LAVATUI_OPENAI_API_KEY or OPENAI_API_KEY is not set";
		return result;
	}

	Json::Value tool_definitions = tools.tool_definitions();
	Json::Value request = build_initial_request(history, tool_definitions);
	Json::Value input = request["input"];

	for (unsigned round = 0; round < 6; round++)
	{
		response_data response = post_json(request);
		Json::Value root;
		if (!parse_response_json(response, root, result)) return result;

		std::vector<tui_tool_notice> calls;
		if (!collect_tool_calls(root, calls))
		{
			result.error = "Failed to parse tool calls from model response";
			return result;
		}

		if (calls.empty())
		{
			result.text = collect_output_text(root);
			result.usage = collect_usage(root);
			result.ok = true;
			if (result.text.empty()) result.text = "(model returned no text)";
			return result;
		}

		append_response_output(input, root);
		for (tui_tool_notice& call : calls)
		{
			tui_tool_result tool_result = tools.execute(call.name, call.arguments);
			call.ok = tool_result.ok;
			call.output = tui_trim_tool_output(tool_result.output, 12000);
			result.tools.push_back(call);
		}

		append_tool_outputs(input, calls);
		request = build_tool_result_request(input, tool_definitions);
	}

	result.error = "Tool-call limit reached";
	return result;
}

tui_openai_client::response_data tui_openai_client::post_json(const Json::Value& request) const
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

	std::string url = mBaseUrl;
	if (!ends_with(url, "/responses")) url += "/responses";

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_string);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "lava-tui/0.1");
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

	const long connect_timeout = timeout_seconds_from_env("LAVATUI_OPENAI_CONNECT_TIMEOUT", DEFAULT_OPENAI_CONNECT_TIMEOUT_SECONDS);
	const long request_timeout = timeout_seconds_from_env("LAVATUI_OPENAI_TIMEOUT", DEFAULT_OPENAI_REQUEST_TIMEOUT_SECONDS);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connect_timeout);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, request_timeout);

	const CURLcode code = curl_easy_perform(curl);
	if (code != CURLE_OK)
	{
		if (code == CURLE_OPERATION_TIMEDOUT) out.error = "OpenAI request timed out after " + std::to_string(request_timeout) + " seconds";
		else out.error = curl_easy_strerror(code);
	}
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.http_code);

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	out.ok = code == CURLE_OK && out.http_code >= 200 && out.http_code < 300;
	return out;
}

Json::Value tui_openai_client::build_initial_request(const std::vector<tui_chat_message>& history, const Json::Value& tool_definitions) const
{
	Json::Value request;
	request["model"] = mModel;
	if (!mReasoningEffort.empty()) request["reasoning"]["effort"] = mReasoningEffort;
	request["instructions"] = openai_instructions();
	request["tools"] = tool_definitions;
	request["tool_choice"] = "auto";
	request["store"] = false;
	request["include"] = include_fields();

	Json::Value input(Json::arrayValue);
	const size_t start = history.size() > 12 ? history.size() - 12 : 0;
	for (size_t i = start; i < history.size(); i++)
	{
		input.append(input_message(history[i].role, history[i].content));
	}
	request["input"] = input;
	return request;
}

Json::Value tui_openai_client::build_tool_result_request(const Json::Value& input, const Json::Value& tool_definitions) const
{
	Json::Value request;
	request["model"] = mModel;
	if (!mReasoningEffort.empty()) request["reasoning"]["effort"] = mReasoningEffort;
	request["instructions"] = openai_instructions();
	request["tools"] = tool_definitions;
	request["tool_choice"] = "auto";
	request["store"] = false;
	request["include"] = include_fields();
	request["input"] = input;
	return request;
}

bool tui_openai_client::parse_response_json(const response_data& response, Json::Value& root, tui_assistant_result& result) const
{
	if (!response.error.empty())
	{
		result.error = response.error;
		return false;
	}

	Json::Reader reader;
	if (!reader.parse(response.body, root, false))
	{
		result.error = "Failed to parse OpenAI response JSON: " + reader.getFormattedErrorMessages();
		return false;
	}

	if (!response.ok)
	{
		const std::string api_error = response_error_message(root);
		if (!api_error.empty()) result.error = api_error;
		else result.error = "OpenAI HTTP error " + std::to_string(response.http_code);
		return false;
	}

	return true;
}

std::string tui_openai_client::collect_output_text(const Json::Value& root) const
{
	std::string text;
	const Json::Value& output = root["output"];
	if (!output.isArray()) return text;

	for (const Json::Value& item : output)
	{
		if (json_string_field(item, "type") != "message") continue;
		const Json::Value& content = item["content"];
		if (!content.isArray()) continue;
		for (const Json::Value& part : content)
		{
			if (json_string_field(part, "type") == "output_text" && part.isMember("text"))
			{
				if (!text.empty()) text += "\n";
				text += part["text"].asString();
			}
		}
	}

	if (text.empty() && root.isMember("output_text")) text = root["output_text"].asString();
	return text;
}

std::string tui_openai_client::collect_usage(const Json::Value& root) const
{
	if (!root.isMember("usage") || !root["usage"].isObject()) return "";
	const Json::Value& usage = root["usage"];
	std::string out;
	if (usage.isMember("input_tokens")) out += "in=" + std::to_string(usage["input_tokens"].asUInt64());
	if (usage.isMember("output_tokens"))
	{
		if (!out.empty()) out += " ";
		out += "out=" + std::to_string(usage["output_tokens"].asUInt64());
	}
	if (usage.isMember("total_tokens"))
	{
		if (!out.empty()) out += " ";
		out += "total=" + std::to_string(usage["total_tokens"].asUInt64());
	}
	return out;
}

bool tui_openai_client::collect_tool_calls(const Json::Value& root, std::vector<tui_tool_notice>& calls) const
{
	const Json::Value& output = root["output"];
	if (!output.isArray()) return true;

	for (const Json::Value& item : output)
	{
		if (json_string_field(item, "type") != "function_call") continue;
		tui_tool_notice notice;
		notice.name = json_string_field(item, "name");
		notice.arguments = json_string_field(item, "arguments");
		notice.call_id = json_string_field(item, "call_id");
		if (notice.name.empty() || notice.call_id.empty()) return false;
		if (notice.arguments.empty()) notice.arguments = "{}";
		calls.push_back(notice);
	}

	return true;
}

void tui_openai_client::append_response_output(Json::Value& input, const Json::Value& root) const
{
	const Json::Value& output = root["output"];
	if (!output.isArray()) return;
	for (const Json::Value& item : output)
	{
		input.append(item);
	}
}

void tui_openai_client::append_tool_outputs(Json::Value& input, const std::vector<tui_tool_notice>& notices) const
{
	for (const tui_tool_notice& notice : notices)
	{
		Json::Value item;
		item["type"] = "function_call_output";
		item["call_id"] = notice.call_id;
		item["output"] = notice.output;
		input.append(item);
	}
}
