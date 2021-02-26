/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2020 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup edsculpt
 */

#include "MEM_guardedalloc.h"

#include "BLI_blenlib.h"
#include "BLI_hash.h"
#include "BLI_math.h"
#include "BLI_task.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"

#include "BKE_brush.h"
#include "BKE_context.h"
#include "BKE_mesh.h"
#include "BKE_mesh_mapping.h"
#include "BKE_object.h"
#include "BKE_paint.h"
#include "BKE_pbvh.h"
#include "BKE_scene.h"

#include "DEG_depsgraph.h"

#include "WM_api.h"
#include "WM_message.h"
#include "WM_toolsystem.h"
#include "WM_types.h"

#include "ED_object.h"
#include "ED_screen.h"
#include "ED_sculpt.h"
#include "paint_intern.h"
#include "sculpt_intern.h"

#include "RNA_access.h"
#include "RNA_define.h"

#include "UI_interface.h"

#include "bmesh.h"

#include <math.h>
#include <stdlib.h>

typedef enum eSculptMaskFilterTypes {
  MASK_FILTER_SMOOTH = 0,
  MASK_FILTER_SHARPEN = 1,
  MASK_FILTER_GROW = 2,
  MASK_FILTER_SHRINK = 3,
  MASK_FILTER_CONTRAST_INCREASE = 5,
  MASK_FILTER_CONTRAST_DECREASE = 6,
} eSculptMaskFilterTypes;

static EnumPropertyItem prop_mask_filter_types[] = {
    {MASK_FILTER_SMOOTH, "SMOOTH", 0, "Smooth Mask", "Smooth mask"},
    {MASK_FILTER_SHARPEN, "SHARPEN", 0, "Sharpen Mask", "Sharpen mask"},
    {MASK_FILTER_GROW, "GROW", 0, "Grow Mask", "Grow mask"},
    {MASK_FILTER_SHRINK, "SHRINK", 0, "Shrink Mask", "Shrink mask"},
    {MASK_FILTER_CONTRAST_INCREASE,
     "CONTRAST_INCREASE",
     0,
     "Increase Contrast",
     "Increase the contrast of the paint mask"},
    {MASK_FILTER_CONTRAST_DECREASE,
     "CONTRAST_DECREASE",
     0,
     "Decrease Contrast",
     "Decrease the contrast of the paint mask"},
    {0, NULL, 0, NULL, NULL},
};

static void mask_filter_task_cb(void *__restrict userdata,
                                const int i,
                                const TaskParallelTLS *__restrict UNUSED(tls))
{
  SculptThreadedTaskData *data = userdata;
  SculptSession *ss = data->ob->sculpt;
  PBVHNode *node = data->nodes[i];
  bool update = false;

  const int mode = data->filter_type;
  float contrast = 0.0f;

  PBVHVertexIter vd;

  if (mode == MASK_FILTER_CONTRAST_INCREASE) {
    contrast = 0.1f;
  }

  if (mode == MASK_FILTER_CONTRAST_DECREASE) {
    contrast = -0.1f;
  }

  BKE_pbvh_vertex_iter_begin(ss->pbvh, node, vd, PBVH_ITER_UNIQUE)
  {
    float delta, gain, offset, max, min;
    float prev_val = *vd.mask;
    SculptVertexNeighborIter ni;
    switch (mode) {
      case MASK_FILTER_SMOOTH:
      case MASK_FILTER_SHARPEN: {
        float val = SCULPT_neighbor_mask_average(ss, vd.index);

        val -= *vd.mask;

        if (mode == MASK_FILTER_SMOOTH) {
          *vd.mask += val;
        }
        else if (mode == MASK_FILTER_SHARPEN) {
          if (*vd.mask > 0.5f) {
            *vd.mask += 0.05f;
          }
          else {
            *vd.mask -= 0.05f;
          }
          *vd.mask += val / 2.0f;
        }
        break;
      }
      case MASK_FILTER_GROW:
        max = 0.0f;
        SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, vd.index, ni) {
          float vmask_f = data->prev_mask[ni.index];
          if (vmask_f > max) {
            max = vmask_f;
          }
        }
        SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);
        *vd.mask = max;
        break;
      case MASK_FILTER_SHRINK:
        min = 1.0f;
        SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, vd.index, ni) {
          float vmask_f = data->prev_mask[ni.index];
          if (vmask_f < min) {
            min = vmask_f;
          }
        }
        SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);
        *vd.mask = min;
        break;
      case MASK_FILTER_CONTRAST_INCREASE:
      case MASK_FILTER_CONTRAST_DECREASE:
        delta = contrast / 2.0f;
        gain = 1.0f - delta * 2.0f;
        if (contrast > 0) {
          gain = 1.0f / ((gain != 0.0f) ? gain : FLT_EPSILON);
          offset = gain * (-delta);
        }
        else {
          delta *= -1.0f;
          offset = gain * (delta);
        }
        *vd.mask = gain * (*vd.mask) + offset;
        break;
    }
    *vd.mask = clamp_f(*vd.mask, 0.0f, 1.0f);
    if (*vd.mask != prev_val) {
      update = true;
    }
    if (vd.mvert) {
      vd.mvert->flag |= ME_VERT_PBVH_UPDATE;
    }
  }
  BKE_pbvh_vertex_iter_end;

  if (update) {
    BKE_pbvh_node_mark_update_mask(node);
  }
}

