/* SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_compiler_compat.h"
#include "BLI_ghash.h"

#include "DNA_customdata_types.h"
#include "DNA_material_types.h"

#include "BKE_attribute.h"
#include "BKE_paint.h" /* for SCULPT_BOUNDARY_NEEDS_UPDATE */
#include "BKE_pbvh.h"

#include "../../bmesh/intern/bmesh_idmap.h"
#include "bmesh.h"

#define PBVH_STACK_FIXED_DEPTH 100

struct PBVHGPUFormat;

/** \file
 * \ingroup bke
 */
struct MSculptVert;
struct CustomData;
struct PBVHTriBuf;

#ifdef __cplusplus
extern "C" {
#endif

struct MLoop;
struct MLoopTri;
struct BMIdMap;
struct MPoly;

/* Axis-aligned bounding box */
typedef struct {
  float bmin[3], bmax[3];
} BB;

/* Axis-aligned bounding box with centroid */
typedef struct {
  float bmin[3], bmax[3], bcentroid[3];
} BBC;

struct MeshElemMap;

/* NOTE: this structure is getting large, might want to split it into
 * union'd structs */
struct PBVHNode {
  /* Opaque handle for drawing code */
  struct PBVHBatches *draw_batches;

  /* Voxel bounds */
  BB vb;
  BB orig_vb;

  /* For internal nodes, the offset of the children in the PBVH
   * 'nodes' array. */
  int children_offset;
  int subtree_tottri;

  int depth;

  /* List of primitives for this node. Semantics depends on
   * PBVH type:
   *
   * - PBVH_FACES: Indices into the PBVH.looptri array.
   * - PBVH_GRIDS: Multires grid indices.
   * - PBVH_BMESH: Unused.  See PBVHNode.bm_faces.
   *
   * NOTE: This is a pointer inside of PBVH.prim_indices; it
   * is not allocated separately per node.
   */
  int *prim_indices;
  unsigned int totprim; /* Number of primitives inside prim_indices. */

  /* Array of indices into the mesh's vertex array. Contains the
   * indices of all vertices used by faces that are within this
   * node's bounding box.
   *
   * Note that a vertex might be used by a multiple faces, and
   * these faces might be in different leaf nodes. Such a vertex
   * will appear in the vert_indices array of each of those leaf
   * nodes.
   *
   * In order to support cases where you want access to multiple
   * nodes' vertices without duplication, the vert_indices array
   * is ordered such that the first part of the array, up to
   * index 'uniq_verts', contains "unique" vertex indices. These
   * vertices might not be truly unique to this node, but if
   * they appear in another node's vert_indices array, they will
   * be above that node's 'uniq_verts' value.
   *
   * Used for leaf nodes in a mesh-based PBVH (not multires.)
   */
  const int *vert_indices;
  unsigned int uniq_verts, face_verts;

  /* Array of indices into the Mesh's MLoop array.
   * PBVH_FACES only.
   */
  int *loop_indices;
  unsigned int loop_indices_num;

  /* An array mapping face corners into the vert_indices
   * array. The array is sized to match 'totprim', and each of
   * the face's corners gets an index into the vert_indices
   * array, in the same order as the corners in the original
   * MLoopTri.
   *
   * Used for leaf nodes in a mesh-based PBVH (not multires.)
   */
  const int (*face_vert_indices)[3];

  /* Indicates whether this node is a leaf or not; also used for
   * marking various updates that need to be applied. */
  PBVHNodeFlags flag;

  /* Used for raycasting: how close bb is to the ray point. */
  float tmin;

  /* Scalar displacements for sculpt mode's layer brush. */
  float *layer_disp;

  int proxy_count;
  PBVHProxyNode *proxies;

  /* GSet of pointers to the BMFaces used by this node.
   * NOTE: PBVH_BMESH only.
   */
  TableGSet *bm_faces;
  TableGSet *bm_unique_verts;
  TableGSet *bm_other_verts;

  PBVHTriBuf *tribuf;       // all triangles
  PBVHTriBuf *tri_buffers;  // tribuffers, one per material used
  int tot_tri_buffers;

  int updategen;

#ifdef PROXY_ADVANCED
  ProxyVertArray proxyverts;
#endif
  PBVHPixelsNode pixels;

  /* Used to flash colors of updated node bounding boxes in
   * debug draw mode (when G.debug_value / bpy.app.debug_value is 889).
   */
  int debug_draw_gen;
  int id;
};

typedef enum {
  PBVH_DYNTOPO_SMOOTH_SHADING = 1,
  PBVH_FAST_DRAW = 2,  // hides facesets/masks and forces smooth to save GPU bandwidth
  PBVH_IGNORE_UVS = 4
} PBVHFlags;

typedef struct PBVHBMeshLog PBVHBMeshLog;
struct DMFlagMat;

struct PBVH {
  struct PBVHPublic header;
  PBVHFlags flags;

  int idgen;

  bool dyntopo_stop;

  PBVHNode *nodes;
  int node_mem_count, totnode;

  /* Memory backing for PBVHNode.prim_indices. */
  int *prim_indices;
  int totprim;
  int totvert;
  int faces_num; /* Do not use directly, use BKE_pbvh_num_faces. */

  int leaf_limit;

  /* Mesh data */
  struct MeshElemMap *vemap;
  SculptPMap *pmap;

  struct Mesh *mesh;

  /* NOTE: Normals are not `const` because they can be updated for drawing by sculpt code. */
  float (*vert_normals)[3];
  bool *hide_vert;
  float (*vert_positions)[3];
  const struct MPoly *mpoly;
  bool *hide_poly;
  /** Material indices. Only valid for polygon meshes. */
  const int *material_indices;
  const struct MLoop *mloop;
  const struct MLoopTri *looptri;
  struct MSculptVert *msculptverts;

  CustomData *vdata;
  CustomData *ldata;
  CustomData *pdata;

  int face_sets_color_seed;
  int face_sets_color_default;
  int *face_sets;
  float *face_areas; /* float2 vector, double buffered to avoid thread contention */
  int face_area_i;

  /* Grid Data */
  CCGKey gridkey;
  CCGElem **grids;
  void **gridfaces;
  const struct DMFlagMat *grid_flag_mats;
  int totgrid;
  BLI_bitmap **grid_hidden;

  /* Used during BVH build and later to mark that a vertex needs to update
   * (its normal must be recalculated). */
  bool *vert_bitmap;

#ifdef PERFCNTRS
  int perf_modified;
#endif

  /* flag are verts/faces deformed */
  bool deformed;
  bool respect_hide;

  /* Dynamic topology */
  float bm_max_edge_len;
  float bm_min_edge_len;
  float bm_detail_range;
  struct BMIdMap *bm_idmap;

  int cd_sculpt_vert;
  int cd_vert_node_offset;
  int cd_face_node_offset;
  int cd_vert_mask_offset;
  int cd_faceset_offset;
  int cd_face_area;
  int cd_vcol_offset;
  int cd_hide_poly;

  int totuv;

  float planes[6][4];
  int num_planes;

  int symmetry;
  int boundary_symmetry;

  BMLog *bm_log;
  struct SubdivCCG *subdiv_ccg;

  bool flat_vcol_shading;
  bool need_full_render;  // used by pbvh drawing for PBVH_BMESH

  int balance_counter;
  int stroke_id;  // used to keep origdata up to date in PBVH_BMESH

  bool is_cached;

  /* This data is for validating cached PBVHs;
   * it is not guaranteed to be valid in any way! */
  struct {
    CustomData vdata, edata, ldata, pdata;
    int totvert, totedge, totloop, totpoly;
    struct BMesh *bm;
  } cached_data;

  bool invalid;

  CustomDataLayer *color_layer;
  eAttrDomain color_domain;

  bool is_drawing;

  /* Used by DynTopo to invalidate the draw cache. */
  bool draw_cache_invalid;

  struct PBVHGPUFormat *vbo_id;
  int *boundary_flags;
  int cd_boundary_flag;

  PBVHPixels pixels;
};

/* pbvh.c */

void BB_reset(BB *bb);
/**
 * Expand the bounding box to include a new coordinate.
 */
void BB_expand(BB *bb, const float co[3]);
/**
 * Expand the bounding box to include another bounding box.
 */
void BB_expand_with_bb(BB *bb, BB *bb2);
void BBC_update_centroid(BBC *bbc);
/**
 * Return 0, 1, or 2 to indicate the widest axis of the bounding box.
 */
int BB_widest_axis(const BB *bb);
void BB_intersect(BB *r_out, BB *a, BB *b);
float BB_volume(const BB *bb);

void pbvh_grow_nodes(PBVH *bvh, int totnode);
bool ray_face_intersection_quad(const float ray_start[3],
                                struct IsectRayPrecalc *isect_precalc,
                                const float t0[3],
                                const float t1[3],
                                const float t2[3],
                                const float t3[3],
                                float *depth);
bool ray_face_intersection_tri(const float ray_start[3],
                               struct IsectRayPrecalc *isect_precalc,
                               const float t0[3],
                               const float t1[3],
                               const float t2[3],
                               float *depth);

bool ray_face_nearest_quad(const float ray_start[3],
                           const float ray_normal[3],
                           const float t0[3],
                           const float t1[3],
                           const float t2[3],
                           const float t3[3],
                           float *r_depth,
                           float *r_dist_sq);
bool ray_face_nearest_tri(const float ray_start[3],
                          const float ray_normal[3],
                          const float t0[3],
                          const float t1[3],
                          const float t2[3],
                          float *r_depth,
                          float *r_dist_sq);

void pbvh_update_BB_redraw(PBVH *bvh, PBVHNode **nodes, int totnode, int flag);

bool ray_face_intersection_depth_tri(const float ray_start[3],
                                     struct IsectRayPrecalc *isect_precalc,
                                     const float t0[3],
                                     const float t1[3],
                                     const float t2[3],
                                     float *r_depth,
                                     float *r_back_depth,
                                     int *hit_count);

/* pbvh_bmesh.c */
bool pbvh_bmesh_node_raycast(PBVH *pbvh,
                             PBVHNode *node,
                             const float ray_start[3],
                             const float ray_normal[3],
                             struct IsectRayPrecalc *isect_precalc,
                             int *hit_count,
                             float *depth,
                             float *back_depth,
                             bool use_original,
                             PBVHVertRef *r_active_vertex_index,
                             PBVHFaceRef *r_active_face_index,
                             float *r_face_normal,
                             int stroke_id);

bool pbvh_bmesh_node_nearest_to_ray(PBVH *pbvh,
                                    PBVHNode *node,
                                    const float ray_start[3],
                                    const float ray_normal[3],
                                    float *depth,
                                    float *dist_sq,
                                    bool use_original,
                                    int stroke_id);

void pbvh_bmesh_normals_update(PBVH *pbvh, PBVHNode **nodes, int totnode);

void pbvh_update_free_all_draw_buffers(PBVH *pbvh, PBVHNode *node);

BLI_INLINE int pbvh_bmesh_node_index_from_vert(PBVH *pbvh, const BMVert *key)
{
  const int node_index = BM_ELEM_CD_GET_INT((const BMElem *)key, pbvh->cd_vert_node_offset);
  BLI_assert(node_index != DYNTOPO_NODE_NONE);
  BLI_assert(node_index << pbvh->totnode);
  return node_index;
}

BLI_INLINE int pbvh_bmesh_node_index_from_face(PBVH *pbvh, const BMFace *key)
{
  const int node_index = BM_ELEM_CD_GET_INT((const BMElem *)key, pbvh->cd_face_node_offset);
  BLI_assert(node_index != DYNTOPO_NODE_NONE);
  BLI_assert(node_index < pbvh->totnode);
  return node_index;
}

BLI_INLINE PBVHNode *pbvh_bmesh_node_from_vert(PBVH *pbvh, const BMVert *key)
{
  int ni = pbvh_bmesh_node_index_from_vert(pbvh, key);

  return ni >= 0 ? pbvh->nodes + ni : NULL;
  // return &pbvh->nodes[pbvh_bmesh_node_index_from_vert(pbvh, key)];
}

BLI_INLINE PBVHNode *pbvh_bmesh_node_from_face(PBVH *pbvh, const BMFace *key)
{
  int ni = pbvh_bmesh_node_index_from_face(pbvh, key);

  return ni >= 0 ? pbvh->nodes + ni : NULL;
  // return &pbvh->nodes[pbvh_bmesh_node_index_from_face(pbvh, key)];
}

bool pbvh_bmesh_node_limit_ensure(PBVH *pbvh, int node_index);

//#define PBVH_BMESH_DEBUG

#ifdef PBVH_BMESH_DEBUG
void pbvh_bmesh_check_nodes(PBVH *pbvh);
void pbvh_bmesh_check_nodes_simple(PBVH *pbvh);
#else
#  define pbvh_bmesh_check_nodes(pbvh)
#  define pbvh_bmesh_check_nodes_simple(pbvh)
#endif

void bke_pbvh_insert_face_finalize(PBVH *pbvh, BMFace *f, const int ni);
void bke_pbvh_insert_face(PBVH *pbvh, struct BMFace *f);
void bke_pbvh_update_vert_boundary(int cd_sculpt_vert,
                                   int cd_faceset_offset,
                                   int cd_vert_node_offset,
                                   int cd_face_node_offset,
                                   int cd_vcol_offset,
                                   int cd_boundary_flags,
                                   BMVert *v,
                                   int bound_symmetry,
                                   const struct CustomData *ldata,
                                   const int totuv);

BLI_INLINE bool pbvh_check_vert_boundary(PBVH *pbvh, struct BMVert *v)
{
  int *flag = (int *)BM_ELEM_CD_GET_VOID_P(v, pbvh->cd_boundary_flag);

  if (*flag & SCULPT_BOUNDARY_NEEDS_UPDATE) {
    bke_pbvh_update_vert_boundary(pbvh->cd_sculpt_vert,
                                  pbvh->cd_faceset_offset,
                                  pbvh->cd_vert_node_offset,
                                  pbvh->cd_face_node_offset,
                                  pbvh->cd_vcol_offset,
                                  pbvh->cd_boundary_flag,
                                  v,
                                  pbvh->boundary_symmetry,
                                  &pbvh->header.bm->ldata,
                                  pbvh->flags & PBVH_IGNORE_UVS ? 0 : pbvh->totuv);
    return true;
  }

  return false;
}

void pbvh_bmesh_check_other_verts(PBVHNode *node);
//#define DEFRAGMENT_MEMORY

/* pbvh_pixels.hh */

void pbvh_node_pixels_free(PBVHNode *node);
void pbvh_pixels_free(PBVH *pbvh);
void pbvh_pixels_free_brush_test(PBVHNode *node);
void pbvh_free_draw_buffers(PBVH *pbvh, PBVHNode *node);

BLI_INLINE bool pbvh_boundary_needs_update_bmesh(PBVH *pbvh, BMVert *v)
{
  int *flags = (int *)BM_ELEM_CD_GET_VOID_P(v, pbvh->cd_boundary_flag);

  return *flags & SCULPT_BOUNDARY_NEEDS_UPDATE;
}

BLI_INLINE void pbvh_boundary_update_bmesh(PBVH *pbvh, BMVert *v)
{
  if (pbvh->cd_boundary_flag == -1) {
    printf("%s: error!\n", __func__);
    return;
  }

  int *flags = (int *)BM_ELEM_CD_GET_VOID_P(v, pbvh->cd_boundary_flag);
  *flags |= SCULPT_BOUNDARY_NEEDS_UPDATE;
}

#ifdef __cplusplus
}
#endif
