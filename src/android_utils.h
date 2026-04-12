#pragma once

#ifdef VK_USE_PLATFORM_ANDROID_KHR
#include <android/native_window.h>
#include <string>

struct AndroidState
{
	ANativeWindow* pendingWindow = nullptr;
};

namespace AndroidGlobs
{
	extern AndroidState* G_STATE;
	extern std::string ANDROID_OUT_TRACE_PATH;
}
#endif
