#include "litert_lm_replay.h"
#include "read.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static void require(bool condition, const char* message)
{
	if (condition) return;
	fprintf(stderr, "%s\n", message);
	exit(1);
}

static LiteRtLmStatusCode response_text(const LiteRtLmResponses*, size_t index, const char** text)
{
	if (!text || index == 4) return static_cast<LiteRtLmStatusCode>(3);
	*text = index == 0 ? "actual" : (index == 1 ? "actual extra" : "");
	return static_cast<LiteRtLmStatusCode>(0);
}

static LiteRtLmStatusCode candidate_count(const LiteRtLmResponses*, size_t* count)
{
	if (!count) return static_cast<LiteRtLmStatusCode>(3);
	*count = 2;
	return static_cast<LiteRtLmStatusCode>(0);
}

static LiteRtLmStatusCode mutable_response_text(const LiteRtLmResponses* response, size_t index, const char** text)
{
	if (!text) return static_cast<LiteRtLmStatusCode>(3);
	*text = reinterpret_cast<const char*>(response);
	return static_cast<LiteRtLmStatusCode>(0);
}

static LiteRtLmStatusCode failed_engine(const LiteRtLmEngineSettings* settings, LiteRtLmEngine** engine)
{
	++*reinterpret_cast<int*>(const_cast<LiteRtLmEngineSettings*>(settings));
	*engine = nullptr;
	return static_cast<LiteRtLmStatusCode>(2);
}

static LiteRtLmStatusCode successful_engine(const LiteRtLmEngineSettings* settings, LiteRtLmEngine** engine)
{
	++*reinterpret_cast<int*>(const_cast<LiteRtLmEngineSettings*>(settings));
	*engine = reinterpret_cast<LiteRtLmEngine*>(const_cast<LiteRtLmEngineSettings*>(settings));
	return static_cast<LiteRtLmStatusCode>(0);
}

static void delete_engine(LiteRtLmEngine* engine)
{
	++*reinterpret_cast<int*>(engine);
}

static LiteRtLmStatusCode json_response_text(const LiteRtLmJsonResponse* response, const char** text)
{
	if (!response || !text) return static_cast<LiteRtLmStatusCode>(3);
	*text = reinterpret_cast<const char*>(response);
	return static_cast<LiteRtLmStatusCode>(0);
}

static LiteRtLmStatusCode set_constraint(LiteRtLmConversationOptionalArgs* args, LiteRtLmConstraintType type, const char* schema)
{
	require(args != nullptr, "Constraint optional args handle lost");
	require(int(type) == 2, "Constraint type lost");
	require(schema && strcmp(schema, R"({"type":"integer","minimum":-100,"maximum":100})") == 0, "Constraint schema changed during replay");
	++*reinterpret_cast<int*>(args);
	return static_cast<LiteRtLmStatusCode>(0);
}

static void test_conversation_output()
{
	lava_reader parent;
	parent.litert_verify_output = true;
	parent.litert_lm_replay = new litert_lm_replay_state();
	litert_lm_replay_state& state = *parent.litert_lm_replay;
	state.initialized = true;
	state.dispatch.json_response_get_string = json_response_text;
	state.dispatch.conversation_optional_args_set_constraint = set_constraint;
	lava_file_reader reader(&parent, "x", size_t(1), 0, uint32_t(1), uint32_t(0), uint8_t(1));
	litert_lm_api_packet& packet = reader.current_litert_lm_packet;
	packet.has_result = true;
	packet.result = 0;
	packet.conversation_parameters["optional_args"] = 3;
	packet.conversation_parameters["constraint_type"] = 2;
	packet.conversation_parameters["constraint_string"] = R"({"type":"integer","minimum":-100,"maximum":100})";
	int calls = 0;
	state.conversation_optional_args_map[3] = reinterpret_cast<LiteRtLmConversationOptionalArgs*>(&calls);
	reader.current.call_id = LITERT_LM_litert_lm_conversation_optional_args_set_constraint;
	retrace_litert_lm_apicall(reader);
	require(calls == 1 && state.verification_failures.empty(), "Constraint setter not replayed");
	state.conversation_optional_args_map.clear();

	reader.current.call_id = LITERT_LM_litert_lm_json_response_get_string;
	packet.conversation_parameters.clear();
	packet.conversation_parameters["response"] = 7;
	packet.conversation_parameters["out_json_present"] = true;
	packet.pointer_flags = 2;
	char text[] = R"({"role":"assistant","content":[{"type":"text","text":"-42"}]})";
	packet.response_text = text;
	packet.conversation_parameters["out_json"] = text;
	state.json_response_map[7] = reinterpret_cast<LiteRtLmJsonResponse*>(text);
	state.json_response_conversations[7] = 11;
	retrace_litert_lm_apicall(reader);
	require(state.verification_failures.empty() && state.verified_responses == 1, "Matching conversation output failed verification");
	require(state.outputs[0]["conversation_index"].asUInt() == 11 && state.outputs[0]["json_response_index"].asUInt() == 7, "Conversation output identifiers missing");
	require(!state.outputs[0].isMember("session_index"), "Conversation confused with a session");
	require(state.outputs[0]["format"] == "conversation_json", "Conversation output format missing");
	require(state.outputs[0]["captured"]["text"] == text && state.outputs[0]["replayed"]["text"] == text, "Conversation JSON not saved");
	packet.response_text = "different";
	retrace_litert_lm_apicall(reader);
	require(state.verification_failures.size() == 1, "Conversation output mismatch not detected");
	require(state.verification_failures[0]["conversation_index"].asUInt() == 11, "Conversation mismatch association missing");
	text[0] = '[';
	require(state.outputs[0]["replayed"]["text"].asString()[0] == '{', "Conversation output retains borrowed storage");
	parent.litert_verify_output = false;
	retrace_litert_lm_apicall(reader);
	require(state.outputs.size() == 3 && state.verified_responses == 2, "Conversation output collection requires verification");
	state.json_response_map.clear();
	packet.result = 3;
	retrace_litert_lm_apicall(reader);
	require(state.outputs[3]["replayed"]["status"].asInt() == 3 && state.outputs[3]["replayed"]["text"].isNull(), "Unavailable conversation response recorded as successful");
	packet.result = 0;
	packet.conversation_parameters["out_json"] = Json::nullValue;
	retrace_litert_lm_apicall(reader);
	require(state.outputs[4]["captured"]["text"].isNull(), "Captured null conversation text changed to an empty string");
}

