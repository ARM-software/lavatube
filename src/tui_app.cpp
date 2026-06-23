#include "tui_app.h"

#include <algorithm>

#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/dom/elements.hpp"

using namespace ftxui;

struct transcript_entry
{
	std::string speaker;
	std::string text;
};

static int clamp_int(int value, int min_value, int max_value)
{
	return std::max(min_value, std::min(max_value, value));
}

static std::vector<std::string> wrap_plain_line(const std::string& line, int width)
{
	std::vector<std::string> out;
	if (width <= 0) width = 1;
	if (line.empty())
	{
		out.push_back("");
		return out;
	}

	size_t pos = 0;
	while (pos < line.size())
	{
		const size_t remaining = line.size() - pos;
		if (remaining <= (size_t)width)
		{
			out.push_back(line.substr(pos));
			break;
		}

		size_t end = pos + width;
		size_t split = line.rfind(' ', end);
		if (split == std::string::npos || split <= pos)
		{
			split = end;
		}

		out.push_back(line.substr(pos, split - pos));
		pos = split;
		while (pos < line.size() && line[pos] == ' ')
		{
			pos++;
		}
	}

	return out;
}

static std::vector<std::string> wrap_text_lines(const std::string& value, int width)
{
	std::vector<std::string> out;
	size_t pos = 0;
	while (pos <= value.size())
	{
		const size_t next = value.find('\n', pos);
		const std::string line = next == std::string::npos ? value.substr(pos) : value.substr(pos, next - pos);
		const std::vector<std::string> wrapped = wrap_plain_line(line, width);
		out.insert(out.end(), wrapped.begin(), wrapped.end());
		if (next == std::string::npos) break;
		pos = next + 1;
	}
	return out;
}

static void append_wrapped_text(Elements& lines, const std::string& value, int width)
{
	const std::vector<std::string> wrapped = wrap_text_lines(value, width);
	for (const std::string& line : wrapped)
	{
		lines.push_back(text(line));
	}
}

class tui_component : public ComponentBase
{
public:
	tui_component(App* screen, const tui_options& options)
		: mScreen(screen)
		, mTools(tool_options(options))
		, mClient(options.api_key, options.model, options.base_url, options.reasoning_effort)
		, mModel(options.model)
		, mBaseUrl(options.base_url)
		, mReasoningEffort(options.reasoning_effort)
		, mSource(mTools.source_label())
	{
		mInputComponent = Input(&mInput, options.replay_service ? "Ask about the replay service" : "Ask about the trace");
		Add(mInputComponent);

		std::string error;
		if (!mTools.validate(error))
		{
			mStatus = error;
			mReady = false;
		}
		else if (!mClient.configured())
		{
			mStatus = "LAVATUI_OPENAI_API_KEY or OPENAI_API_KEY is not set";
			mReady = false;
		}
		else
		{
			mStatus = "Ready";
			mReady = true;
		}

		transcript_entry entry;
		entry.speaker = "status";
		entry.text = mStatus;
		mTranscript.push_back(entry);
	}

	~tui_component() override
	{
		if (mWorker.joinable()) mWorker.join();
	}

	Element OnRender() override
	{
		std::vector<transcript_entry> transcript;
		std::string status;
		bool busy = false;
		int scroll_offset = 0;
		{
			std::lock_guard<std::mutex> lock(mMutex);
			transcript = mTranscript;
			status = mStatus;
			busy = mBusy;
			scroll_offset = mScrollOffset;
		}

		const int view_width = std::max(1, mScreen->dimx() - 1);
		const int view_height = std::max(1, mScreen->dimy() - 4);
		Elements lines;
		for (const transcript_entry& entry : transcript)
		{
			lines.push_back(text(entry.speaker + ":") | bold);
			append_wrapped_text(lines, entry.text, view_width);
			lines.push_back(text(""));
		}
		if (busy)
		{
			lines.push_back(text("assistant: working") | dim);
		}

		{
			std::lock_guard<std::mutex> lock(mMutex);
			mViewHeight = view_height;
			mMaxScrollOffset = std::max(0, (int)lines.size() - view_height);
			if (mFollowOutput)
			{
				mScrollOffset = mMaxScrollOffset;
			}
			else
			{
				mScrollOffset = clamp_int(mScrollOffset, 0, mMaxScrollOffset);
			}
			scroll_offset = mScrollOffset;
		}

		Element view = vbox(lines) | focusPosition(0, scroll_offset + view_height / 2) | vscroll_indicator | yframe | flex;
		Element input = hbox({ text("> "), mInputComponent->Render() | flex }) | border;
		Element status_line = text(status_bar(status, busy)) | inverted;
		return vbox({ view, input, status_line });
	}

	bool OnEvent(Event event) override
	{
		if (event == Event::CtrlD || event == Event::Escape)
		{
			mScreen->Exit();
			return true;
		}
		if (event == Event::Custom)
		{
			if (mWorker.joinable()) mWorker.join();
			return true;
		}
		if (event == Event::Return)
		{
			submit_input();
			return true;
		}
		if (event == Event::PageUp)
		{
			scroll_relative(-page_scroll_amount());
			return true;
		}
		if (event == Event::PageDown)
		{
			scroll_relative(page_scroll_amount());
			return true;
		}
		if (event == Event::ArrowDownCtrl)
		{
			scroll_to_bottom();
			return true;
		}
		if (event.is_mouse())
		{
			if (event.mouse().button == Mouse::WheelUp)
			{
				scroll_relative(-3);
				return true;
			}
			if (event.mouse().button == Mouse::WheelDown)
			{
				scroll_relative(3);
				return true;
			}
		}
		return mInputComponent->OnEvent(event);
	}

