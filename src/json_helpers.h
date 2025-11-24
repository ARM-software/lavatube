#pragma once

#include "lavatube.h"
#include "jsoncpp/json/value.h"

// --- JSON write helpers ---

Json::Value trackable_json(const trackable* t);
Json::Value trackedaccelerationstructure_json(const trackedaccelerationstructure* t);
Json::Value trackedbuffer_json(const trackedbuffer* t);
Json::Value trackedimage_json(const trackedimage* t);
Json::Value trackedtensor_json(const trackedtensor* t);
Json::Value trackedswapchain_json(const trackedswapchain* t);
Json::Value trackedcmdbuffer_trace_json(const trackedcmdbuffer_trace* t);
Json::Value trackedimageview_json(const trackedimageview* t);
Json::Value trackedbufferview_json(const trackedbufferview* t);
Json::Value trackeddescriptorset_trace_json(const trackeddescriptorset_trace* t);
Json::Value trackedqueue_json(const trackedqueue* t);
Json::Value trackedevent_trace_json(const trackedevent_trace* t);
Json::Value trackedmemory_json(const trackedmemory* t);
Json::Value trackedfence_json(const trackedfence* t);
Json::Value trackedpipeline_json(const trackedpipeline* t);
Json::Value trackedcommandpool_trace_json(const trackedcommandpool_trace* t);
Json::Value trackeddescriptorpool_trace_json(const trackeddescriptorpool_trace* t);
Json::Value trackeddevice_json(const trackeddevice* t);
Json::Value trackedshadermodule_json(const trackedshadermodule* t);
Json::Value trackedphysicaldevice_json(const trackedphysicaldevice* t);
Json::Value trackedframebuffer_json(const trackedframebuffer* t);
Json::Value trackedrenderpass_json(const trackedrenderpass* t);
Json::Value trackedpipelinelayout_json(const trackedpipelinelayout* t);
Json::Value trackeddescriptorsetlayout_json(const trackeddescriptorsetlayout* t);

// --- JSON read helpers ---

trackable trackable_json(const Json::Value& v);
trackedfence trackedfence_json(const Json::Value& v);
trackedpipeline trackedpipeline_json(const Json::Value& v);
trackedaccelerationstructure trackedaccelerationstructure_json(const Json::Value& v);
trackedbuffer trackedbuffer_json(const Json::Value& v);
trackedimage trackedimage_json(const Json::Value& v);
trackedtensor trackedtensor_json(const Json::Value& v);
trackedswapchain_replay trackedswapchain_replay_json(const Json::Value& v);
trackedcmdbuffer trackedcmdbuffer_json(const Json::Value& v);
trackedimageview trackedimageview_json(const Json::Value& v);
trackedbufferview trackedbufferview_json(const Json::Value& v);
trackeddescriptorset trackeddescriptorset_json(const Json::Value& v);
trackedqueue trackedqueue_json(const Json::Value& v);
trackeddevice trackeddevice_json(const Json::Value& v);
trackedphysicaldevice trackedphysicaldevice_json(const Json::Value& v);
trackedframebuffer trackedframebuffer_json(const Json::Value& v);
trackedshadermodule trackedshadermodule_json(const Json::Value& v);
trackedrenderpass trackedrenderpass_json(const Json::Value& v);
trackedpipelinelayout trackedpipelinelayout_json(const Json::Value& v);
trackeddescriptorsetlayout trackeddescriptorsetlayout_json(const Json::Value& v);
