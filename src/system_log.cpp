#include "system_log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <sstream>

extern char** environ;

system_log_collector::system_log_collector() = default;

system_log_collector::~system_log_collector()
{
	stop();
	if (mOutput) fclose(mOutput);
}

std::string system_log_collector::single_line(const std::string& value)
{
	std::string result;
	bool previous_space = false;
	for (char c : value)
	{
		const bool whitespace = c == '\n' || c == '\r' || c == '\t';
		if (whitespace)
		{
			if (!result.empty() && !previous_space) result += ' ';
			previous_space = true;
		}
		else
		{
			result += c;
			previous_space = c == ' ';
		}
	}
	while (!result.empty() && result.back() == ' ') result.pop_back();
	return result;
}

#ifndef VK_USE_PLATFORM_ANDROID_KHR
system_log_collector::probe_result system_log_collector::probe(const char* source)
{
	probe_result result;
	int error_pipe[2] = { -1, -1 };
	if (pipe2(error_pipe, O_CLOEXEC) != 0)
	{
		result.error = std::string("failed to create journalctl error pipe: ") + strerror(errno);
		return result;
	}

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_adddup2(&actions, error_pipe[1], STDERR_FILENO);
	posix_spawn_file_actions_addclose(&actions, error_pipe[0]);
	posix_spawn_file_actions_addclose(&actions, error_pipe[1]);

	char* arguments[] = {
		(char*)"journalctl",
		(char*)source,
		(char*)"--lines=0",
		(char*)"--no-pager",
		nullptr,
	};
	pid_t pid = -1;
	const int spawn_result = posix_spawnp(&pid, "journalctl", &actions, nullptr, arguments, environ);
	posix_spawn_file_actions_destroy(&actions);
	close(error_pipe[1]);
	if (spawn_result != 0)
	{
		close(error_pipe[0]);
		result.error = std::string("failed to start journalctl: ") + strerror(spawn_result);
		return result;
	}

	std::string child_error;
	char buffer[4096];
	while (true)
	{
		const ssize_t bytes = read(error_pipe[0], buffer, sizeof(buffer));
		if (bytes < 0)
		{
			if (errno == EINTR) continue;
			break;
		}
		if (bytes == 0) break;
		if (child_error.size() < 16384)
		{
			const size_t retained = std::min((size_t)bytes, (size_t)16384 - child_error.size());
			child_error.append(buffer, retained);
		}
	}
	close(error_pipe[0]);

	int status = 0;
	pid_t wait_result = -1;
	do
	{
		wait_result = waitpid(pid, &status, 0);
	} while (wait_result < 0 && errno == EINTR);
	if (wait_result < 0)
	{
		result.error = std::string("failed to wait for journalctl: ") + strerror(errno);
		return result;
	}
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
	{
		result.available = true;
		return result;
	}

	std::ostringstream message;
	if (WIFEXITED(status)) message << "journalctl exited with status " << WEXITSTATUS(status);
	else if (WIFSIGNALED(status)) message << "journalctl terminated by signal " << WTERMSIG(status);
	else message << "journalctl failed";
	const std::string detail = single_line(child_error);
	if (!detail.empty()) message << ": " << detail;
	result.error = message.str();
	return result;
}

std::string system_log_collector::escape_pattern_literal(const std::string& value)
{
	const std::string special = R"(\.^$|()[]{}*+?)";
	std::string escaped;
	for (char c : value)
	{
		if (special.find(c) != std::string::npos) escaped += '\\';
		escaped += c;
	}
	return escaped;
}

std::string system_log_collector::filter_pattern(const std::string& trace_filename)
{
	std::vector<std::string> terms = {
		"lavatube", "lava-replay", "vulkan", "gpu", "drm", "mali", "panthor", "nvidia", "amdgpu", "i915"
	};
	const size_t slash = trace_filename.find_last_of("/\\");
	std::string trace_name = slash == std::string::npos ? trace_filename : trace_filename.substr(slash + 1);
	const size_t extension = trace_name.find_last_of('.');
	if (extension != std::string::npos) trace_name.resize(extension);
	if (!trace_name.empty()) terms.push_back(trace_name);

	std::string pattern = "(";
	for (const std::string& term : terms)
	{
		if (pattern.size() > 1) pattern += "|";
		pattern += escape_pattern_literal(term);
	}
	pattern += ")";
	return pattern;
}

