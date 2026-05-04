#pragma once

#include "write.h"

static inline lava_file_writer& prepare_trace_callback(callback_context& cb)
{
	lava_file_writer& writer = lava_writer::instance().file_writer();
	memset(&writer.use_result, 0, sizeof(writer.use_result));
	static_assert(sizeof(writer.use_result) >= sizeof(cb.result));
	memcpy(&writer.use_result, &cb.result, sizeof(cb.result));
	return writer;
}

template<auto TraceFn>
struct replay_trace_callback;

template<typename R, typename... Args, R(VKAPI_PTR *TraceFn)(Args...)>
struct replay_trace_callback<TraceFn>
{
	static void call(callback_context& cb, Args... args)
	{
		prepare_trace_callback(cb);
		(void)TraceFn(args...);
	}
};

template<auto TraceFn, auto PostFn>
struct replay_trace_callback_with_post;

template<typename R, typename... Args, R(VKAPI_PTR *TraceFn)(Args...), void(*PostFn)(lava_file_writer&, Args...)>
struct replay_trace_callback_with_post<TraceFn, PostFn>
{
	static void call(callback_context& cb, Args... args)
	{
		lava_file_writer& writer = prepare_trace_callback(cb);
		(void)TraceFn(args...);
		PostFn(writer, args...);
	}
};

template<auto TraceFn, auto PreFn>
struct replay_trace_callback_with_pre;

template<typename R, typename... Args, R(VKAPI_PTR *TraceFn)(Args...), void(*PreFn)(callback_context&, Args...)>
struct replay_trace_callback_with_pre<TraceFn, PreFn>
{
	static void call(callback_context& cb, Args... args)
	{
		PreFn(cb, args...);
		prepare_trace_callback(cb);
		(void)TraceFn(args...);
	}
};