int main()
{
	lava_reader parent;
	parent.litert_verify_output = true;
	parent.litert_lm_replay = new litert_lm_replay_state();
	litert_lm_replay_state& state = *parent.litert_lm_replay;
	state.initialized = true;
	state.dispatch.responses_get_response_text_at = response_text;
	state.dispatch.responses_get_num_candidates = candidate_count;
	state.response_sessions[7] = 11;
	lava_file_reader reader(&parent, "x", size_t(1), 0, uint32_t(1), uint32_t(0), uint8_t(1));
	litert_lm_api_packet& packet = reader.current_litert_lm_packet;
	packet.has_result = true;
	packet.pointer_flags = 3;
	packet.responses_index = 7;
	packet.response_text = "actual";
	reader.current.packet = 42;
	reader.current.call_id = LITERT_LM_litert_lm_responses_get_response_text_at;
	retrace_litert_lm_apicall(reader);
	require(state.verification_failures.empty(), "Matching response failed verification");
	require(state.report_verification()["status"] == "passed", "Matching output not reported as passed");
	require(state.outputs.size() == 1, "Response not saved");
	require(state.outputs[0]["captured"]["text"] == "actual" && state.outputs[0]["replayed"]["text"] == "actual", "Response text missing from results");
	require(state.outputs[0]["session_index"].asUInt() == 11 && state.outputs[0]["responses_index"].asUInt() == 7, "Response identifiers missing");
	require(state.outputs[0]["packet"].asUInt() == 42, "Response packet missing");

	packet.response_text = "ActuAl";
	retrace_litert_lm_apicall(reader);
	require(state.verification_failures.size() == 1, "Text mismatch not detected");
	require(state.verification_failures[0]["first_differing_byte"].asUInt64() == 0, "Incorrect first differing byte");
	require(state.verification_failures[0]["session_index"].asUInt() == 11, "Session association missing");
	require(state.outputs[1]["captured"]["text"] == "ActuAl" && state.outputs[1]["replayed"]["text"] == "actual", "Differing responses not saved independently");
	packet.candidate_index = 1;
	packet.response_text = "actual";
	retrace_litert_lm_apicall(reader);
	require(state.verification_failures[1]["first_differing_byte"].asUInt64() == 6, "Prefix mismatch not detected");
	packet.candidate_index = 2;
	retrace_litert_lm_apicall(reader);
	require(state.verification_failures[2]["replayed_length"].asUInt64() == 0, "Empty output mismatch not detected");
	packet.response_text.clear();
	retrace_litert_lm_apicall(reader);
	require(state.verification_failures.size() == 3, "Matching empty output failed");
	require(state.outputs[4]["replayed"]["text"].isString() && state.outputs[4]["replayed"]["text"].asString().empty(), "Empty response treated as unavailable");

	// A matching API error is checked without comparing an absent output.
	packet.candidate_index = 4;
	packet.result = 3;
	retrace_litert_lm_apicall(reader);
	require(state.verification_failures.size() == 3, "Matching error failed verification");
	require(state.outputs[5]["captured"]["status"].asInt() == 3 && state.outputs[5]["replayed"]["status"].asInt() == 3, "Failed response statuses missing");
	require(state.outputs[5]["captured"]["text"].isNull() && state.outputs[5]["replayed"]["text"].isNull(), "Failed getter saved invalid text");
	packet.result = 0;
	retrace_litert_lm_apicall(reader);
	require(state.verification_failures.size() == 4, "Status mismatch not detected");
	require(state.verification_unavailable == 1, "Unavailable response counted as verified");

	reader.current.call_id = LITERT_LM_litert_lm_responses_get_num_candidates;
	packet.num_candidates = 2;
	retrace_litert_lm_apicall(reader);
	require(state.verification_failures.size() == 4, "Matching candidate count failed");
	packet.num_candidates = 1;
	retrace_litert_lm_apicall(reader);
	require(state.verification_failures.size() == 5, "Candidate count mismatch not detected");
	Json::Value result = state.report_verification();
	require(result["status"] == "failed", "Mismatches not reported as failed");
	require(result["responses_checked"].asUInt64() == 5, "Incorrect response count");
	require(result["statuses_checked"].asUInt64() == 9, "Incorrect status count");

	parent.litert_verify_output = false;
	retrace_litert_lm_apicall(reader);
	require(state.verified_statuses == 9 && state.verification_failures.size() == 5, "Verification was not opt-in");
	parent.litert_verify_output = true;
	state.dispatch.responses_get_num_candidates = nullptr;
	retrace_litert_lm_apicall(reader);
	require(state.verification_unavailable == 2, "Missing dispatch silently skipped");

	// Output collection is independent of verification and owns the returned text.
	parent.litert_verify_output = false;
	reader.current.call_id = LITERT_LM_litert_lm_responses_get_response_text_at;
	char native_text[] = "actual";
	state.responses_map[7] = reinterpret_cast<LiteRtLmResponses*>(native_text);
	state.dispatch.responses_get_response_text_at = mutable_response_text;
	packet.candidate_index = 0;
	packet.response_text = "captured";
	const Json::ArrayIndex output_index = state.outputs.size();
	retrace_litert_lm_apicall(reader);
	packet.response_text = "changed";
	native_text[0] = 'A';
	require(state.outputs.size() == output_index + 1, "Normal replay did not save output");
	require(state.outputs[output_index]["captured"]["text"] == "captured" && state.outputs[output_index]["replayed"]["text"] == "actual", "Saved response changed after packet or native memory reuse");
	require(state.outputs[output_index]["candidate_index"].asUInt64() == 0, "Candidate index missing");
	require(state.verified_statuses == 9, "Output collection enabled verification");
	state.dispatch.responses_get_response_text_at = nullptr;
	retrace_litert_lm_apicall(reader);
	require(state.outputs[output_index + 1]["replayed"]["status"].isNull() && state.outputs[output_index + 1]["replayed"]["text"].isNull(), "Skipped getter reported as successful");
	state.dispatch.responses_get_response_text_at = response_text;
	packet.pointer_flags = 1;
	packet.result = 3;
	retrace_litert_lm_apicall(reader);
	require(!state.outputs[output_index + 2]["output_requested"].asBool() && state.outputs[output_index + 2]["captured"]["text"].isNull(), "Null output pointer not recorded");
	state.responses_map.clear();
	state.response_sessions.clear();
	retrace_litert_lm_apicall(reader);
	require(state.outputs[output_index + 3]["session_index"].isNull() && state.outputs[output_index + 3]["replayed"]["status"].isNull(), "Unknown session or skipped call not recorded");

	litert_lm_replay_state empty;
	require(empty.report_verification()["status"] == "no_output", "Missing output reported as passed");

	// A failed GPU initialization must allow the captured CPU fallback to run.
	reader.current.call_id = LITERT_LM_litert_lm_engine_create;
	packet.settings_index = 4;
	packet.engine_index = CONTAINER_NULL_VALUE;
	packet.result = 2;
	int engine_calls = 0;
	state.settings_map[4] = reinterpret_cast<LiteRtLmEngineSettings*>(&engine_calls);
	state.dispatch.engine_create = failed_engine;
	state.dispatch.engine_delete = delete_engine;
	retrace_litert_lm_apicall(reader);
	require(engine_calls == 1 && state.engine_map.empty(), "Captured engine failure did not continue without an engine");
	// A backend override can make that same initialization succeed. Dispose of
	// the unused engine rather than storing it under the captured null handle.
	state.dispatch.engine_create = successful_engine;
	parent.litert_verify_output = true;
	const auto failures_before_engine = state.verification_failures.size();
	retrace_litert_lm_apicall(reader);
	require(engine_calls == 3 && state.engine_map.empty(), "Unexpectedly successful engine was not discarded");
	// Tolerating captured failures must not hide status changes in verification.
	require(state.verification_failures.size() == failures_before_engine + 1, "Engine status mismatch not verified");
	state.settings_map.clear();
	test_conversation_output();
	return 0;
}
