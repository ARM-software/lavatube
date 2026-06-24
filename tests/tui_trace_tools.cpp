#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <string>

#include "packfile.h"
#include "tui_trace_tools.h"

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
	const std::string pack = directory + ".vk";

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

int main()
{
	test_validate_and_tools();
	return 0;
}
