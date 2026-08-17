#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/stat.h>

#include <deque>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <re2/re2.h>

#include "lavatube.h"

static bool verbose = false;

struct log_tail_options
{
	std::string expression = ".*";
	uint32_t limit = 10;
	uint64_t since = 0;
	bool update = true;
};

void usage()
{
	printf("lava-cli %d.%d.%d-" RELTYPE "\n", LAVATUBE_VERSION_MAJOR, LAVATUBE_VERSION_MINOR, LAVATUBE_VERSION_PATCH);
	printf("lava-cli [options] <command> [args]\n");
	printf("\n");
	printf("Options:\n");
	printf("    -h/--help                This help\n");
	printf("    -v/--verbose             Verbose output\n");
	printf("    -P/--port PORT           Port number (default %d)\n", (int)p__port);
	printf("    -H/--host HOST           Host name\n");
#ifndef NDEBUG
	printf("    -d/--debug level         Set debug level [0,1,2,3]\n");
	printf("    -df/--debugfile FILE     Output debug output to the given file\n");
#endif
	printf("\n");
	printf("Replay control:\n");
	printf("    status                   Show replay state. Outputs RUNNING, DONE, PAUSED, or current paused packet/call.\n");
	printf("    continue                 Resume replay and wait until completion.\n");
	printf("    stop                     Stop the replay.\n");
	printf("    diagnose deadlock        Detect replay thread wait cycles and blocking GPU waits.\n");
	printf("    diagnose device          Wait for replay devices to become idle and report errors.\n");
	printf("    set debug LEVEL          Set replay debug level (zero is the least verbose) [0,1,2,3].\n");
	printf("    set blackhole BOOL       Set replay blackhole mode (submit empty commandbuffers) [true,false], default is false.\n");
	printf("    set idle-check BOOL      Wait for GPU idle after control commands [true,false], default is true.\n");
	printf("    set isolate-thread BOOL  Run only the selected replay thread [true,false], default is false.\n");
#ifndef NDEBUG
	printf("    self-test                Run internal consistency assert checks.\n");
#endif
	printf("    thread INDEX             Select which traced thread receives step and goto commands.\n");
	printf("    step                     Advance one packet.\n");
	printf("    step packets N           Advance N packets.\n");
	printf("    step calls N             Advance N Vulkan API calls.\n");
	printf("    goto PACKET              Continue until thread-relative packet number PACKET.\n");
	printf("    goto NAME                Continue until next Vulkan command NAME, e.g. vkQueueSubmit.\n");
	printf("\n");
	printf("Replay logs:\n");
	printf("    log update               Append new replay logs to the local host/port cache.\n");
	printf("    log tail [REGEX] [limit=N] [since=LINE] [update=on|off]\n");
	printf("                             Print matching cached lines; update=off still validates the replay session.\n");
	printf("    syslog update            Append filtered system-journal entries to a separate local cache.\n");
	printf("    syslog tail [REGEX] [limit=N] [since=LINE] [update=on|off]\n");
	printf("                             Print matching cached system-journal lines.\n");
	printf("\n");
	printf("Call inspection:\n");
	printf("    parameters               Print JSON parameters for the currently paused Vulkan call.\n");
	printf("    instrument [detailed]    Instrument the command buffer after a paused vkBeginCommandBuffer.\n");
	printf("    show instrumentation N   Print cached VK_ARM_shader_instrumentation data for command buffer N.\n");
	printf("\n");
	printf("Trace metadata:\n");
	printf("    show TYPE INDEX          Print JSON metadata for replay object TYPE with INDEX.\n");
	printf("    info objects             Print object creation counts from limits metadata.\n");
	printf("    info trace               Print JSON information about the replayed trace file.\n");
	printf("    info threads             List traced threads.\n");
	printf("    info thread THREAD       Print JSON metadata for THREAD.\n");
	printf("    info frame THREAD FRAME  Print JSON metadata for FRAME in THREAD.\n");
	printf("    info memory              Print current Vulkan memory heap usage and budgets.\n");
	printf("    info suballocator        Print current suballocator heap internals.\n");
	printf("\n");
	printf("    Long running commands keep 'status', log updates, 'info trace', 'info threads', 'diagnose deadlock', and 'stop' responsive; other concurrent commands fail.\n");
	exit(-1);
}