static int sculpt_mask_filter_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  SculptSession *ss = ob->sculpt;
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  PBVH *pbvh = ob->sculpt->pbvh;
  PBVHNode **nodes;
  Sculpt *sd = CTX_data_tool_settings(C)->sculpt;
  int totnode;
  int filter_type = RNA_enum_get(op->ptr, "filter_type");

  BKE_sculpt_update_object_for_edit(depsgraph, ob, true, true, false);

  SCULPT_vertex_random_access_ensure(ss);

  if (!ob->sculpt->pmap) {
    return OPERATOR_CANCELLED;
  }

  int num_verts = SCULPT_vertex_count_get(ss);

  BKE_pbvh_search_gather(pbvh, NULL, NULL, &nodes, &totnode);
  SCULPT_undo_push_begin(ob, "Mask filter");

  for (int i = 0; i < totnode; i++) {
    SCULPT_undo_push_node(ob, nodes[i], SCULPT_UNDO_MASK);
  }

  float *prev_mask = NULL;
  int iterations = RNA_int_get(op->ptr, "iterations");

  /* Auto iteration count calculates the number of iteration based on the vertices of the mesh to
   * avoid adding an unnecessary amount of undo steps when using the operator from a shortcut.
   * One iteration per 50000 vertices in the mesh should be fine in most cases.
   * Maybe we want this to be configurable. */
  if (RNA_boolean_get(op->ptr, "auto_iteration_count")) {
    iterations = (int)(num_verts / 50000.0f) + 1;
  }

  for (int i = 0; i < iterations; i++) {
    if (ELEM(filter_type, MASK_FILTER_GROW, MASK_FILTER_SHRINK)) {
      prev_mask = MEM_mallocN(num_verts * sizeof(float), "prevmask");
      for (int j = 0; j < num_verts; j++) {
        prev_mask[j] = SCULPT_vertex_mask_get(ss, j);
      }
    }

    SculptThreadedTaskData data = {
        .sd = sd,
        .ob = ob,
        .nodes = nodes,
        .filter_type = filter_type,
        .prev_mask = prev_mask,
    };

    TaskParallelSettings settings;
    BKE_pbvh_parallel_range_settings(&settings, true, totnode);
    BLI_task_parallel_range(0, totnode, &data, mask_filter_task_cb, &settings);

    if (ELEM(filter_type, MASK_FILTER_GROW, MASK_FILTER_SHRINK)) {
      MEM_freeN(prev_mask);
    }
  }

  MEM_SAFE_FREE(nodes);

  SCULPT_undo_push_end();

  SCULPT_tag_update_overlays(C);

  return OPERATOR_FINISHED;
}

