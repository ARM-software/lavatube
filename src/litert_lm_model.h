#pragma once

#include <cstdint>
#include <string>

struct litert_lm_model_identity
{
	std::string model_id;
	std::string uuid;
	uint64_t size = 0;
	std::string fingerprint;
};

// Reads the FlatBuffer header and at most three 64 KiB samples of the payload.
bool inspect_litert_lm_model(const std::string& path, litert_lm_model_identity& identity);
bool find_litert_lm_model(const std::string& filename, const litert_lm_model_identity& identity,
	const char* override_path, std::string& path, std::string& error);