static bool parse_u64(const std::string& text, uint64_t& value)
{
	if (text.empty() || text[0] == '-') return false;
	char* end = nullptr;
	errno = 0;
	const unsigned long long parsed = strtoull(text.c_str(), &end, 10);
	if (errno != 0 || end == text.c_str() || *end != '\0') return false;
	value = (uint64_t)parsed;
	return true;
}

static bool parse_log_tail_options(const std::vector<std::string>& command, log_tail_options& options)
{
	bool have_expression = false;
	bool have_limit = false;
	bool have_since = false;
	bool have_update = false;

	for (size_t i = 2; i < command.size(); i++)
	{
		const std::string& argument = command[i];
		if (argument.rfind("limit=", 0) == 0)
		{
			uint64_t limit = 0;
			if (have_limit || !parse_u64(argument.substr(6), limit) || limit == 0 || limit > 1000)
			{
				fprintf(stderr, "ERROR limit must be between 1 and 1000\n");
				return false;
			}
			have_limit = true;
			options.limit = (uint32_t)limit;
		}
		else if (argument.rfind("since=", 0) == 0)
		{
			if (have_since || !parse_u64(argument.substr(6), options.since))
			{
				fprintf(stderr, "ERROR since must be a non-negative line number\n");
				return false;
			}
			have_since = true;
		}
		else if (argument.rfind("update=", 0) == 0)
		{
			if (have_update)
			{
				fprintf(stderr, "ERROR update was specified more than once\n");
				return false;
			}
			const std::string value = argument.substr(7);
			if (value == "on") options.update = true;
			else if (value == "off") options.update = false;
			else
			{
				fprintf(stderr, "ERROR update must be on or off\n");
				return false;
			}
			have_update = true;
		}
		else if (argument.find('=') != std::string::npos)
		{
			fprintf(stderr, "ERROR unknown %s tail option: %s\n", command[0].c_str(), argument.c_str());
			return false;
		}
		else
		{
			if (have_expression)
			{
				fprintf(stderr, "ERROR %s tail accepts only one regular expression\n", command[0].c_str());
				return false;
			}
			have_expression = true;
			options.expression = argument;
		}
	}
	return true;
}

static std::string command_string(const std::vector<std::string>& command)
{
	std::string keyword;
	for (const std::string& argument : command)
	{
		if (!keyword.empty()) keyword += " ";
		keyword += argument;
	}
	return keyword;
}

static std::string remote_command(const std::string& hostname, int port, const std::string& keyword, size_t max_response)
{
	const int fd = lava_tcp_connect(hostname, port);
	if (!lava_tcp_send_all(fd, keyword + "\n"))
	{
		close(fd);
		DIE("Failed to send %s command to %s:%d: %s", keyword.c_str(), hostname.c_str(), port, strerror(errno));
	}
	const std::string response = lava_tcp_receive_all(fd, max_response);
	close(fd);
	return response;
}

static uint64_t log_cache_hash(const std::string& hostname, int port)
{
	uint64_t value = UINT64_C(1469598103934665603);
	const std::string key = hostname + ":" + std::to_string(port);
	for (unsigned char c : key)
	{
		value ^= c;
		value *= UINT64_C(1099511628211);
	}
	return value;
}