void SCULPT_mask_filter_smooth_apply(
    Sculpt *sd, Object *ob, PBVHNode **nodes, const int totnode, const int smooth_iterations)
{
  SculptThreadedTaskData data = {
      .sd = sd,
      .ob = ob,
      .nodes = nodes,
      .filter_type = MASK_FILTER_SMOOTH,
  };

  for (int i = 0; i < smooth_iterations; i++) {
    TaskParallelSettings settings;
    BKE_pbvh_parallel_range_settings(&settings, true, totnode);
    BLI_task_parallel_range(0, totnode, &data, mask_filter_task_cb, &settings);
  }
}

void SCULPT_OT_mask_filter(struct wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Mask Filter";
  ot->idname = "SCULPT_OT_mask_filter";
  ot->description = "Applies a filter to modify the current mask";

  /* API callbacks. */
  ot->exec = sculpt_mask_filter_exec;
  ot->poll = SCULPT_mode_poll;

  ot->flag = OPTYPE_REGISTER;

  /* RNA. */
  RNA_def_enum(ot->srna,
               "filter_type",
               prop_mask_filter_types,
               MASK_FILTER_SMOOTH,
               "Type",
               "Filter that is going to be applied to the mask");
  RNA_def_int(ot->srna,
              "iterations",
              1,
              1,
              100,
              "Iterations",
              "Number of times that the filter is going to be applied",
              1,
              100);
  RNA_def_boolean(
      ot->srna,
      "auto_iteration_count",
      false,
      "Auto Iteration Count",
      "Use a automatic number of iterations based on the number of vertices of the sculpt");
}


/******************************************************************************************/
/* Interactive Preview Mask Filter */


#define SCULPT_IPMASK_FILTER_MIN_MULTITHREAD 1000
#define SCULPT_IPMASK_FILTER_GRANULARITY 1000

typedef enum eSculptIPMaskFilterType {
  IPMASK_FILTER_SMOOTH_SHARPEN,
  IPMASK_FILTER_GROW_SHRINK,
  IPMASK_FILTER_CONTRAST_INCREASE_DECREASE,
  IPMASK_FILTER_INTENSITY_INCREASE_DECREASE,
  IPMASK_FILTER_LINEAR_INTENSITY_INCREASE_DECREASE,
} eSculptIPMaskFilterType;

static EnumPropertyItem prop_ipmask_filter_types[] = {
    {IPMASK_FILTER_SMOOTH_SHARPEN, "SMOOTH_SHARPEN", 0, "Smooth/Sharpen", "Smooth and sharpen the mask"},
    {IPMASK_FILTER_GROW_SHRINK, "GROW_SHRINK", 0, "Grow/Shrink", "Grow and shirnk the mask"},
    {0, NULL, 0, NULL, NULL},
};


typedef enum MaskFilterStepDirectionType {
    MASK_FILTER_STEP_DIRECTION_FORWARD,
    MASK_FILTER_STEP_DIRECTION_BACKWARD,
} MaskFilterStepDirectionType;


/* Grown/Shrink vertex callbacks. */
static float sculpt_ipmask_vertex_grow_cb(SculptSession *ss, const int vertex,  float *current_mask) {
        float max = 0.0f;
        SculptVertexNeighborIter ni;
        SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, vertex, ni) {
          float vmask_f = current_mask[ni.index];
          if (vmask_f > max) {
            max = vmask_f;
          }
        }
        SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);
        return max;
}

static float sculpt_ipmask_vertex_shrink_cb(SculptSession *ss, const int vertex,  float *current_mask) {
        float min = 1.0f;
        SculptVertexNeighborIter ni;
        SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, vertex, ni) {
          float vmask_f = current_mask[ni.index];
          if (vmask_f < min) {
            min = vmask_f;
          }
        }
        SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);
        return min;
}

/* Smooth/Sharpen vertex callbacks. */
static float sculpt_ipmask_vertex_smooth_cb(SculptSession *ss, const int vertex,  float *current_mask) {
  float accum = 0.0f;
  int total = 0;
  SculptVertexNeighborIter ni;
  SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, vertex, ni) {
    accum += current_mask[ni.index];
    total++;
  }
  SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);
  return total > 0? accum/total : current_mask[vertex];
}

