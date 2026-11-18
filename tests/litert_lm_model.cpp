#include "litert_lm_model.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static void check(bool condition, const char* message)
{
	if (condition) return;
	fprintf(stderr, "%s\n", message);
	exit(1);
}

static void number(std::vector<uint8_t>& bytes, size_t offset, uint64_t value, size_t count)
{
	for (size_t i = 0; i < count; i++) bytes[offset + i] = (value >> (i * 8)) & 255;
}

static std::vector<uint8_t> model(const char* uuid, uint8_t payload)
{
	std::vector<uint8_t> bytes(256 + 256 * 1024, payload);
	memset(bytes.data(), 0, 256);
	memcpy(bytes.data(), "LITERTLM", 8);
	number(bytes, 8, 1, 4);
	number(bytes, 24, 256, 8);
	// FlatBuffer offsets below are relative to the start of its header at 32.
	std::vector<uint8_t> header(224, 0);
	number(header, 0, 16, 4);
	number(header, 4, 6, 2);
	number(header, 6, 8, 2);
	number(header, 8, uuid ? 4 : 0, 2);
	number(header, 16, 12, 4);
	if (uuid)
	{
		number(header, 20, 28, 4);
		number(header, 32, 6, 2);
		number(header, 34, 8, 2);
		number(header, 36, 4, 2);
		number(header, 48, 16, 4);
		number(header, 52, 12, 4);
		number(header, 64, 1, 4);
		number(header, 68, 28, 4);
		number(header, 80, 10, 2);
		number(header, 82, 16, 2);
		number(header, 84, 4, 2);
		number(header, 86, 8, 2);
		number(header, 88, 12, 2);
		number(header, 96, 16, 4);
		number(header, 100, 28, 4);
		number(header, 104, 9, 1);
		number(header, 108, 52, 4);
		number(header, 128, 4, 4);
		memcpy(header.data() + 132, "uuid", 4);
		number(header, 144, 6, 2);
		number(header, 146, 8, 2);
		number(header, 148, 4, 2);
		number(header, 160, 16, 4);
		number(header, 164, 12, 4);
		check(strlen(uuid) <= 36, "Fixture UUID too long");
		number(header, 176, strlen(uuid), 4);
		memcpy(header.data() + 180, uuid, strlen(uuid));
	}
	memcpy(bytes.data() + 32, header.data(), header.size());
	return bytes;
}

static void write_model(const std::string& path, const std::vector<uint8_t>& bytes)
{
	FILE* file = fopen(path.c_str(), "wb");
	check(file != nullptr, "Cannot create model fixture");
	check(fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size(), "Cannot write model fixture");
	check(fclose(file) == 0, "Cannot close model fixture");
}

int main()
{
	char temporary[] = "/tmp/lavatube-model-XXXXXX";
	check(mkdtemp(temporary) != nullptr, "Cannot create test directory");
	const std::string base = temporary;
	const std::string models = base + "/models";
	check(mkdir(models.c_str(), 0700) == 0, "Cannot create models directory");
	check(mkdir((models + "/gemma3").c_str(), 0700) == 0, "Cannot create model directory");
	check(mkdir((models + "/gemma4").c_str(), 0700) == 0, "Cannot create model directory");
	const std::string first = models + "/gemma3/model.litertlm";
	const std::string second = models + "/gemma4/model.litertlm";
	const char* uuid = "2fa073f5-2d5e-44ff-8bb9-64d926dc40e2";
	write_model(first, model(nullptr, 1));
	write_model(second, model(uuid, 2));
	litert_lm_model_identity legacy, modern, copy;
	check(inspect_litert_lm_model(first, legacy), "Cannot inspect legacy model");
	check(legacy.uuid.empty(), "Legacy fixture unexpectedly has UUID");
	check(inspect_litert_lm_model(second, modern), "Cannot inspect UUID model");
	check(modern.uuid == uuid, "Incorrect UUID extraction");
	check(modern.model_id == "gemma4", "Incorrect directory hint");
	check(legacy.size == modern.size, "Fixtures must have identical sizes");
	check(setenv("LITERT_LM_DIR", base.c_str(), 1) == 0, "Cannot set model root");
	std::string found, error;
	check(find_litert_lm_model("model.litertlm", modern, nullptr, found, error) && found == second,
		"Default model search selected wrong same-named model");
	check(!find_litert_lm_model("model.litertlm", modern, first.c_str(), found, error)
		&& error.find("does not match") != std::string::npos, "Wrong explicit model was accepted");
	check(!find_litert_lm_model("model.litertlm", modern, "/nonexistent/lavatube-model", found, error),
		"Missing explicit model silently fell back");
	check(find_litert_lm_model("model.litertlm", legacy, models.c_str(), found, error) && found == first,
		"Legacy model search failed");
	const std::string renamed = models + "/gemma3/renamed.litertlm";
	write_model(renamed, model(uuid, 2));
	check(inspect_litert_lm_model(renamed, copy) && copy.fingerprint == modern.fingerprint,
		"Moving model changed identity");
	check(unlink(second.c_str()) == 0, "Cannot remove original model");
	check(find_litert_lm_model("model.litertlm", modern, nullptr, found, error) && found == renamed,
		"Renamed model was not found");
	check(unsetenv("LITERT_LM_DIR") == 0 && setenv("HOME", base.c_str(), 1) == 0,
		"Cannot set fallback model root");
	check(mkdir((base + "/.litert-lm").c_str(), 0700) == 0, "Cannot create home fixture");
	check(symlink(models.c_str(), (base + "/.litert-lm/models").c_str()) == 0, "Cannot link home models");
	check(find_litert_lm_model("model.litertlm", modern, nullptr, found, error), "HOME model search failed");
	write_model(renamed, model(uuid, 3));
	check(!find_litert_lm_model("model.litertlm", modern, renamed.c_str(), found, error),
		"Changed weights with unchanged UUID were accepted");
	write_model(first, model(nullptr, 4));
	check(!find_litert_lm_model("model.litertlm", legacy, first.c_str(), found, error),
		"Changed legacy model was accepted");
	std::vector<uint8_t> malformed = model(uuid, 2);
	number(malformed, 32, UINT32_MAX, 4);
	write_model(renamed, malformed);
	check(!inspect_litert_lm_model(renamed, copy), "Out-of-bounds FlatBuffer accepted");
	malformed = model(uuid, 2);
	number(malformed, 32 + 64, UINT32_MAX, 4);
	write_model(renamed, malformed);
	check(!inspect_litert_lm_model(renamed, copy), "Out-of-bounds metadata vector accepted");
	malformed.resize(16);
	write_model(renamed, malformed);
	check(!inspect_litert_lm_model(renamed, copy), "Truncated model accepted");
	check(!inspect_litert_lm_model(models, copy), "Directory accepted as model");
	unlink(first.c_str());
	unlink(renamed.c_str());
	unlink((base + "/.litert-lm/models").c_str());
	rmdir((base + "/.litert-lm").c_str());
	rmdir((models + "/gemma3").c_str());
	rmdir((models + "/gemma4").c_str());
	rmdir(models.c_str());
	rmdir(base.c_str());
	return 0;
}
