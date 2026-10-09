// dshow_video.h - DirectShow movies played into a renderer the game wrote.
//
// dshow.cpp owns the filter graph object; this module adds what a graph
// needs when the game builds the graph itself rather than calling
// RenderFile: AddFilter for the game's renderer, AddSourceFilter for an
// MPEG-1 video file, and Connect between them, standing in for the file
// source, the MPEG-1 splitter and the MPEG Video Decoder a Windows graph
// would have inserted. The graph's IMediaControl, IMediaSeeking and
// IMediaPosition hand a graph that has a movie over to the functions below.
#pragma once
#include "../runtime/guest.h"

#include <string>

namespace dsv {

// True once AddSourceFilter has opened a movie on the graph.
bool has_movie(uint32_t graph_id);
// True when `host_path` is a file this module plays (an MPEG-1 elementary
// stream).
bool is_movie_file(const std::string &host_path);

// IGraphBuilder::AddFilter(pFilter, pName) for a filter the game implements:
// joins it to the graph (JoinFilterGraph) and keeps a reference. `graph_view`
// is the graph's own interface pointer, which the filter keeps.
uint32_t add_filter(X86 *c, uint32_t graph_id, uint32_t graph_view, uint32_t filter, uint32_t name);
// IGraphBuilder::AddSourceFilter for a movie file; writes the source filter
// to `out` and returns the HRESULT.
uint32_t add_source(X86 *c, uint32_t graph_id, const std::string &guest_path,
                    const std::string &host_path, uint32_t out);
// IGraphBuilder::Connect(out, in) from the movie source's output pin to the
// game's renderer input pin. Returns 1 (S_FALSE) when `out` is not ours, so
// the caller can fall back to its own answer.
uint32_t connect(X86 *c, uint32_t graph_id, uint32_t out, uint32_t in);

// IMediaControl.
uint32_t run(X86 *c, uint32_t graph_id);
uint32_t pause(X86 *c, uint32_t graph_id);
uint32_t stop(X86 *c, uint32_t graph_id);
uint32_t state(uint32_t graph_id); // State_Stopped 0, State_Paused 1, State_Running 2

// Media time in 100 ns units.
uint64_t duration(uint32_t graph_id);
uint64_t position(uint32_t graph_id);
void seek(uint32_t graph_id, uint64_t t);

// Delivers the pictures that are due to every running graph's renderer.
void pump(X86 *c);
// The graph is gone.
void forget(uint32_t graph_id);
void reset();
void register_interfaces();

} // namespace dsv

// dshow.cpp: queues an event on the graph's IMediaEventEx.
void dshow_post_graph_event(uint32_t graph_id, uint32_t code);
