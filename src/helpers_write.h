#pragma once

#include "write.h"

trackable* debug_object_trackable(trace_records& r, VkDebugReportObjectTypeEXT type, uint64_t object);
trackable* object_trackable(const trace_records& r, VkObjectType type, uint64_t object);
