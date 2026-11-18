#include "litert_lm_replay.h"
#include "read.h"

#include <cstdio>
#include <cstdlib>

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
	reader.current.call_id = LITERT_LM_litert_lm_responses_get_response_text_at;
	retrace_litert_lm_apicall(reader);
	require(state.verification_failures.empty(), "Matching response failed verification");
	require(state.report_verification()["status"] == "passed", "Matching output not reported as passed");

	packet.response_text = "ActuAl";
	retrace_litert_lm_apicall(reader);
	require(state.verification_failures.size() == 1, "Text mismatch not detected");
	require(state.verification_failures[0]["first_differing_byte"].asUInt64() == 0, "Incorrect first differing byte");
	require(state.verification_failures[0]["session_index"].asUInt() == 11, "Session association missing");
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

	// A matching API error is checked without comparing an absent output.
	packet.candidate_index = 4;
	packet.result = 3;
	retrace_litert_lm_apicall(reader);
	require(state.verification_failures.size() == 3, "Matching error failed verification");
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

	litert_lm_replay_state empty;
	require(empty.report_verification()["status"] == "no_output", "Missing output reported as passed");
	return 0;
}
