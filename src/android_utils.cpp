#include "android_utils.h"

#ifdef VK_USE_PLATFORM_ANDROID_KHR
namespace AndroidGlobs
{
	AndroidState* G_STATE = nullptr;
	std::string ANDROID_OUT_TRACE_PATH = "/sdcard/Android/data/com.arm.lavatube/files/";
}
#endif