static bool prepare_log_cache_path(const std::string& hostname, int port, const std::string& stream, std::string& path)
{
	std::string directory;
	const char* runtime_directory = getenv("XDG_RUNTIME_DIR");
	if (runtime_directory && runtime_directory[0] != '\0')
	{
		directory = std::string(runtime_directory) + "/lavatube";
	}
	else
	{
		directory = "/tmp/lavatube-" + std::to_string((uint64_t)getuid());
	}

	if (mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST)
	{
		fprintf(stderr, "ERROR failed to create log cache directory %s: %s\n", directory.c_str(), strerror(errno));
		return false;
	}

	struct stat directory_status = {};
	if (stat(directory.c_str(), &directory_status) != 0 || !S_ISDIR(directory_status.st_mode) || directory_status.st_uid != getuid())
	{
		fprintf(stderr, "ERROR log cache directory %s is not a private directory owned by this user\n", directory.c_str());
		return false;
	}
	if ((directory_status.st_mode & 077) != 0 && chmod(directory.c_str(), 0700) != 0)
	{
		fprintf(stderr, "ERROR failed to make log cache directory %s private: %s\n", directory.c_str(), strerror(errno));
		return false;
	}

	char filename[40];
	snprintf(filename, sizeof(filename), "/%016" PRIx64 ".%s", log_cache_hash(hostname, port), stream.c_str());
	path = directory + filename;
	return true;
}

static bool write_private_file(const std::string& path, const std::string& data)
{
	const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0) return false;
	if (fchmod(fd, 0600) != 0)
	{
		close(fd);
		return false;
	}

	const char* position = data.data();
	size_t remaining = data.size();
	while (remaining > 0)
	{
		const ssize_t written = write(fd, position, remaining);
		if (written < 0)
		{
			if (errno == EINTR) continue;
			close(fd);
			return false;
		}
		if (written == 0)
		{
			close(fd);
			return false;
		}
		position += written;
		remaining -= (size_t)written;
	}
	return close(fd) == 0;
}

static bool synchronize_log_session(const std::string& path, const std::string& session)
{
	std::string cached_session;
	std::ifstream session_input(path + ".session");
	if (session_input) std::getline(session_input, cached_session);
	if (cached_session == session) return true;

	if (!write_private_file(path, "") || !write_private_file(path + ".session", session + "\n"))
	{
		fprintf(stderr, "ERROR failed to reset the local log cache for the current replay session: %s\n", strerror(errno));
		return false;
	}
	return true;
}

static bool parse_log_session_response(const std::string& response, std::string& session)
{
	std::istringstream header(response);
	std::string status;
	std::string extra;
	header >> status >> session;
	return header && status == "OK" && !session.empty() && !(header >> extra);
}

static bool consume_response_warnings(std::string& response, bool& warning_reported)
{
	while (response.rfind("WARNING ", 0) == 0)
	{
		const size_t newline = response.find('\n');
		if (newline == std::string::npos) return false;
		if (!warning_reported) fprintf(stderr, "%s", response.substr(0, newline + 1).c_str());
		warning_reported = true;
		response.erase(0, newline + 1);
	}
	return true;
}

static bool validate_log_session(const std::string& hostname, int port, const std::string& stream, const std::string& path)
{
	std::string response = remote_command(hostname, port, stream + " session", 64 * 1024);
	bool warning_reported = false;
	if (!consume_response_warnings(response, warning_reported))
	{
		fprintf(stderr, "ERROR malformed %s warning response\n", stream.c_str());
		return false;
	}
	if (response.rfind("ERROR", 0) == 0)
	{
		fprintf(stderr, "%s", response.c_str());
		return false;
	}
	std::string session;
	if (!parse_log_session_response(response, session))
	{
		fprintf(stderr, "ERROR malformed %s session response\n", stream.c_str());
		return false;
	}
	return synchronize_log_session(path, session);
}

