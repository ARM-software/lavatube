#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "android_replay_args.h"
#include "android_utils.h"
#include "replay_entry.h"
#include "util.h"

struct android_replay_runtime
{
	AndroidState window_state;
	android_app* app = nullptr;
	std::vector<std::string> arguments;
	std::string result_path;
	std::atomic_bool replay_finished { false };
	std::atomic_bool stop_requested { false };
	int exit_status = 1;
};

static bool write_replay_result(const std::string& path, const char* status, int exit_code)
{
	const std::string temporary_path = path + ".tmp";
	FILE* file = fopen(temporary_path.c_str(), "w");
	if (file == nullptr)
	{
		ELOG("Failed to create Android replay result %s: %s", temporary_path.c_str(), strerror(errno));
		return false;
	}
	fprintf(file, "{\"status\":\"%s\",\"exit_code\":%d}\n", status, exit_code);
	if (fclose(file) != 0)
	{
		ELOG("Failed to close Android replay result %s: %s", temporary_path.c_str(), strerror(errno));
		return false;
	}
	if (rename(temporary_path.c_str(), path.c_str()) != 0)
	{
		ELOG("Failed to publish Android replay result %s: %s", path.c_str(), strerror(errno));
		return false;
	}
	return true;
}

static bool get_intent_extra(android_app* app, JNIEnv* environment, const char* name, std::string& value)
{
	jclass activity_class = environment->GetObjectClass(app->activity->clazz);
	if (activity_class == nullptr) return false;
	jmethodID get_intent = environment->GetMethodID(activity_class, "getIntent", "()Landroid/content/Intent;");
	if (get_intent == nullptr)
	{
		environment->DeleteLocalRef(activity_class);
		return false;
	}
	jobject intent = environment->CallObjectMethod(app->activity->clazz, get_intent);
	if (intent == nullptr)
	{
		environment->DeleteLocalRef(activity_class);
		return false;
	}
	jclass intent_class = environment->GetObjectClass(intent);
	jmethodID get_string_extra = environment->GetMethodID(intent_class, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;");
	jstring key = environment->NewStringUTF(name);
	jstring result = nullptr;
	if (get_string_extra != nullptr && key != nullptr)
	{
		result = static_cast<jstring>(environment->CallObjectMethod(intent, get_string_extra, key));
	}
	if (result != nullptr)
	{
		const char* text = environment->GetStringUTFChars(result, nullptr);
		if (text != nullptr)
		{
			value = text;
			environment->ReleaseStringUTFChars(result, text);
		}
	}
	if (result != nullptr) environment->DeleteLocalRef(result);
	if (key != nullptr) environment->DeleteLocalRef(key);
	environment->DeleteLocalRef(intent_class);
	environment->DeleteLocalRef(intent);
	environment->DeleteLocalRef(activity_class);
	return result != nullptr;
}

static bool get_external_files_directory(android_app* app, JNIEnv* environment, std::string& path)
{
	jclass activity_class = environment->GetObjectClass(app->activity->clazz);
	if (activity_class == nullptr) return false;
	jmethodID get_external_files_dir = environment->GetMethodID(activity_class, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;");
	if (get_external_files_dir == nullptr)
	{
		environment->DeleteLocalRef(activity_class);
		return false;
	}
	jobject file = environment->CallObjectMethod(app->activity->clazz, get_external_files_dir, nullptr);
	if (file == nullptr)
	{
		environment->DeleteLocalRef(activity_class);
		return false;
	}
	jclass file_class = environment->GetObjectClass(file);
	jmethodID get_absolute_path = environment->GetMethodID(file_class, "getAbsolutePath", "()Ljava/lang/String;");
	jstring java_path = nullptr;
	if (get_absolute_path != nullptr)
	{
		java_path = static_cast<jstring>(environment->CallObjectMethod(file, get_absolute_path));
	}
	if (java_path != nullptr)
	{
		const char* text = environment->GetStringUTFChars(java_path, nullptr);
		if (text != nullptr)
		{
			path = text;
			environment->ReleaseStringUTFChars(java_path, text);
		}
	}
	if (java_path != nullptr) environment->DeleteLocalRef(java_path);
	environment->DeleteLocalRef(file_class);
	environment->DeleteLocalRef(file);
	environment->DeleteLocalRef(activity_class);
	return !path.empty();
}

static void request_android_replay_stop(android_replay_runtime* runtime)
{
	if (runtime->stop_requested.exchange(true, std::memory_order_acq_rel)) return;
	ILOG("Android lifecycle requested replay stop");
	lava_replay_request_stop();
}

static void handle_android_command(android_app* app, int32_t command)
{
	android_replay_runtime* runtime = static_cast<android_replay_runtime*>(app->userData);
	if (runtime == nullptr) return;
	switch (command)
	{
		case APP_CMD_INIT_WINDOW:
			if (runtime->window_state.pendingWindow == nullptr && app->window != nullptr)
			{
				ANativeWindow_acquire(app->window);
				runtime->window_state.pendingWindow = app->window;
				ILOG("Android replay window initialized");
			}
			break;
		case APP_CMD_TERM_WINDOW:
		case APP_CMD_DESTROY:
			if (!runtime->replay_finished.load(std::memory_order_acquire)) request_android_replay_stop(runtime);
			break;
		default:
			break;
	}
}

static void process_android_event(android_replay_runtime* runtime, int timeout)
{
	int events = 0;
	android_poll_source* source = nullptr;
	const int result = ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source));
	if (result >= 0 && source != nullptr) source->process(runtime->app, source);
	if (runtime->app->destroyRequested != 0 && !runtime->replay_finished.load(std::memory_order_acquire)) request_android_replay_stop(runtime);
}