bool system_log_collector::spawn_follower(const std::vector<std::string>& sources, const std::string& pattern)
{
	int output_pipe[2] = { -1, -1 };
	int error_pipe[2] = { -1, -1 };
	if (pipe2(output_pipe, O_CLOEXEC) != 0 || pipe2(error_pipe, O_CLOEXEC) != 0)
	{
		if (output_pipe[0] >= 0) close(output_pipe[0]);
		if (output_pipe[1] >= 0) close(output_pipe[1]);
		if (error_pipe[0] >= 0) close(error_pipe[0]);
		if (error_pipe[1] >= 0) close(error_pipe[1]);
		mError = std::string("failed to create journalctl pipes: ") + strerror(errno);
		mStatus.store(collector_status::failed, std::memory_order_release);
		return false;
	}

	std::vector<std::string> arguments = { "journalctl" };
	for (const std::string& source : sources) arguments.push_back(source);
	arguments.push_back("--follow");
	arguments.push_back("--lines=0");
	arguments.push_back("--no-pager");
	arguments.push_back("--output=short-iso-precise");
	arguments.push_back("--truncate-newline");
	arguments.push_back("--case-sensitive=no");
	arguments.push_back("--grep=" + pattern);
	std::vector<char*> argument_pointers;
	for (std::string& argument : arguments) argument_pointers.push_back(argument.data());
	argument_pointers.push_back(nullptr);

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&actions, error_pipe[1], STDERR_FILENO);
	posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
	posix_spawn_file_actions_addclose(&actions, output_pipe[1]);
	posix_spawn_file_actions_addclose(&actions, error_pipe[0]);
	posix_spawn_file_actions_addclose(&actions, error_pipe[1]);
	pid_t pid = -1;
	const int spawn_result = posix_spawnp(&pid, "journalctl", &actions, nullptr, argument_pointers.data(), environ);
	posix_spawn_file_actions_destroy(&actions);
	close(output_pipe[1]);
	close(error_pipe[1]);
	if (spawn_result != 0)
	{
		close(output_pipe[0]);
		close(error_pipe[0]);
		mPid.store(-1, std::memory_order_release);
		mError = std::string("failed to start journalctl follower: ") + strerror(spawn_result);
		mStatus.store(collector_status::failed, std::memory_order_release);
		return false;
	}
	mPid.store(pid, std::memory_order_release);

	mStdoutFd = output_pipe[0];
	mStderrFd = error_pipe[0];
	mStatus.store(collector_status::starting, std::memory_order_release);
	mThread = std::thread(&system_log_collector::reader_main, this);

	for (unsigned i = 0; i < 50 && mStatus.load(std::memory_order_acquire) == collector_status::starting; i++)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return mStatus.load(std::memory_order_acquire) == collector_status::ready;
}

bool system_log_collector::start(const std::string& trace_filename)
{
	if (mStatus.load(std::memory_order_acquire) != collector_status::stopped) return false;
	mOutput = tmpfile();
	if (!mOutput)
	{
		mError = std::string("failed to create system-log storage: ") + strerror(errno);
		mStatus.store(collector_status::failed, std::memory_order_release);
		return false;
	}

	const probe_result user = probe("--user");
	const probe_result system = probe("--system");
	std::vector<std::string> sources;
	if (user.available) sources.push_back("--user");
	else mWarning = "user journal unavailable: " + user.error;
	if (system.available) sources.push_back("--system");
	else
	{
		if (!mWarning.empty()) mWarning += "; ";
		mWarning += "system journal unavailable: " + system.error;
	}
	if (sources.empty())
	{
		mError = mWarning + ". Grant journal access before starting replay, for example with 'newgrp adm'.";
		mWarning.clear();
		mStatus.store(collector_status::failed, std::memory_order_release);
		return false;
	}
	return spawn_follower(sources, filter_pattern(trace_filename));
}

bool system_log_collector::append_output_lines()
{
	size_t newline = mPendingOutput.find('\n');
	while (newline != std::string::npos)
	{
		flockfile(mOutput);
		const size_t written = fwrite(mPendingOutput.data(), 1, newline + 1, mOutput);
		const int flush_result = fflush(mOutput);
		funlockfile(mOutput);
		if (written != newline + 1 || flush_result != 0)
		{
			const int write_error = errno == 0 ? EIO : errno;
			mError = std::string("failed to store system log: ") + strerror(write_error);
			return false;
		}
		mPendingOutput.erase(0, newline + 1);
		newline = mPendingOutput.find('\n');
	}
	return true;
}