	Component ActiveChild() override
	{
		return mInputComponent;
	}

	bool Focusable() const override
	{
		return true;
	}

private:
	static tui_trace_tools_options tool_options(const tui_options& options)
	{
		tui_trace_tools_options out;
		out.trace_file = options.trace_file;
		out.hostname = options.hostname;
		out.port = options.port;
		out.replay_service = options.replay_service;
		return out;
	}

	static void worker_main(tui_component* self, std::string prompt)
	{
		self->run_prompt(prompt);
	}

	void submit_input()
	{
		const std::string prompt = mInput;
		if (prompt.empty()) return;

		{
			std::lock_guard<std::mutex> lock(mMutex);
			if (mBusy) return;
			scroll_to_bottom_locked();
			add_transcript_locked("user", prompt);
			if (!mReady)
			{
				add_transcript_locked("status", mStatus);
				mInput.clear();
				return;
			}
			tui_chat_message message;
			message.role = "user";
			message.content = prompt;
			mHistory.push_back(message);
			mBusy = true;
			mStatus = "Waiting for model";
		}

		mInput.clear();
		if (mWorker.joinable()) mWorker.join();
		mWorker = std::thread(&tui_component::worker_main, this, prompt);
	}

	void run_prompt(const std::string& prompt)
	{
		(void)prompt;
		std::vector<tui_chat_message> history;
		{
			std::lock_guard<std::mutex> lock(mMutex);
			history = mHistory;
		}

		tui_assistant_result result = mClient.ask(history, mTools);

		{
			std::lock_guard<std::mutex> lock(mMutex);
			for (const tui_tool_notice& notice : result.tools)
			{
				add_transcript_locked("tool", notice.name + "(" + notice.arguments + ")");
			}
			if (result.ok)
			{
				add_transcript_locked("assistant", result.text);
				tui_chat_message message;
				message.role = "assistant";
				message.content = result.text;
				mHistory.push_back(message);
				mStatus = result.usage.empty() ? "Ready" : "Ready " + result.usage;
			}
			else
			{
				add_transcript_locked("error", result.error);
				mStatus = result.error;
			}
			trim_history_locked();
			mBusy = false;
		}

		mScreen->Post(Event::Custom);
	}

	void add_transcript_locked(const std::string& speaker, const std::string& text)
	{
		transcript_entry entry;
		entry.speaker = speaker;
		entry.text = text;
		mTranscript.push_back(entry);
		if (mTranscript.size() > 80)
		{
			mTranscript.erase(mTranscript.begin(), mTranscript.begin() + (mTranscript.size() - 80));
		}
		if (mFollowOutput)
		{
			mScrollOffset = mMaxScrollOffset;
		}
	}

	int page_scroll_amount()
	{
		std::lock_guard<std::mutex> lock(mMutex);
		return std::max(1, mViewHeight - 2);
	}

	void scroll_relative(int delta)
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mScrollOffset = clamp_int(mScrollOffset + delta, 0, mMaxScrollOffset);
		mFollowOutput = mScrollOffset >= mMaxScrollOffset;
	}

	void scroll_to_bottom()
	{
		std::lock_guard<std::mutex> lock(mMutex);
		scroll_to_bottom_locked();
	}

	void scroll_to_bottom_locked()
	{
		mScrollOffset = mMaxScrollOffset;
		mFollowOutput = true;
	}

	void trim_history_locked()
	{
		if (mHistory.size() <= 12) return;
		mHistory.erase(mHistory.begin(), mHistory.begin() + (mHistory.size() - 12));
	}

	std::string status_bar(const std::string& status, bool busy) const
	{
		std::string out = mSource + " model=" + mModel;
		if (!mReasoningEffort.empty()) out += " reasoning=" + mReasoningEffort;
		if (!mBaseUrl.empty()) out += " base_url=" + mBaseUrl;
		out += busy ? " busy " : " ";
		out += status;
		out += "  PageUp/PageDown scroll Ctrl-Down bottom Ctrl-D/Esc quit";
		return out;
	}

	App* mScreen = nullptr;
	Component mInputComponent;
	std::string mInput;
	tui_trace_tools mTools;
	tui_openai_client mClient;
	std::string mModel;
	std::string mBaseUrl;
	std::string mReasoningEffort;
	std::string mSource;
	bool mReady = false;
	bool mBusy = false;
	bool mFollowOutput = true;
	int mScrollOffset = 0;
	int mMaxScrollOffset = 0;
	int mViewHeight = 1;
	std::string mStatus;
	std::vector<transcript_entry> mTranscript;
	std::vector<tui_chat_message> mHistory;
	std::mutex mMutex;
	std::thread mWorker;
};

int run_tui(const tui_options& options)
{
	App screen = App::Fullscreen();
	screen.TrackMouse(true);
	Component component = Make<tui_component>(&screen, options);
	screen.Loop(component);
	return 0;
}
