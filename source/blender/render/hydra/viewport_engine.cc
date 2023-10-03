/* SPDX-FileCopyrightText: 2011-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "viewport_engine.h"

#include <pxr/base/gf/camera.h>
#include <pxr/imaging/glf/drawTarget.h>
#include <pxr/usd/usdGeom/camera.h>

#include "DNA_camera_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_vec_types.h" /* This include must be before `BKE_camera.h` due to `rctf` type. */
#include "DNA_view3d_types.h"

#include "BLI_math_matrix.h"
#include "BLI_timecode.h"
#include "PIL_time.h"

#include "BKE_camera.h"
#include "BKE_context.h"

#include "DEG_depsgraph_query.hh"

#include "GPU_context.h"
#include "GPU_matrix.h"

#include "RE_engine.h"

#include "hydra/camera.h"

namespace blender::render::hydra {

struct ViewSettings {
  ViewSettings(bContext *context);

  int width();
  int height();

  pxr::GfCamera gf_camera();

  io::hydra::CameraData camera_data;

  int screen_width;
  int screen_height;
  pxr::GfVec4i border;
};

ViewSettings::ViewSettings(bContext *context)
    : camera_data(CTX_wm_view3d(context), CTX_wm_region(context))
{
  View3D *view3d = CTX_wm_view3d(context);
  RegionView3D *region_data = static_cast<RegionView3D *>(CTX_wm_region_data(context));
  ARegion *region = CTX_wm_region(context);

  screen_width = region->winx;
  screen_height = region->winy;

  Scene *scene = CTX_data_scene(context);

  /* Getting render border. */
  int x1 = 0, y1 = 0;
  int x2 = screen_width, y2 = screen_height;

  if (region_data->persp == RV3D_CAMOB) {
    if (scene->r.mode & R_BORDER) {
      Object *camera_obj = scene->camera;

      float camera_points[4][3];
      BKE_camera_view_frame(scene, static_cast<Camera *>(camera_obj->data), camera_points);

      float screen_points[4][2];
      for (int i = 0; i < 4; i++) {
        float world_location[] = {
            camera_points[i][0], camera_points[i][1], camera_points[i][2], 1.0f};
        mul_m4_v4(camera_obj->object_to_world, world_location);
        mul_m4_v4(region_data->persmat, world_location);

        if (world_location[3] > 0.0) {
          screen_points[i][0] = screen_width * 0.5f +
                                screen_width * 0.5f * (world_location[0] / world_location[3]);
          screen_points[i][1] = screen_height * 0.5f +
                                screen_height * 0.5f * (world_location[1] / world_location[3]);
        }
      }

      /* Getting camera view region. */
      float x1_f = std::min(
          {screen_points[0][0], screen_points[1][0], screen_points[2][0], screen_points[3][0]});
      float x2_f = std::max(
          {screen_points[0][0], screen_points[1][0], screen_points[2][0], screen_points[3][0]});
      float y1_f = std::min(
          {screen_points[0][1], screen_points[1][1], screen_points[2][1], screen_points[3][1]});
      float y2_f = std::max(
          {screen_points[0][1], screen_points[1][1], screen_points[2][1], screen_points[3][1]});

      /* Adjusting region to border. */
      float x = x1_f, y = y1_f;
      float dx = x2_f - x1_f, dy = y2_f - y1_f;

      x1 = x + scene->r.border.xmin * dx;
      x2 = x + scene->r.border.xmax * dx;
      y1 = y + scene->r.border.ymin * dy;
      y2 = y + scene->r.border.ymax * dy;

      /* Adjusting to region screen resolution. */
      x1 = std::max(std::min(x1, screen_width), 0);
      x2 = std::max(std::min(x2, screen_width), 0);
      y1 = std::max(std::min(y1, screen_height), 0);
      y2 = std::max(std::min(y2, screen_height), 0);
    }
  }
  else {
    if (view3d->flag2 & V3D_RENDER_BORDER) {
      int x = x1, y = y1;
      int dx = x2 - x1, dy = y2 - y1;

      x1 = int(x + view3d->render_border.xmin * dx);
      x2 = int(x + view3d->render_border.xmax * dx);
      y1 = int(y + view3d->render_border.ymin * dy);
      y2 = int(y + view3d->render_border.ymax * dy);
    }
  }

  border = pxr::GfVec4i(x1, y1, x2 - x1, y2 - y1);
}

int ViewSettings::width()
{
  return border[2];
}

int ViewSettings::height()
{
  return border[3];
}

pxr::GfCamera ViewSettings::gf_camera()
{
  return camera_data.gf_camera(pxr::GfVec4f(float(border[0]) / screen_width,
                                            float(border[1]) / screen_height,
                                            float(border[2]) / screen_width,
                                            float(border[3]) / screen_height));
}

DrawTexture::DrawTexture()
{
  float coords[8] = {0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0};

  GPUVertFormat format = {0};
  GPU_vertformat_attr_add(&format, "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
  GPU_vertformat_attr_add(&format, "texCoord", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
  GPUVertBuf *vbo = GPU_vertbuf_create_with_format(&format);
  GPU_vertbuf_data_alloc(vbo, 4);
  GPU_vertbuf_attr_fill(vbo, 0, coords);
  GPU_vertbuf_attr_fill(vbo, 1, coords);

  batch_ = GPU_batch_create_ex(GPU_PRIM_TRI_FAN, vbo, nullptr, GPU_BATCH_OWNS_VBO);
}

DrawTexture::~DrawTexture()
{
  if (texture_) {
    GPU_texture_free(texture_);
  }
  GPU_batch_discard(batch_);
}

void DrawTexture::write_data(int width, int height, const void *data)
{
  if (texture_ && width == GPU_texture_width(texture_) && height == GPU_texture_height(texture_)) {
    if (data) {
      GPU_texture_update(texture_, GPU_DATA_FLOAT, data);
    }
    return;
  }

  if (texture_) {
    GPU_texture_free(texture_);
  }

  texture_ = GPU_texture_create_2d("tex_hydra_render_viewport",
                                   width,
                                   height,
                                   1,
                                   GPU_RGBA32F,
                                   GPU_TEXTURE_USAGE_GENERAL,
                                   (float *)data);
}

void DrawTexture::draw(GPUShader *shader, const pxr::GfVec4d &viewport, GPUTexture *tex)
{
  if (!tex) {
    tex = texture_;
  }
  int slot = GPU_shader_get_sampler_binding(shader, "image");
  GPU_texture_bind(tex, slot);
  GPU_shader_uniform_1i(shader, "image", slot);

  GPU_matrix_push();
  GPU_matrix_translate_2f(viewport[0], viewport[1]);
  GPU_matrix_scale_2f(viewport[2] - viewport[0], viewport[3] - viewport[1]);
  GPU_batch_set_shader(batch_, shader);
  GPU_batch_draw(batch_);
  GPU_matrix_pop();
}

GPUTexture *DrawTexture::texture() const
{
  return texture_;
}

void ViewportEngine::render()
{
  ViewSettings view_settings(context_);
  if (view_settings.width() * view_settings.height() == 0) {
    return;
  };

  pxr::GfCamera gf_camera = view_settings.gf_camera();
  free_camera_delegate_->SetCamera(gf_camera);

  pxr::GfVec4d viewport(view_settings.border[0],
                        view_settings.border[1],
                        view_settings.border[2],
                        view_settings.border[3]);
  render_task_delegate_->set_viewport(viewport);
  if (light_tasks_delegate_) {
    light_tasks_delegate_->set_viewport(viewport);
  }

  render_task_delegate_->add_aov(pxr::HdAovTokens->color);
  render_task_delegate_->add_aov(pxr::HdAovTokens->depth);

  GPUFrameBuffer *view_framebuffer = GPU_framebuffer_active_get();
  render_task_delegate_->bind();

  auto t = tasks();
  engine_->Execute(render_index_.get(), &t);

  render_task_delegate_->unbind();

  GPU_framebuffer_bind(view_framebuffer);
  GPUShader *shader = GPU_shader_get_builtin_shader(GPU_SHADER_3D_IMAGE);
  GPU_shader_bind(shader);

  GPURenderTaskDelegate *gpu_task = dynamic_cast<GPURenderTaskDelegate *>(
      render_task_delegate_.get());
  if (gpu_task) {
    draw_texture_.draw(shader, viewport, gpu_task->aov_texture(pxr::HdAovTokens->color));
  }
  else {
    draw_texture_.write_data(view_settings.width(), view_settings.height(), nullptr);
    render_task_delegate_->read_aov(pxr::HdAovTokens->color, draw_texture_.texture());
    draw_texture_.draw(shader, viewport);
  }

  GPU_shader_unbind();

  if (renderer_percent_done() == 0.0f) {
    time_begin_ = PIL_check_seconds_timer();
  }

  char elapsed_time[32];

  BLI_timecode_string_from_time_simple(
      elapsed_time, sizeof(elapsed_time), PIL_check_seconds_timer() - time_begin_);

  float percent_done = renderer_percent_done();
  if (!render_task_delegate_->is_converged()) {
    notify_status(percent_done / 100.0,
                  std ::string("Time: ") + elapsed_time +
                      " | Done: " + std::to_string(int(percent_done)) + "%",
                  "Render");
    bl_engine_->flag |= RE_ENGINE_DO_DRAW;
  }
  else {
    notify_status(percent_done / 100.0, std::string("Time: ") + elapsed_time, "Rendering Done");
  }
}

void ViewportEngine::render(bContext *context)
{
  context_ = context;
  render();
}

void ViewportEngine::notify_status(float /*progress*/,
                                   const std::string &info,
                                   const std::string &status)
{
  RE_engine_update_stats(bl_engine_, status.c_str(), info.c_str());
}

}  // namespace blender::render::hydra