static bool parse_log_update_response(const std::string& response, std::string& session, uint64_t& start, uint64_t& end, uint64_t& snapshot_end, size_t& payload_offset)
{
	const size_t newline = response.find('\n');
	if (newline == std::string::npos) return false;

	std::istringstream header(response.substr(0, newline));
	std::string status;
	std::string extra;
	header >> status >> session >> start >> end >> snapshot_end;
	if (!header || status != "OK" || (header >> extra)) return false;
	if (session.empty()) return false;
	if (start > end || end > snapshot_end) return false;
	payload_offset = newline + 1;
	return response.size() - payload_offset == end - start;
}

static bool append_log_chunk(const std::string& path, const std::string& response, uint64_t start, uint64_t end, size_t payload_offset)
{
	int flags = O_WRONLY | O_CREAT | O_CLOEXEC;
	if (start == 0) flags |= O_TRUNC;
	else flags |= O_APPEND;
	const int fd = open(path.c_str(), flags, 0600);
	if (fd < 0)
	{
		fprintf(stderr, "ERROR failed to open local log cache %s: %s; restart replay before retrying to avoid a log gap\n", path.c_str(), strerror(errno));
		return false;
	}

	struct stat status = {};
	if (fstat(fd, &status) != 0 || status.st_size < 0 || (uint64_t)status.st_size != start)
	{
		fprintf(stderr, "ERROR local log cache does not match the replay cursor; restart replay before retrying to avoid a log gap\n");
		close(fd);
		return false;
	}
	if (fchmod(fd, 0600) != 0)
	{
		fprintf(stderr, "ERROR failed to make local log cache private: %s; restart replay before retrying to avoid a log gap\n", strerror(errno));
		close(fd);
		return false;
	}

	const char* data = response.data() + payload_offset;
	size_t remaining = (size_t)(end - start);
	while (remaining > 0)
	{
		const ssize_t written = write(fd, data, remaining);
		if (written < 0)
		{
			if (errno == EINTR) continue;
			fprintf(stderr, "ERROR failed to append local log cache: %s; restart replay before retrying to avoid a log gap\n", strerror(errno));
			close(fd);
			return false;
		}
		if (written == 0)
		{
			fprintf(stderr, "ERROR failed to append local log cache; restart replay before retrying to avoid a log gap\n");
			close(fd);
			return false;
		}
		data += written;
		remaining -= (size_t)written;
	}
	if (close(fd) != 0)
	{
		fprintf(stderr, "ERROR failed to close local log cache: %s; restart replay before retrying to avoid a log gap\n", strerror(errno));
		return false;
	}
	return true;
}

static bool update_log_cache(const std::string& hostname, int port, const std::string& stream, const std::string& path)
{
	std::string expected_session;
	uint64_t snapshot_end = 0;
	uint64_t end = 0;
	bool warning_reported = false;
	do
	{
		std::string response = remote_command(hostname, port, stream + " update", 512 * 1024 + 64 * 1024);
		if (!consume_response_warnings(response, warning_reported))
		{
			fprintf(stderr, "ERROR malformed %s warning response\n", stream.c_str());
			return false;
		}
		if (response.rfind("ERROR", 0) == 0)
		{
			fprintf(stderr, "%s", response.c_str());
			return false;
		}

		uint64_t start = 0;
		std::string session;
		size_t payload_offset = 0;
		if (!parse_log_update_response(response, session, start, end, snapshot_end, payload_offset))
		{
			fprintf(stderr, "ERROR malformed or truncated %s update response; restart replay before retrying to avoid a log gap\n", stream.c_str());
			return false;
		}
		if (expected_session.empty())
		{
			expected_session = session;
			if (!synchronize_log_session(path, session)) return false;
		}
		else if (session != expected_session)
		{
			fprintf(stderr, "ERROR replay service restarted during log update; restart replay before retrying to avoid a log gap\n");
			return false;
		}
		if (!append_log_chunk(path, response, start, end, payload_offset)) return false;
	} while (end < snapshot_end);
	return true;
}

