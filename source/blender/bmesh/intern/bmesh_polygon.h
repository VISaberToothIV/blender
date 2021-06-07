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
 */

#pragma once

/** \file
 * \ingroup bmesh
 */

struct Heap;
struct BMPartialUpdate;

#include "BLI_compiler_attrs.h"
#include "BLI_compiler_compat.h"

void BM_face_calc_tessellation(const BMFace *f,
                               const bool use_fixed_quad,
                               BMLoop **r_loops,
                               uint (*r_index)[3]);
void BM_face_calc_point_in_face(const BMFace *f, float r_co[3]);
float BM_face_calc_normal(const BMFace *f, float r_no[3]) ATTR_NONNULL();
float BM_face_calc_normal_vcos(const BMesh *bm,
                               const BMFace *f,
                               float r_no[3],
                               float const (*vertexCos)[3]) ATTR_NONNULL();

void BM_verts_calc_normal_from_cloud_ex(
    BMVert **varr, int varr_len, float r_normal[3], float r_center[3], int *r_index_tangent);
void BM_verts_calc_normal_from_cloud(BMVert **varr, int varr_len, float r_normal[3]);

float BM_face_calc_normal_subset(const BMLoop *l_first, const BMLoop *l_last, float r_no[3])
    ATTR_NONNULL();
float BM_face_calc_area(const BMFace *f) ATTR_WARN_UNUSED_RESULT ATTR_NONNULL();
float BM_face_calc_area_with_mat3(const BMFace *f, const float mat3[3][3]) ATTR_WARN_UNUSED_RESULT
    ATTR_NONNULL();
float BM_face_calc_area_uv(const BMFace *f, int cd_loop_uv_offset) ATTR_WARN_UNUSED_RESULT
    ATTR_NONNULL();
float BM_face_calc_perimeter(const BMFace *f) ATTR_WARN_UNUSED_RESULT ATTR_NONNULL();
float BM_face_calc_perimeter_with_mat3(const BMFace *f,
                                       const float mat3[3][3]) ATTR_WARN_UNUSED_RESULT
    ATTR_NONNULL();
void BM_face_calc_tangent_edge(const BMFace *f, float r_tangent[3]) ATTR_NONNULL();
void BM_face_calc_tangent_edge_pair(const BMFace *f, float r_tangent[3]) ATTR_NONNULL();
void BM_face_calc_tangent_edge_diagonal(const BMFace *f, float r_tangent[3]) ATTR_NONNULL();
void BM_face_calc_tangent_vert_diagonal(const BMFace *f, float r_tangent[3]) ATTR_NONNULL();
void BM_face_calc_tangent_auto(const BMFace *f, float r_tangent[3]) ATTR_NONNULL();
void BM_face_calc_center_bounds(const BMFace *f, float r_cent[3]) ATTR_NONNULL();
void BM_face_calc_center_bounds_vcos(const BMesh *bm,
                                     const BMFace *f,
                                     float r_center[3],
                                     float const (*vertexCos)[3]) ATTR_NONNULL();
void BM_face_calc_center_median(const BMFace *f, float r_center[3]) ATTR_NONNULL();
void BM_face_calc_center_median_vcos(const BMesh *bm,
                                     const BMFace *f,
                                     float r_center[3],
                                     float const (*vertexCos)[3]) ATTR_NONNULL();
void BM_face_calc_center_median_weighted(const BMFace *f, float r_cent[3]) ATTR_NONNULL();

void BM_face_calc_bounds_expand(const BMFace *f, float min[3], float max[3]);

void BM_face_normal_update(BMFace *f) ATTR_NONNULL();

void BM_edge_normals_update(BMEdge *e) ATTR_NONNULL();

bool BM_vert_calc_normal_ex(const BMVert *v, const char hflag, float r_no[3]);
bool BM_vert_calc_normal(const BMVert *v, float r_no[3]);
void BM_vert_normal_update(BMVert *v) ATTR_NONNULL();
void BM_vert_normal_update_all(BMVert *v) ATTR_NONNULL();

void BM_face_normal_flip_ex(BMesh *bm,
                            BMFace *f,
                            const int cd_loop_mdisp_offset,
                            const bool use_loop_mdisp_flip) ATTR_NONNULL();
void BM_face_normal_flip(BMesh *bm, BMFace *f) ATTR_NONNULL();
bool BM_face_point_inside_test(const BMFace *f, const float co[3]) ATTR_WARN_UNUSED_RESULT
    ATTR_NONNULL();

void BM_face_triangulate(BMesh *bm,
                         BMFace *f,
                         BMFace **r_faces_new,
                         int *r_faces_new_tot,
                         BMEdge **r_edges_new,
                         int *r_edges_new_tot,
                         struct LinkNode **r_faces_double,
                         const int quad_method,
                         const int ngon_method,
                         const bool use_tag,
                         struct MemArena *pf_arena,
                         struct Heap *pf_heap) ATTR_NONNULL(1, 2);

void BM_face_splits_check_legal(BMesh *bm, BMFace *f, BMLoop *(*loops)[2], int len) ATTR_NONNULL();
void BM_face_splits_check_optimal(BMFace *f, BMLoop *(*loops)[2], int len) ATTR_NONNULL();

/**
 * Small utility functions for fast access
 *
 * faster alternative to:
 * BM_iter_as_array(bm, BM_VERTS_OF_FACE, f, (void **)v, 3);
 */
BLI_INLINE void BM_face_as_array_vert_tri(BMFace *f, BMVert *r_verts[3])
{
  BMLoop *l = BM_FACE_FIRST_LOOP(f);

  BLI_assert(f->len == 3);

  r_verts[0] = l->v;
  l = l->next;
  r_verts[1] = l->v;
  l = l->next;
  r_verts[2] = l->v;
}

/**
 * Small utility functions for fast access
 *
 * faster alternative to:
 * BM_iter_as_array(bm, BM_LOOPS_OF_FACE, f, (void **)l, 3);
 */
BLI_INLINE void BM_face_as_array_loop_tri(BMFace *f, BMLoop *r_loops[3])
{
  BMLoop *l = BM_FACE_FIRST_LOOP(f);

  BLI_assert(f->len == 3);

  r_loops[0] = l;
  l = l->next;
  r_loops[1] = l;
  l = l->next;
  r_loops[2] = l;
}

void BM_face_as_array_loop_quad(BMFace *f, BMLoop *r_loops[4]) ATTR_NONNULL();

void BM_vert_tri_calc_tangent_edge(BMVert *verts[3], float r_tangent[3]);
void BM_vert_tri_calc_tangent_edge_pair(BMVert *verts[3], float r_tangent[3]);

void BM_face_as_array_vert_quad(BMFace *f, BMVert *r_verts[4]);
