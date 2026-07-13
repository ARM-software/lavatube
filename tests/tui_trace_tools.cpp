#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/stat.h>

#include <string>
#include <thread>
#include <vector>

#include "packfile.h"
#include "tui_trace_tools.h"
#include "util.h"

#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"

struct fake_service_state
{
	int listen_fd = -1;
	std::vector<std::string> commands;
	unsigned expected_connections = 0;
};

static int make_fake_listener(int& port)
{
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	assert(fd != -1);

	const int reuse = 1;
	assert(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0);

	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	assert(bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
	assert(listen(fd, 8) == 0);

	socklen_t len = sizeof(addr);
	assert(getsockname(fd, (struct sockaddr*)&addr, &len) == 0);
	port = ntohs(addr.sin_port);
	return fd;
}

static std::string fake_response(const std::string& command)
{
	if (command == "status") return "PAUSED\n";
	if (command == "info objects") return "object_type\tcount\nVkBuffer\t2\n";
	if (command == "info suballocator") return "| Device | Heap | Total Bytes |\n|--------|------|-------------|\n| 0      | 0    | 33554432    |\n";
	if (command == "step calls 2") return "PAUSED packet=10 api_calls=4 vkQueueSubmit\n";
	if (command == "show VkBuffer 1") return "{ \"size\" : 32 }\n";
	if (command == "parameters") return "{ \"command\" : \"vkQueueSubmit\" }\n";
	return "ERROR\n";
}

static void fake_service_thread(fake_service_state* state)
{
	for (unsigned i = 0; i < state->expected_connections; i++)
	{
		const int client_fd = accept(state->listen_fd, nullptr, nullptr);
		assert(client_fd != -1);
		const std::string command = lava_tcp_receive_line(client_fd);
		state->commands.push_back(command);
		const std::string response = fake_response(command);
		assert(lava_tcp_send_all(client_fd, response));
		close(client_fd);
	}
	close(state->listen_fd);
}

#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"

static void write_file(const std::string& path, const char* data)
{
	int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0664);
	assert(fd != -1);
	const size_t len = strlen(data);
	size_t written = 0;
	while (written < len)
	{
		const ssize_t result = write(fd, data + written, len - written);
		assert(result > 0);
		written += result;
	}
	const int close_result = close(fd);
	assert(close_result == 0);
}

static std::string make_trace()
{
	char path[] = "/tmp/lavatube-tui-test-XXXXXX";
	char* dir = mkdtemp(path);
	assert(dir != nullptr);
	const std::string directory = dir;
	const std::string pack = directory + ".api";

	write_file(directory + "/limits.json", "{ \"VkBuffer\": 2, \"VkImage\": 0, \"VkDevice\": 1 }\n");
	write_file(directory + "/tracking.json", "{ \"VkBuffer\": [ { \"index\": 0, \"size\": 16 }, { \"index\": 1, \"size\": 32 } ] }\n");
	write_file(directory + "/metadata.json", "{ \"threads\": 1, \"global_frames\": 2 }\n");
	write_file(directory + "/frames_0.json", "{ \"frames\": [ { \"global_frame\": 0, \"position\": 0 }, { \"global_frame\": 1, \"position\": 10 } ], \"uncompressed_size\": 20, \"thread_name\": \"main\" }\n");

	assert(pack_directory(pack, directory, false));
	return pack;
}

static void cleanup_trace(const std::string& pack)
{
	const std::string directory = pack.substr(0, pack.size() - 3);
	remove((directory + "/limits.json").c_str());
	remove((directory + "/tracking.json").c_str());
	remove((directory + "/metadata.json").c_str());
	remove((directory + "/frames_0.json").c_str());
	rmdir(directory.c_str());
	remove(pack.c_str());
}

static void test_validate_and_tools()
{
	const std::string pack = make_trace();
	tui_trace_tools tools(pack);
	std::string error;
	assert(tools.validate(error));

	tui_tool_result result = tools.execute("list_objects_created", "{}");
	assert(result.ok);
	assert(result.output.find("VkBuffer\t2") != std::string::npos);
	assert(result.output.find("VkDevice\t1") != std::string::npos);
	assert(result.output.find("VkImage") == std::string::npos);

	result = tools.execute("get_frame_meta", "{ \"thread\": 0, \"frame\": 1 }");
	assert(result.ok);
	assert(result.output.find("\"global_frame\":1") != std::string::npos);
	assert(result.output.find("\"position\":10") != std::string::npos);

	result = tools.execute("get_thread_meta", "{ \"thread\": 0 }");
	assert(result.ok);
	assert(result.output.find("uncompressed_size") != std::string::npos);
	assert(result.output.find("frames") == std::string::npos);

	result = tools.execute("get_object_meta", "{ \"type\": \"VkBuffer\", \"index\": 1 }");
	assert(result.ok);
	assert(result.output.find("\"size\":32") != std::string::npos);

	result = tools.execute("get_object_meta", "{ \"type\": \"VkBuffer\", \"index\": 10 }");
	assert(!result.ok);
	assert(result.output.find("\"ok\":false") != std::string::npos);

	cleanup_trace(pack);
}

static void test_service_tools()
{
	int port = 0;
	fake_service_state state;
	state.listen_fd = make_fake_listener(port);
	state.expected_connections = 6;
	std::thread thread(fake_service_thread, &state);

	tui_trace_tools_options options;
	options.replay_service = true;
	options.hostname = "127.0.0.1";
	options.port = port;
	tui_trace_tools tools(options);

	std::string error;
	assert(tools.validate(error));

	tui_tool_result result = tools.execute("list_objects_created", "{}");
	assert(result.ok);
	assert(result.output.find("VkBuffer\t2") != std::string::npos);

	result = tools.execute("step_replay", "{ \"unit\": \"calls\", \"count\": 2 }");
	assert(result.ok);
	assert(result.output.find("vkQueueSubmit") != std::string::npos);

	result = tools.execute("get_object_meta", "{ \"type\": \"VkBuffer\", \"index\": 1 }");
	assert(result.ok);
	assert(result.output.find("\"size\"") != std::string::npos);

	result = tools.execute("get_suballocator_info", "{}");
	assert(result.ok);
	assert(result.output.find("Total Bytes") != std::string::npos);

	result = tools.execute("get_current_call_parameters", "{}");
	assert(result.ok);
	assert(result.output.find("vkQueueSubmit") != std::string::npos);

	thread.join();
	assert(state.commands.size() == 6);
	assert(state.commands[0] == "status");
	assert(state.commands[1] == "info objects");
	assert(state.commands[2] == "step calls 2");
	assert(state.commands[3] == "show VkBuffer 1");
	assert(state.commands[4] == "info suballocator");
	assert(state.commands[5] == "parameters");
}

int main()
{
	test_validate_and_tools();
	test_service_tools();
	return 0;
}