static float sculpt_ipmask_vertex_sharpen_cb(SculptSession *ss, const int vertex, float *current_mask) {
  float accum = 0.0f;
  int total = 0;
  float vmask = current_mask[vertex];
  SculptVertexNeighborIter ni;
  SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, vertex, ni) {
    accum += current_mask[ni.index];
    total++;
  }
  SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);
  const float avg = total > 0? accum/total : current_mask[vertex];
  const float val = avg - vmask;

  float new_mask;
          if (vmask > 0.5f) {
            new_mask = vmask +  0.03f;
          }
          else {
            new_mask = vmask - 0.03f;
          }
          new_mask += val / 2.0f;
    return clamp_f(new_mask, 0.0f, 1.0f);

}


static MaskFilterDeltaStep *sculpt_ipmask_filter_delta_create(const float *current_mask, const float *next_mask, const int totvert) {
    int tot_modified_values = 0;
    for (int i = 0; i < totvert; i++) {
        if (current_mask[i] == next_mask[i]) {
            continue;
        }
        tot_modified_values++;
    }

    MaskFilterDeltaStep *delta_step = MEM_callocN(sizeof (MaskFilterDeltaStep), "mask filter delta step");
    delta_step->totelem = tot_modified_values;
    delta_step->index = MEM_malloc_arrayN(sizeof (int), tot_modified_values, "delta indices");
    delta_step->delta = MEM_malloc_arrayN(sizeof (float), tot_modified_values, "delta values");

    int delta_step_index = 0;
    for (int i = 0; i < totvert; i++) {
        if (current_mask[i] == next_mask[i]) {
            continue;
        }
        delta_step->index[delta_step_index] = i;
        delta_step->delta[delta_step_index] = next_mask[i] - current_mask[i];
        delta_step_index++;
    }
    printf("DELTA STEP SIZE %d\n", delta_step->totelem);
    return delta_step;
}

typedef struct SculptIPMaskFilterTaskData {
    SculptSession *ss;
    float *next_mask;
    float *current_mask;
    MaskFilterStepDirectionType direction;
} SculptIPMaskFilterTaskData;

static void ipmask_filter_compute_step_task_cb(void *__restrict userdata,
                                const int i,
                                const TaskParallelTLS *__restrict UNUSED(tls))
{
  SculptIPMaskFilterTaskData *data = userdata;
        if (data->direction == MASK_FILTER_STEP_DIRECTION_FORWARD) {
            data->next_mask[i] = data->ss->filter_cache->mask_filter_step_forward(data->ss, i, data->current_mask);
        }
        else {
            data->next_mask[i] = data->ss->filter_cache->mask_filter_step_backward(data->ss, i, data->current_mask);
        }
}

static float *sculpt_ipmask_step_compute(SculptSession *ss, float *current_mask, MaskFilterStepDirectionType direction) {
    const int totvert = SCULPT_vertex_count_get(ss);
    float *next_mask = MEM_malloc_arrayN(sizeof (float), totvert, "delta values");

    SculptIPMaskFilterTaskData data = {
        .ss = ss,
        .next_mask = next_mask,
        .current_mask = current_mask,
        .direction = direction,
    };
    TaskParallelSettings settings;
    memset(&settings, 0, sizeof(TaskParallelSettings));
    settings.use_threading = totvert > SCULPT_IPMASK_FILTER_MIN_MULTITHREAD;
    BLI_task_parallel_range(0, totvert, &data, ipmask_filter_compute_step_task_cb, &settings);

    return next_mask;
}

static float *sculpt_ipmask_current_state_get(SculptSession *ss) {
    const int totvert = SCULPT_vertex_count_get(ss);
    float *current_mask = MEM_malloc_arrayN(sizeof (float), totvert, "delta values");

    for (int i = 0; i < totvert; i++) {
        current_mask[i] = SCULPT_vertex_mask_get(ss, i);
    }

    return current_mask;
}

