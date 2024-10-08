/* SPDX-FileCopyrightText: 2004 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup sequencer
 */

struct Depsgraph;
struct GPUOffScreen;
struct GPUViewport;
struct ImBuf;
struct ListBase;
struct Main;
struct Scene;
struct Sequence;
struct StripElem;
struct rctf;

enum eSeqTaskId {
  SEQ_TASK_MAIN_RENDER,
  SEQ_TASK_PREFETCH_RENDER,
};

struct SeqRenderData {
  Main *bmain;
  Depsgraph *depsgraph;
  Scene *scene;
  int rectx;
  int recty;
  int preview_render_size;
  bool use_proxies;
  bool ignore_missing_media;
  int for_render;
  int motion_blur_samples;
  float motion_blur_shutter;
  bool skip_cache;
  bool is_proxy_render;
  bool is_prefetch_render;
  bool is_playing;
  bool is_scrubbing;
  int view_id;
  /* ID of task for assigning temp cache entries to particular task(thread, etc.) */
  eSeqTaskId task_id;

  /* special case for OpenGL render */
  GPUOffScreen *gpu_offscreen;
  GPUViewport *gpu_viewport;
  // int gpu_samples;
  // bool gpu_full_samples;
};

/**
 * \return The image buffer or NULL.
 *
 * \note The returned #ImBuf has its reference increased, free after usage!
 */
ImBuf *SEQ_render_give_ibuf(const SeqRenderData *context, float timeline_frame, int chanshown);
ImBuf *SEQ_render_give_ibuf_direct(const SeqRenderData *context,
                                   float timeline_frame,
                                   Sequence *seq);
void SEQ_render_new_render_data(Main *bmain,
                                Depsgraph *depsgraph,
                                Scene *scene,
                                int rectx,
                                int recty,
                                int preview_render_size,
                                int for_render,
                                SeqRenderData *r_context);
StripElem *SEQ_render_give_stripelem(const Scene *scene, const Sequence *seq, int timeline_frame);

void SEQ_render_imbuf_from_sequencer_space(Scene *scene, ImBuf *ibuf);
void SEQ_render_pixel_from_sequencer_space_v4(Scene *scene, float pixel[4]);
/**
 * Check if `seq` is muted for rendering.
 * This function also checks `SeqTimelineChannel` flag.
 */
bool SEQ_render_is_muted(const ListBase *channels, const Sequence *seq);
