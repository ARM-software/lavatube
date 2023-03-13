#pragma once

#include "util.h"
#include "jsoncpp/json/value.h"

void allocators_set(VkAllocationCallbacks*& callbacks);
Json::Value allocators_json();
void allocators_print(FILE* fp);