static void ipmask_filter_apply_task_cb(void *__restrict userdata,
                                const int i,
                                const TaskParallelTLS *__restrict UNUSED(tls))
{
  SculptThreadedTaskData *data = userdata;
  SculptSession *ss = data->ss;
  FilterCache *filter_cache = ss->filter_cache;
  PBVHNode *node = filter_cache->nodes[i];
  PBVHVertexIter vd;
  bool update = false;
  BKE_pbvh_vertex_iter_begin(ss->pbvh, node, vd, PBVH_ITER_UNIQUE)
  {
      if (SCULPT_automasking_factor_get(filter_cache->automasking, ss, vd.index) < 0.5f) {
          continue;
      }

      if (*vd.mask == data->new_mask[vd.index]) {
          continue;
      }
      *vd.mask = data->new_mask[vd.index];
      update = true;
      if (vd.mvert) {
        vd.mvert->flag |= ME_VERT_PBVH_UPDATE;
       }
  }
  BKE_pbvh_vertex_iter_end;

  if (update) {
    BKE_pbvh_node_mark_redraw(node);
  }

}

static void sculpt_ipmask_apply_mask_data(SculptSession *ss, float *new_mask) {
    FilterCache *filter_cache = ss->filter_cache;
    SculptThreadedTaskData data = {
        .ss = ss,
        .nodes = filter_cache->nodes,
        .new_mask = new_mask,
    };

    TaskParallelSettings settings;
    BKE_pbvh_parallel_range_settings(&settings, true, filter_cache->totnode);
    BLI_task_parallel_range(0, filter_cache->totnode, &data, ipmask_filter_apply_task_cb, &settings);
}

static float *sculpt_ipmask_apply_delta_step(MaskFilterDeltaStep *delta_step, const float *current_mask, const MaskFilterStepDirectionType direction) {
    float *next_mask = MEM_dupallocN(current_mask);
    for (int i = 0; i < delta_step->totelem; i++) {
        if (direction == MASK_FILTER_STEP_DIRECTION_FORWARD) {
            next_mask[delta_step->index[i]] = current_mask[delta_step->index[i]] + delta_step->delta[i];
        }
        else {
            next_mask[delta_step->index[i]] = current_mask[delta_step->index[i]] - delta_step->delta[i];
        }
    }
    return next_mask;
}


static void sculpt_ipmask_restore_state_from_delta(SculptSession *ss, MaskFilterDeltaStep *delta_step, MaskFilterStepDirectionType direction) {
          float *current_mask = sculpt_ipmask_current_state_get(ss);
          float *next_mask = sculpt_ipmask_apply_delta_step(delta_step, current_mask, direction);
          MEM_freeN(current_mask);
          sculpt_ipmask_apply_mask_data(ss, next_mask);
          MEM_freeN(next_mask);
}

static void sculpt_ipmask_compute_and_store_step(SculptSession *ss, const int iterations, const int delta_index, MaskFilterStepDirectionType direction) {
          BLI_assert(iterations > 0);
          const int totvert = SCULPT_vertex_count_get(ss);
          float *current_mask = sculpt_ipmask_current_state_get(ss);
          float *original_mask = MEM_dupallocN(current_mask);
          float *next_mask = NULL;

          /* Compute the filter. */
          for (int i = 0; i < iterations; i++) {
            MEM_SAFE_FREE(next_mask);
            next_mask = sculpt_ipmask_step_compute(ss, current_mask, direction);
            MEM_freeN(current_mask);
            current_mask = MEM_dupallocN(next_mask);
          }

          /* Pack and store the delta step. */
          MaskFilterDeltaStep *delta_step;
          if (direction == MASK_FILTER_STEP_DIRECTION_FORWARD) {
            delta_step = sculpt_ipmask_filter_delta_create(original_mask, next_mask, totvert);
          }
          else {
            delta_step = sculpt_ipmask_filter_delta_create(next_mask, original_mask, totvert);
          }
          BLI_ghash_insert(ss->filter_cache->mask_delta_step, POINTER_FROM_INT(delta_index), delta_step);
          MEM_freeN(original_mask);


          /* Store the result in the sculpt mesh. */
          sculpt_ipmask_apply_mask_data(ss, next_mask);
          MEM_freeN(next_mask);
}

