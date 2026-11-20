#pragma once

// Profiler zones for Tracy (phase E1). Thread-safe, usable in jobs and
// on any thread; compiled out when the build has no Tracy (SR_TRACY off). Names must be
// string literals.
//
//   SR_ZONE("region: decode");          // a zone for the rest of the scope
//   SR_FRAME_MARK();                    // the end of a frame (once a frame, main thread)
//   SR_PLOT("streaming: MB", value);    // a value plotted over time
//
// SR_PROFILE (Profiler.h) also opens a zone, and adds the main thread's timing to the
// developer tools' Performance panel.

#include <cstddef>

#ifdef SR_TRACY
#include <tracy/Tracy.hpp>
#define SR_TRACE_CONCAT2(a, b) a##b
#define SR_TRACE_CONCAT(a, b) SR_TRACE_CONCAT2(a, b)
#define SR_ZONE(name) ZoneNamedN(SR_TRACE_CONCAT(srZone_, __LINE__), name, true)
#define SR_FRAME_MARK() FrameMark
#define SR_FRAME_MARK_NAMED(name) FrameMarkNamed(name)
#define SR_PLOT(name, value) TracyPlot(name, value)
#else
#define SR_ZONE(name) ((void)0)
#define SR_FRAME_MARK() ((void)0)
#define SR_FRAME_MARK_NAMED(name) ((void)0)
#define SR_PLOT(name, value) ((void)0)
#endif

namespace sr::core {

// Names the calling thread for the debugger and the profiler.
void SetThreadName(const char* name);

// Runs the calling thread above normal priority (the game and render threads, THREADING.md 4.1).
void RaiseThreadPriority();

// A Tracy profiler is connected (always false without Tracy).
bool ProfilerConnected();

// Sends a message to the profiler's timeline (no-op without Tracy).
void TraceMessage(const char* text, std::size_t size);

} // namespace sr::core