static bool wait_for_android_window(android_replay_runtime* runtime)
{
	while (runtime->window_state.pendingWindow == nullptr && runtime->app->destroyRequested == 0)
	{
		process_android_event(runtime, -1);
	}
	return runtime->window_state.pendingWindow != nullptr && !runtime->stop_requested.load(std::memory_order_acquire);
}

static void android_replay_worker(android_replay_runtime* runtime)
{
	std::vector<char*> argument_pointers;
	argument_pointers.reserve(runtime->arguments.size());
	for (std::string& argument : runtime->arguments) argument_pointers.push_back(argument.data());
	runtime->exit_status = lava_replay_main(static_cast<int>(argument_pointers.size()), argument_pointers.data());
	const bool stopped = runtime->stop_requested.load(std::memory_order_acquire);
	const char* status = stopped ? "stopped" : (runtime->exit_status == 0 ? "done" : "failed");
	write_replay_result(runtime->result_path, status, runtime->exit_status);
	runtime->replay_finished.store(true, std::memory_order_release);
	if (runtime->app->looper != nullptr) ALooper_wake(runtime->app->looper);
}

void android_main(android_app* app)
{
	ILOG("Android lava-replay starting");
	ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);

	JNIEnv* environment = nullptr;
	if (app->activity->vm->AttachCurrentThread(&environment, nullptr) != JNI_OK || environment == nullptr)
	{
		ELOG("Failed to attach Android replay thread to Java VM");
		ANativeActivity_finish(app->activity);
		return;
	}

	std::string external_directory;
	if (!get_external_files_directory(app, environment, external_directory))
	{
		ELOG("Failed to resolve Android app-specific external files directory");
		app->activity->vm->DetachCurrentThread();
		ANativeActivity_finish(app->activity);
		return;
	}
	const std::string result_path = external_directory + "/lava-replay-result.json";

	std::string mode;
	if (get_intent_extra(app, environment, "mode", mode) && mode == "prepare")
	{
		write_replay_result(result_path, "done", 0);
		ILOG("Android replay storage prepared at %s", external_directory.c_str());
		app->activity->vm->DetachCurrentThread();
		ANativeActivity_finish(app->activity);
		return;
	}

	std::string command_line;
	std::string parse_error;
	std::vector<std::string> parsed_arguments;
	if (!get_intent_extra(app, environment, "args", command_line) ||
		!parse_android_replay_arguments(command_line, parsed_arguments, parse_error) || parsed_arguments.empty())
	{
		ELOG("Invalid Android replay arguments: %s", parse_error.empty() ? "missing args intent extra" : parse_error.c_str());
		write_replay_result(result_path, "failed", 2);
		app->activity->vm->DetachCurrentThread();
		ANativeActivity_finish(app->activity);
		return;
	}
	if (chdir(external_directory.c_str()) != 0)
	{
		ELOG("Failed to enter Android replay directory %s: %s", external_directory.c_str(), strerror(errno));
		write_replay_result(result_path, "failed", 2);
		app->activity->vm->DetachCurrentThread();
		ANativeActivity_finish(app->activity);
		return;
	}
	if (setenv("TMPDIR", external_directory.c_str(), 1) != 0)
	{
		ELOG("Failed to set Android replay temporary directory: %s", strerror(errno));
		write_replay_result(result_path, "failed", 2);
		app->activity->vm->DetachCurrentThread();
		ANativeActivity_finish(app->activity);
		return;
	}
	app->activity->vm->DetachCurrentThread();

	android_replay_runtime runtime;
	runtime.app = app;
	runtime.result_path = result_path;
	runtime.arguments.push_back("lava-replay");
	runtime.arguments.insert(runtime.arguments.end(), parsed_arguments.begin(), parsed_arguments.end());
	app->userData = &runtime;
	app->onAppCmd = handle_android_command;
	AndroidGlobs::G_STATE = &runtime.window_state;
	write_replay_result(result_path, "running", 0);

	if (!wait_for_android_window(&runtime))
	{
		write_replay_result(result_path, "stopped", 0);
	}
	else
	{
		std::thread replay_thread(android_replay_worker, &runtime);
		while (!runtime.replay_finished.load(std::memory_order_acquire)) process_android_event(&runtime, -1);
		replay_thread.join();
	}

	AndroidGlobs::G_STATE = nullptr;
	if (runtime.window_state.pendingWindow != nullptr)
	{
		ANativeWindow_release(runtime.window_state.pendingWindow);
		runtime.window_state.pendingWindow = nullptr;
	}
	app->userData = nullptr;
	if (app->destroyRequested == 0)
	{
		app->userData = &runtime;
		ANativeActivity_finish(app->activity);
		while (app->destroyRequested == 0) process_android_event(&runtime, -1);
		app->userData = nullptr;
	}
	ILOG("Android lava-replay finished with status %d", runtime.exit_status);
}