#define IPMASK_FILTER_STEP_SENSITIVITY 0.05f
static int sculpt_ipmask_filter_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  Object *ob = CTX_data_active_object(C);
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  SculptSession *ss = ob->sculpt;
  FilterCache *filter_cache = ss->filter_cache;
  const int iterations = RNA_int_get(op->ptr, "iterations");

  if (event->type == LEFTMOUSE && event->val == KM_RELEASE) {

  for (int i = 0; i < filter_cache->totnode; i++) {
      BKE_pbvh_node_mark_update_mask(filter_cache->nodes[i]);
  }
    SCULPT_filter_cache_free(ss);
    SCULPT_undo_push_end();
    SCULPT_flush_update_done(C, ob, SCULPT_UPDATE_MASK);
    return OPERATOR_FINISHED;
  }

  if (event->type != MOUSEMOVE) {
    return OPERATOR_RUNNING_MODAL;
  }


  const float len = event->x - event->prevclickx;
  const int target_step = len * IPMASK_FILTER_STEP_SENSITIVITY * UI_DPI_FAC;
  if (target_step == filter_cache->mask_filter_current_step) {
      return OPERATOR_RUNNING_MODAL;
  }

  BKE_sculpt_update_object_for_edit(depsgraph, ob, true, true, false);


  while(filter_cache->mask_filter_current_step != target_step) {
      int next_step = filter_cache->mask_filter_current_step;
      int delta_index = next_step;
      MaskFilterStepDirectionType direction;

      if (target_step > filter_cache->mask_filter_current_step) {
          next_step = filter_cache->mask_filter_current_step + 1;
          delta_index = filter_cache->mask_filter_current_step;
          direction = MASK_FILTER_STEP_DIRECTION_FORWARD;
      }
      else {
          next_step = filter_cache->mask_filter_current_step -1;
          delta_index = filter_cache->mask_filter_current_step -1;
          direction = MASK_FILTER_STEP_DIRECTION_BACKWARD;
      }


      if (BLI_ghash_haskey(filter_cache->mask_delta_step, POINTER_FROM_INT(delta_index))) {
          MaskFilterDeltaStep *delta_step = BLI_ghash_lookup(filter_cache->mask_delta_step, POINTER_FROM_INT(delta_index));
          sculpt_ipmask_restore_state_from_delta(ss, delta_step, direction);
      }
      else {
          sculpt_ipmask_compute_and_store_step(ss, iterations, delta_index, direction);
      }
      filter_cache->mask_filter_current_step = next_step;
  }


  SCULPT_tag_update_overlays(C);

  return OPERATOR_RUNNING_MODAL;
}


static int sculpt_ipmask_filter_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  Object *ob = CTX_data_active_object(C);
  Sculpt *sd = CTX_data_tool_settings(C)->sculpt;
  SculptSession *ss = ob->sculpt;
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);

  SCULPT_undo_push_begin(ob, "mask filter");

  BKE_sculpt_update_object_for_edit(depsgraph, ob, true, true, false);


  ss->filter_cache = MEM_callocN(sizeof(FilterCache), "filter cache");

  FilterCache *filter_cache = ss->filter_cache;
  filter_cache->active_face_set = SCULPT_FACE_SET_NONE;
  filter_cache->automasking = SCULPT_automasking_cache_init(sd, NULL, ob);
  filter_cache->mask_filter_current_step = 0;

  BKE_pbvh_search_gather(ss->pbvh, NULL, NULL, &filter_cache->nodes, &filter_cache->totnode);
  for (int i = 0; i < filter_cache->totnode; i++) {
    SCULPT_undo_push_node(ob, filter_cache->nodes[i], SCULPT_UNDO_MASK);
  }

  filter_cache->mask_delta_step = BLI_ghash_int_new("mask filter delta steps");

  const int filter_type= RNA_enum_get(op->ptr, "filter_type");
  switch (filter_type) {
    case IPMASK_FILTER_SMOOTH_SHARPEN:
      filter_cache->mask_filter_step_forward = sculpt_ipmask_vertex_smooth_cb;
      filter_cache->mask_filter_step_backward = sculpt_ipmask_vertex_sharpen_cb;
      break;
    case IPMASK_FILTER_GROW_SHRINK:
      filter_cache->mask_filter_step_forward = sculpt_ipmask_vertex_grow_cb;
      filter_cache->mask_filter_step_backward = sculpt_ipmask_vertex_shrink_cb;
      break;
  }

  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}