static bool tail_log_cache(const std::string& path, const std::string& stream, const log_tail_options& options)
{
	re2::RE2 expression(options.expression);
	if (!expression.ok())
	{
		fprintf(stderr, "ERROR invalid regular expression: %s\n", expression.error().c_str());
		return false;
	}

	std::ifstream input(path);
	if (!input)
	{
		fprintf(stderr, "ERROR no local %s cache; run 'lava-cli %s update' first\n", stream.c_str(), stream.c_str());
		return false;
	}

	std::deque<std::pair<uint64_t, std::string>> matches;
	std::string line;
	uint64_t line_number = 0;
	while (std::getline(input, line))
	{
		line_number++;
		if (line_number <= options.since) continue;
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (!re2::RE2::PartialMatch(line, expression)) continue;
		matches.emplace_back(line_number, line);
		if (matches.size() > options.limit) matches.pop_front();
	}
	if (input.bad())
	{
		fprintf(stderr, "ERROR failed to read local log cache %s\n", path.c_str());
		return false;
	}

	for (const auto& match : matches)
	{
		printf("%" PRIu64 ":%s\n", match.first, match.second.c_str());
	}
	return true;
}

int main(int argc, char **argv)
{
	std::string hostname = "localhost";
	std::vector<std::string> command;
	int port = p__port;
	int remaining = argc - 1; // zeroth is name of program

	for (int i = 1; i < argc; i++)
	{
		if (match(argv[i], "-h", "--help", remaining))
		{
			usage();
		}
		else if (match(argv[i], "-d", "--debug", remaining))
		{
			p__debug_level = get_int(argv[++i], remaining);
		}
		else if (match(argv[i], "-P", "--port", remaining))
		{
			port = get_int(argv[++i], remaining);
		}
		else if (match(argv[i], "-H", "--host", remaining))
		{
			hostname = get_str(argv[++i], remaining);
		}
		else if (match(argv[i], "-v", "--verbose", remaining))
		{
			verbose = true;
		}
		else if (match(argv[i], "-df", "--debugfile", remaining))
		{
			std::string val = get_str(argv[++i], remaining);
			if (p__debug_destination != stdout) ABORT("We already have a different debug file destination!");
			p__debug_destination = fopen(val.c_str(), "w");
		}
		else
		{
			command.push_back(get_str(argv[i], remaining));
			while (remaining > 0)
			{
				command.push_back(get_str(argv[++i], remaining));
			}
		}
	}

	if (command.empty()) usage();
	const bool log_stream = !command.empty() && (command[0] == "log" || command[0] == "syslog");
	const bool log_update = log_stream && command.size() >= 2 && command[1] == "update";
	const bool log_tail = log_stream && command.size() >= 2 && command[1] == "tail";
	if (log_update || log_tail)
	{
		if (log_update && command.size() != 2)
		{
			fprintf(stderr, "ERROR %s update does not accept arguments\n", command[0].c_str());
			return 1;
		}

		log_tail_options options;
		if (log_tail && !parse_log_tail_options(command, options)) return 1;
		std::string cache_path;
		if (!prepare_log_cache_path(hostname, port, command[0], cache_path)) return 1;

		if (verbose) printf("Connecting to %s:%d\n", hostname.c_str(), port);
		if (log_update || options.update)
		{
			if (!update_log_cache(hostname, port, command[0], cache_path)) return 1;
		}
		else if (!validate_log_session(hostname, port, command[0], cache_path)) return 1;
		if (log_update)
		{
			printf("DONE\n");
			return 0;
		}
		return tail_log_cache(cache_path, command[0], options) ? 0 : 1;
	}

	if (verbose)
	{
		printf("Connecting to %s:%d\n", hostname.c_str(), port);
	}

	const std::string response = remote_command(hostname, port, command_string(command), 1024 * 1024);
	printf("%s", response.c_str());
	if (response.empty() || response.rfind("ERROR", 0) == 0 || response == "DEVICE_LOST\n" || response == "DEVICE_LOST") return 1;

	return 0;
}