void system_log_collector::read_pipe(int& fd, bool output_pipe)
{
	char buffer[4096];
	const ssize_t bytes = read(fd, buffer, sizeof(buffer));
	if (bytes < 0)
	{
		if (errno == EINTR || errno == EAGAIN) return;
		close(fd);
		fd = -1;
		return;
	}
	if (bytes == 0)
	{
		close(fd);
		fd = -1;
		return;
	}
	if (output_pipe)
	{
		mPendingOutput.append(buffer, (size_t)bytes);
		if (!append_output_lines())
		{
			const pid_t pid = mPid.load(std::memory_order_acquire);
			if (pid >= 0) kill(pid, SIGTERM);
		}
	}
	else if (mChildError.size() < 16384)
	{
		const size_t retained = std::min((size_t)bytes, (size_t)16384 - mChildError.size());
		mChildError.append(buffer, retained);
	}
}

void system_log_collector::reader_main()
{
	const auto ready_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
	while (mStdoutFd >= 0 || mStderrFd >= 0)
	{
		struct pollfd descriptors[2] = {};
		descriptors[0].fd = mStdoutFd;
		descriptors[0].events = mStdoutFd >= 0 ? POLLIN : 0;
		descriptors[1].fd = mStderrFd;
		descriptors[1].events = mStderrFd >= 0 ? POLLIN : 0;
		const int result = poll(descriptors, 2, 50);
		if (result < 0)
		{
			if (errno == EINTR) continue;
			mError = std::string("failed to poll journalctl: ") + strerror(errno);
			const pid_t pid = mPid.load(std::memory_order_acquire);
			if (pid >= 0) kill(pid, SIGTERM);
			break;
		}
		if (mStdoutFd >= 0 && (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR))) read_pipe(mStdoutFd, true);
		if (mStderrFd >= 0 && (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR))) read_pipe(mStderrFd, false);
		if (mStatus.load(std::memory_order_acquire) == collector_status::starting && std::chrono::steady_clock::now() >= ready_time)
		{
			mStatus.store(collector_status::ready, std::memory_order_release);
		}
	}

	mPendingOutput.clear();
	int status = 0;
	pid_t wait_result = -1;
	const pid_t pid = mPid.load(std::memory_order_acquire);
	if (pid >= 0)
	{
		do
		{
			wait_result = waitpid(pid, &status, 0);
		} while (wait_result < 0 && errno == EINTR);
	}
	mPid.store(-1, std::memory_order_release);
	if (mStopRequested.load(std::memory_order_acquire))
	{
		mStatus.store(collector_status::stopped, std::memory_order_release);
		return;
	}
	if (mError.empty())
	{
		std::ostringstream message;
		if (wait_result < 0) message << "failed to wait for journalctl follower: " << strerror(errno);
		else if (WIFEXITED(status)) message << "journalctl follower exited with status " << WEXITSTATUS(status);
		else if (WIFSIGNALED(status)) message << "journalctl follower terminated by signal " << WTERMSIG(status);
		else message << "journalctl follower stopped unexpectedly";
		const std::string detail = single_line(mChildError);
		if (!detail.empty()) message << ": " << detail;
		mError = message.str();
	}
	mStatus.store(collector_status::failed, std::memory_order_release);
}

void system_log_collector::stop()
{
	if (mThread.joinable())
	{
		mStopRequested.store(true, std::memory_order_release);
		const pid_t pid = mPid.load(std::memory_order_acquire);
		if (pid >= 0) kill(pid, SIGTERM);
		mThread.join();
	}
	if (mStdoutFd >= 0) close(mStdoutFd);
	if (mStderrFd >= 0) close(mStderrFd);
	mStdoutFd = -1;
	mStderrFd = -1;
	mPid.store(-1, std::memory_order_release);
}
#else
system_log_collector::probe_result system_log_collector::probe(const char*)
{
	return {};
}

std::string system_log_collector::escape_pattern_literal(const std::string& value)
{
	return value;
}

std::string system_log_collector::filter_pattern(const std::string&)
{
	return "";
}

bool system_log_collector::spawn_follower(const std::vector<std::string>&, const std::string&)
{
	return false;
}

bool system_log_collector::start(const std::string&)
{
	mError = "system-log collection is not implemented on Android";
	mStatus.store(collector_status::failed, std::memory_order_release);
	return false;
}

void system_log_collector::reader_main() {}
void system_log_collector::read_pipe(int&, bool) {}
bool system_log_collector::append_output_lines() { return false; }
void system_log_collector::stop() {}
#endif

bool system_log_collector::available(std::string& warning, std::string& error) const
{
	const collector_status status = mStatus.load(std::memory_order_acquire);
	if (status == collector_status::ready)
	{
		warning = mWarning;
		return true;
	}
	if (status == collector_status::starting) error = "system-log collector is still starting";
	else if (status == collector_status::failed) error = mError;
	else error = "system-log collector is not running";
	return false;
}