static int sculpt_ipmask_filter_exec(bContext *C, wmOperator *op)
{
    return OPERATOR_FINISHED;
}


void SCULPT_OT_ipmask_filter(struct wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Interactive Preview Mask Filter";
  ot->idname = "SCULPT_OT_ipmask_filter";
  ot->description = "Applies a filter to modify the current mask";

  /* API callbacks. */
  ot->exec = sculpt_ipmask_filter_exec;
  ot->invoke = sculpt_ipmask_filter_invoke;
  ot->modal = sculpt_ipmask_filter_modal;
  ot->poll = SCULPT_mode_poll;

  ot->flag = OPTYPE_REGISTER;

  /* RNA. */
  RNA_def_enum(ot->srna,
               "filter_type",
               prop_ipmask_filter_types,
               IPMASK_FILTER_GROW_SHRINK,
               "Type",
               "Filter that is going to be applied to the mask");
  RNA_def_int(ot->srna,
              "iterations",
              1,
              1,
              100,
              "Iterations per Step",
              "Number of times that the filter is going to be applied per step",
              1,
              100);
}

/******************************************************************************************/

static float neighbor_dirty_mask(SculptSession *ss, PBVHVertexIter *vd)
{
  int total = 0;
  float avg[3];
  zero_v3(avg);

  SculptVertexNeighborIter ni;
  SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, vd->index, ni) {
    float normalized[3];
    sub_v3_v3v3(normalized, SCULPT_vertex_co_get(ss, ni.index), vd->co);
    normalize_v3(normalized);
    add_v3_v3(avg, normalized);
    total++;
  }
  SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);

  if (total > 0) {
    mul_v3_fl(avg, 1.0f / total);
    float normal[3];
    if (vd->no) {
      normal_short_to_float_v3(normal, vd->no);
    }
    else {
      copy_v3_v3(normal, vd->fno);
    }
    float dot = dot_v3v3(avg, normal);
    float angle = max_ff(saacosf(dot), 0.0f);
    return angle;
  }
  return 0.0f;
}

typedef struct DirtyMaskRangeData {
  float min, max;
} DirtyMaskRangeData;

static void dirty_mask_compute_range_task_cb(void *__restrict userdata,
                                             const int i,
                                             const TaskParallelTLS *__restrict tls)
{
  SculptThreadedTaskData *data = userdata;
  SculptSession *ss = data->ob->sculpt;
  PBVHNode *node = data->nodes[i];
  DirtyMaskRangeData *range = tls->userdata_chunk;
  PBVHVertexIter vd;

  BKE_pbvh_vertex_iter_begin(ss->pbvh, node, vd, PBVH_ITER_UNIQUE)
  {
    float dirty_mask = neighbor_dirty_mask(ss, &vd);
    range->min = min_ff(dirty_mask, range->min);
    range->max = max_ff(dirty_mask, range->max);
  }
  BKE_pbvh_vertex_iter_end;
}

static void dirty_mask_compute_range_reduce(const void *__restrict UNUSED(userdata),
                                            void *__restrict chunk_join,
                                            void *__restrict chunk)
{
  DirtyMaskRangeData *join = chunk_join;
  DirtyMaskRangeData *range = chunk;
  join->min = min_ff(range->min, join->min);
  join->max = max_ff(range->max, join->max);
}

