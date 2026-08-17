#pragma once

#include <stdio.h>
#include <sys/types.h>

#include <stdint.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class system_log_collector
{
public:
	system_log_collector();
	~system_log_collector();

	bool start(const std::string& trace_filename);
	void stop();
	FILE* output() const { return mOutput; }
	bool available(std::string& warning, std::string& error) const;

private:
	enum class collector_status : uint8_t
	{
		stopped = 0,
		starting = 1,
		ready = 2,
		failed = 3,
	};

	struct probe_result
	{
		bool available = false;
		std::string warning;
		std::string error;
	};

	static probe_result probe(const char* source);
	static std::string filter_pattern(const std::string& trace_filename);
	static std::string escape_pattern_literal(const std::string& value);
	static std::string single_line(const std::string& value);
	bool spawn_follower(const std::vector<std::string>& sources, const std::string& pattern);
	void reader_main();
	void read_pipe(int& fd, bool output_pipe);
	bool append_output_lines();
	void append_follower_warning(const std::string& detail);
	void publish_follower_warning_lines();

	FILE* mOutput = nullptr;
	std::atomic<pid_t> mPid{ -1 };
	int mStdoutFd = -1;
	int mStderrFd = -1;
	std::thread mThread;
	std::atomic<collector_status> mStatus{ collector_status::stopped };
	std::atomic_bool mStopRequested{ false };
	mutable std::mutex mWarningMutex;
	std::string mWarning;
	std::string mError;
	std::string mPendingOutput;
	std::string mChildError;
	std::string mPendingWarning;
};
