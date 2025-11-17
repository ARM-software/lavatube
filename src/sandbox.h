#pragma once

/// Basic sandboxing that you always want. Call this immediately upon program start.
const char* sandbox_level_one();

/// Run at the program start after evaluating command-line arguments. It will limit certain policies that we never need.
/// Returns null on success, or an error string on failure. Initialize the window system integration before calling this.
const char* sandbox_level_two();

/// You need to open all files prior to starting this, eg if you want to dump results into a JSON, open the output file first.
/// Returns null on success, or an error string on failure. Note that this applies to an individual thread and its children,
/// so use before spawning or apply to it to each child and the parent thread, and make sure child threads cannot race the
/// parent setting its policy.
const char* sandbox_level_three();