static void dirty_mask_apply_task_cb(void *__restrict userdata,
                                     const int i,
                                     const TaskParallelTLS *__restrict UNUSED(tls))
{
  SculptThreadedTaskData *data = userdata;
  SculptSession *ss = data->ob->sculpt;
  PBVHNode *node = data->nodes[i];
  PBVHVertexIter vd;

  const bool dirty_only = data->dirty_mask_dirty_only;
  const float min = data->dirty_mask_min;
  const float max = data->dirty_mask_max;

  float range = max - min;
  if (range < 0.0001f) {
    range = 0.0f;
  }
  else {
    range = 1.0f / range;
  }

  BKE_pbvh_vertex_iter_begin(ss->pbvh, node, vd, PBVH_ITER_UNIQUE)
  {
    float dirty_mask = neighbor_dirty_mask(ss, &vd);
    float mask = *vd.mask + (1.0f - ((dirty_mask - min) * range));
    if (dirty_only) {
      mask = fminf(mask, 0.5f) * 2.0f;
    }
    *vd.mask = CLAMPIS(mask, 0.0f, 1.0f);

    if (vd.mvert) {
      vd.mvert->flag |= ME_VERT_PBVH_UPDATE;
    }
  }
  BKE_pbvh_vertex_iter_end;
  BKE_pbvh_node_mark_update_mask(node);
}

static int sculpt_dirty_mask_exec(bContext *C, wmOperator *op)
{
  ARegion *region = CTX_wm_region(C);
  Object *ob = CTX_data_active_object(C);
  SculptSession *ss = ob->sculpt;
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  PBVH *pbvh = ob->sculpt->pbvh;
  PBVHNode **nodes;
  Sculpt *sd = CTX_data_tool_settings(C)->sculpt;
  int totnode;

  BKE_sculpt_update_object_for_edit(depsgraph, ob, true, true, false);

  SCULPT_vertex_random_access_ensure(ss);

  if (!ob->sculpt->pmap) {
    return OPERATOR_CANCELLED;
  }

  BKE_pbvh_search_gather(pbvh, NULL, NULL, &nodes, &totnode);
  SCULPT_undo_push_begin(ob, "Dirty Mask");

  for (int i = 0; i < totnode; i++) {
    SCULPT_undo_push_node(ob, nodes[i], SCULPT_UNDO_MASK);
  }

  SculptThreadedTaskData data = {
      .sd = sd,
      .ob = ob,
      .nodes = nodes,
      .dirty_mask_dirty_only = RNA_boolean_get(op->ptr, "dirty_only"),
  };
  DirtyMaskRangeData range = {
      .min = FLT_MAX,
      .max = -FLT_MAX,
  };

  TaskParallelSettings settings;
  BKE_pbvh_parallel_range_settings(&settings, true, totnode);

  settings.func_reduce = dirty_mask_compute_range_reduce;
  settings.userdata_chunk = &range;
  settings.userdata_chunk_size = sizeof(DirtyMaskRangeData);

  BLI_task_parallel_range(0, totnode, &data, dirty_mask_compute_range_task_cb, &settings);
  data.dirty_mask_min = range.min;
  data.dirty_mask_max = range.max;
  BLI_task_parallel_range(0, totnode, &data, dirty_mask_apply_task_cb, &settings);

  MEM_SAFE_FREE(nodes);

  BKE_pbvh_update_vertex_data(pbvh, PBVH_UpdateMask);

  SCULPT_undo_push_end();

  ED_region_tag_redraw(region);

  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, ob);

  return OPERATOR_FINISHED;
}

void SCULPT_OT_dirty_mask(struct wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Dirty Mask";
  ot->idname = "SCULPT_OT_dirty_mask";
  ot->description = "Generates a mask based on the geometry cavity and pointiness";

  /* API callbacks. */
  ot->exec = sculpt_dirty_mask_exec;
  ot->poll = SCULPT_mode_poll;

  ot->flag = OPTYPE_REGISTER;

  /* RNA. */
  RNA_def_boolean(
      ot->srna, "dirty_only", false, "Dirty Only", "Don't calculate cleans for convex areas");
}
