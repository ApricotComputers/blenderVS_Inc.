/* SPDX-FileCopyrightText: 2006 by Nicholas Bishop. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 * Implements the Sculpt Mode tools.
 */

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "MEM_guardedalloc.h"

#include "CLG_log.h"

#include "BLI_array_utils.hh"
#include "BLI_bit_span_ops.hh"
#include "BLI_blenlib.h"
#include "BLI_dial_2d.h"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_ghash.h"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_rotation.h"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_task.h"
#include "BLI_task.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_brush_types.h"
#include "DNA_customdata_types.h"
#include "DNA_key_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_ccg.hh"
#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_customdata.hh"
#include "BKE_global.hh"
#include "BKE_image.h"
#include "BKE_key.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"
#include "BKE_modifier.hh"
#include "BKE_multires.hh"
#include "BKE_node_runtime.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_pbvh_api.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"
#include "BKE_subdiv_ccg.hh"
#include "BKE_subsurf.hh"
#include "BLI_math_vector.hh"

#include "NOD_texture.h"

#include "DEG_depsgraph.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "ED_gpencil_legacy.hh"
#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_sculpt.hh"
#include "ED_view3d.hh"

#include "paint_intern.hh"
#include "sculpt_intern.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "bmesh.hh"

#include "editors/sculpt_paint/brushes/types.hh"
#include "mesh_brush_common.hh"

using blender::float3;
using blender::MutableSpan;
using blender::Set;
using blender::Span;
using blender::Vector;

static CLG_LogRef LOG = {"ed.sculpt_paint"};

namespace blender::ed::sculpt_paint {
float sculpt_calc_radius(const ViewContext &vc,
                         const Brush &brush,
                         const Scene &scene,
                         const float3 location)
{
  if (!BKE_brush_use_locked_size(&scene, &brush)) {
    return paint_calc_object_space_radius(vc, location, BKE_brush_size_get(&scene, &brush));
  }
  else {
    return BKE_brush_unprojected_radius_get(&scene, &brush);
  }
}
}  // namespace blender::ed::sculpt_paint

bool ED_sculpt_report_if_shape_key_is_locked(const Object &ob, ReportList *reports)
{
  SculptSession &ss = *ob.sculpt;

  if (ss.shapekey_active && (ss.shapekey_active->flag & KEYBLOCK_LOCKED_SHAPE) != 0) {
    if (reports) {
      BKE_reportf(reports, RPT_ERROR, "The active shape key of %s is locked", ob.id.name + 2);
    }
    return true;
  }

  return false;
}

/* -------------------------------------------------------------------- */
/** \name Sculpt bke::pbvh::Tree Abstraction API
 *
 * This is read-only, for writing use bke::pbvh::Tree vertex iterators. There vd.index matches
 * the indices used here.
 *
 * For multi-resolution, the same vertex in multiple grids is counted multiple times, with
 * different index for each grid.
 * \{ */

void SCULPT_vertex_random_access_ensure(SculptSession &ss)
{
  if (ss.pbvh->type() == blender::bke::pbvh::Type::BMesh) {
    BM_mesh_elem_index_ensure(ss.bm, BM_VERT);
    BM_mesh_elem_table_ensure(ss.bm, BM_VERT);
  }
}

int SCULPT_vertex_count_get(const SculptSession &ss)
{
  switch (ss.pbvh->type()) {
    case blender::bke::pbvh::Type::Mesh:
      return ss.totvert;
    case blender::bke::pbvh::Type::BMesh:
      return BM_mesh_elem_count(ss.bm, BM_VERT);
    case blender::bke::pbvh::Type::Grids:
      return BKE_pbvh_get_grid_num_verts(*ss.pbvh);
  }

  return 0;
}

const float *SCULPT_vertex_co_get(const SculptSession &ss, PBVHVertRef vertex)
{
  switch (ss.pbvh->type()) {
    case blender::bke::pbvh::Type::Mesh: {
      if (ss.shapekey_active || ss.deform_modifiers_active) {
        const Span<float3> positions = BKE_pbvh_get_vert_positions(*ss.pbvh);
        return positions[vertex.i];
      }
      return ss.vert_positions[vertex.i];
    }
    case blender::bke::pbvh::Type::BMesh:
      return ((BMVert *)vertex.i)->co;
    case blender::bke::pbvh::Type::Grids: {
      const CCGKey key = BKE_subdiv_ccg_key_top_level(*ss.subdiv_ccg);
      const int grid_index = vertex.i / key.grid_area;
      const int index_in_grid = vertex.i - grid_index * key.grid_area;
      CCGElem *elem = ss.subdiv_ccg->grids[grid_index];
      return CCG_elem_co(key, CCG_elem_offset(key, elem, index_in_grid));
    }
  }
  return nullptr;
}

const blender::float3 SCULPT_vertex_normal_get(const SculptSession &ss, PBVHVertRef vertex)
{
  switch (ss.pbvh->type()) {
    case blender::bke::pbvh::Type::Mesh: {
      const Span<float3> vert_normals = BKE_pbvh_get_vert_normals(*ss.pbvh);
      return vert_normals[vertex.i];
    }
    case blender::bke::pbvh::Type::BMesh: {
      BMVert *v = (BMVert *)vertex.i;
      return v->no;
    }
    case blender::bke::pbvh::Type::Grids: {
      const CCGKey key = BKE_subdiv_ccg_key_top_level(*ss.subdiv_ccg);
      const int grid_index = vertex.i / key.grid_area;
      const int index_in_grid = vertex.i - grid_index * key.grid_area;
      CCGElem *elem = ss.subdiv_ccg->grids[grid_index];
      return CCG_elem_no(key, CCG_elem_offset(key, elem, index_in_grid));
    }
  }
  BLI_assert_unreachable();
  return {};
}

const float *SCULPT_vertex_co_for_grab_active_get(const SculptSession &ss, PBVHVertRef vertex)
{
  if (ss.pbvh->type() == blender::bke::pbvh::Type::Mesh) {
    /* Always grab active shape key if the sculpt happens on shapekey. */
    if (ss.shapekey_active) {
      const Span<float3> positions = BKE_pbvh_get_vert_positions(*ss.pbvh);
      return positions[vertex.i];
    }

    /* Sculpting on the base mesh. */
    return ss.vert_positions[vertex.i];
  }

  /* Everything else, such as sculpting on multires. */
  return SCULPT_vertex_co_get(ss, vertex);
}

PBVHVertRef SCULPT_active_vertex_get(const SculptSession &ss)
{
  if (ELEM(ss.pbvh->type(),
           blender::bke::pbvh::Type::Mesh,
           blender::bke::pbvh::Type::BMesh,
           blender::bke::pbvh::Type::Grids))
  {
    return ss.active_vertex;
  }

  return BKE_pbvh_make_vref(PBVH_REF_NONE);
}

const float *SCULPT_active_vertex_co_get(const SculptSession &ss)
{
  return SCULPT_vertex_co_get(ss, SCULPT_active_vertex_get(ss));
}

MutableSpan<float3> SCULPT_mesh_deformed_positions_get(SculptSession &ss)
{
  switch (ss.pbvh->type()) {
    case blender::bke::pbvh::Type::Mesh:
      if (ss.shapekey_active || ss.deform_modifiers_active) {
        return BKE_pbvh_get_vert_positions(*ss.pbvh);
      }
      return ss.vert_positions;
    case blender::bke::pbvh::Type::BMesh:
    case blender::bke::pbvh::Type::Grids:
      return {};
  }
  return {};
}

float *SCULPT_brush_deform_target_vertex_co_get(SculptSession &ss,
                                                const int deform_target,
                                                PBVHVertexIter *iter)
{
  switch (deform_target) {
    case BRUSH_DEFORM_TARGET_GEOMETRY:
      return iter->co;
    case BRUSH_DEFORM_TARGET_CLOTH_SIM:
      return ss.cache->cloth_sim->deformation_pos[iter->index];
  }
  return iter->co;
}

ePaintSymmetryFlags SCULPT_mesh_symmetry_xyz_get(const Object &object)
{
  const Mesh *mesh = static_cast<const Mesh *>(object.data);
  return ePaintSymmetryFlags(mesh->symmetry);
}

/* Sculpt Face Sets and Visibility. */

namespace blender::ed::sculpt_paint {

namespace face_set {

int active_face_set_get(const SculptSession &ss)
{
  switch (ss.pbvh->type()) {
    case bke::pbvh::Type::Mesh:
      if (!ss.face_sets) {
        return SCULPT_FACE_SET_NONE;
      }
      return ss.face_sets[ss.active_face_index];
    case bke::pbvh::Type::Grids: {
      if (!ss.face_sets) {
        return SCULPT_FACE_SET_NONE;
      }
      const int face_index = BKE_subdiv_ccg_grid_to_face_index(*ss.subdiv_ccg,
                                                               ss.active_grid_index);
      return ss.face_sets[face_index];
    }
    case bke::pbvh::Type::BMesh:
      return SCULPT_FACE_SET_NONE;
  }
  return SCULPT_FACE_SET_NONE;
}

}  // namespace face_set

namespace hide {

bool vert_visible_get(const SculptSession &ss, PBVHVertRef vertex)
{
  switch (ss.pbvh->type()) {
    case bke::pbvh::Type::Mesh: {
      const Mesh *mesh = BKE_pbvh_get_mesh(*ss.pbvh);
      const bke::AttributeAccessor attributes = mesh->attributes();
      const VArray hide_vert = *attributes.lookup_or_default<bool>(
          ".hide_vert", bke::AttrDomain::Point, false);
      return !hide_vert[vertex.i];
    }
    case bke::pbvh::Type::BMesh:
      return !BM_elem_flag_test((BMVert *)vertex.i, BM_ELEM_HIDDEN);
    case bke::pbvh::Type::Grids: {
      const CCGKey key = BKE_subdiv_ccg_key_top_level(*ss.subdiv_ccg);
      const int grid_index = vertex.i / key.grid_area;
      const int index_in_grid = vertex.i - grid_index * key.grid_area;
      if (!ss.subdiv_ccg->grid_hidden.is_empty()) {
        return !ss.subdiv_ccg->grid_hidden[grid_index][index_in_grid];
      }
    }
  }
  return true;
}

bool vert_any_face_visible_get(const SculptSession &ss, PBVHVertRef vertex)
{
  switch (ss.pbvh->type()) {
    case bke::pbvh::Type::Mesh: {
      if (!ss.hide_poly) {
        return true;
      }
      for (const int face : ss.vert_to_face_map[vertex.i]) {
        if (!ss.hide_poly[face]) {
          return true;
        }
      }
      return false;
    }
    case bke::pbvh::Type::BMesh:
      return true;
    case bke::pbvh::Type::Grids:
      return true;
  }
  return true;
}

bool vert_all_faces_visible_get(const SculptSession &ss, PBVHVertRef vertex)
{
  switch (ss.pbvh->type()) {
    case bke::pbvh::Type::Mesh: {
      if (!ss.hide_poly) {
        return true;
      }
      for (const int face : ss.vert_to_face_map[vertex.i]) {
        if (ss.hide_poly[face]) {
          return false;
        }
      }
      return true;
    }
    case bke::pbvh::Type::BMesh: {
      BMVert *v = (BMVert *)vertex.i;
      BMEdge *e = v->e;

      if (!e) {
        return true;
      }

      do {
        BMLoop *l = e->l;

        if (!l) {
          continue;
        }

        do {
          if (BM_elem_flag_test(l->f, BM_ELEM_HIDDEN)) {
            return false;
          }
        } while ((l = l->radial_next) != e->l);
      } while ((e = BM_DISK_EDGE_NEXT(e, v)) != v->e);

      return true;
    }
    case bke::pbvh::Type::Grids: {
      if (!ss.hide_poly) {
        return true;
      }
      const CCGKey key = BKE_subdiv_ccg_key_top_level(*ss.subdiv_ccg);
      const int grid_index = vertex.i / key.grid_area;
      const int face_index = BKE_subdiv_ccg_grid_to_face_index(*ss.subdiv_ccg, grid_index);
      return !ss.hide_poly[face_index];
    }
  }
  return true;
}

bool vert_all_faces_visible_get(const Span<bool> hide_poly,
                                const GroupedSpan<int> vert_to_face_map,
                                const int vert)
{
  if (hide_poly.is_empty()) {
    return true;
  }

  for (const int face : vert_to_face_map[vert]) {
    if (hide_poly[face]) {
      return false;
    }
  }
  return true;
}

bool vert_all_faces_visible_get(const Span<bool> hide_poly,
                                const SubdivCCG &subdiv_ccg,
                                const SubdivCCGCoord vert)
{
  const int face_index = BKE_subdiv_ccg_grid_to_face_index(subdiv_ccg, vert.grid_index);
  return hide_poly[face_index];
}

bool vert_all_faces_visible_get(BMVert *vert)
{
  BMEdge *edge = vert->e;

  if (!edge) {
    return true;
  }

  do {
    BMLoop *loop = edge->l;

    if (!loop) {
      continue;
    }

    do {
      if (BM_elem_flag_test(loop->f, BM_ELEM_HIDDEN)) {
        return false;
      }
    } while ((loop = loop->radial_next) != edge->l);
  } while ((edge = BM_DISK_EDGE_NEXT(edge, vert)) != vert->e);

  return true;
}

}  // namespace hide

namespace face_set {

int vert_face_set_get(const SculptSession &ss, PBVHVertRef vertex)
{
  switch (ss.pbvh->type()) {
    case bke::pbvh::Type::Mesh: {
      if (!ss.face_sets) {
        return SCULPT_FACE_SET_NONE;
      }
      int face_set = 0;
      for (const int face_index : ss.vert_to_face_map[vertex.i]) {
        if (ss.face_sets[face_index] > face_set) {
          face_set = ss.face_sets[face_index];
        }
      }
      return face_set;
    }
    case bke::pbvh::Type::BMesh:
      return 0;
    case bke::pbvh::Type::Grids: {
      if (!ss.face_sets) {
        return SCULPT_FACE_SET_NONE;
      }
      const CCGKey key = BKE_subdiv_ccg_key_top_level(*ss.subdiv_ccg);
      const int grid_index = vertex.i / key.grid_area;
      const int face_index = BKE_subdiv_ccg_grid_to_face_index(*ss.subdiv_ccg, grid_index);
      return ss.face_sets[face_index];
    }
  }
  return 0;
}

bool vert_has_face_set(const GroupedSpan<int> vert_to_face_map,
                       const int *face_sets,
                       const int vert,
                       const int face_set)
{
  if (!face_sets) {
    return face_set == SCULPT_FACE_SET_NONE;
  }
  const Span<int> faces = vert_to_face_map[vert];
  return std::any_of(
      faces.begin(), faces.end(), [&](const int face) { return face_sets[face] == face_set; });
}

bool vert_has_face_set(const SubdivCCG &subdiv_ccg,
                       const int *face_sets,
                       const int grid,
                       const int face_set)
{
  if (!face_sets) {
    return face_set == SCULPT_FACE_SET_NONE;
  }
  const int face = BKE_subdiv_ccg_grid_to_face_index(subdiv_ccg, grid);
  return face_sets[face] == face_set;
}

bool vert_has_face_set(const int face_set_offset, const BMVert &vert, const int face_set)
{
  if (face_set_offset == -1) {
    return false;
  }
  BMIter iter;
  BMFace *face;
  BM_ITER_ELEM (face, &iter, &const_cast<BMVert &>(vert), BM_FACES_OF_VERT) {
    if (BM_ELEM_CD_GET_INT(face, face_set_offset) == face_set) {
      return true;
    }
  }
  return false;
}

bool vert_has_face_set(const SculptSession &ss, PBVHVertRef vertex, int face_set)
{
  switch (ss.pbvh->type()) {
    case bke::pbvh::Type::Mesh: {
      if (!ss.face_sets) {
        return face_set == SCULPT_FACE_SET_NONE;
      }
      for (const int face_index : ss.vert_to_face_map[vertex.i]) {
        if (ss.face_sets[face_index] == face_set) {
          return true;
        }
      }
      return false;
    }
    case bke::pbvh::Type::BMesh:
      return true;
    case bke::pbvh::Type::Grids: {
      if (!ss.face_sets) {
        return face_set == SCULPT_FACE_SET_NONE;
      }
      const CCGKey key = BKE_subdiv_ccg_key_top_level(*ss.subdiv_ccg);
      const int grid_index = vertex.i / key.grid_area;
      const int face_index = BKE_subdiv_ccg_grid_to_face_index(*ss.subdiv_ccg, grid_index);
      return ss.face_sets[face_index] == face_set;
    }
  }
  return true;
}

bool vert_has_unique_face_set(const SculptSession &ss, PBVHVertRef vertex)
{
  switch (ss.pbvh->type()) {
    case bke::pbvh::Type::Mesh: {
      return vert_has_unique_face_set(ss.vert_to_face_map, ss.face_sets, vertex.i);
    }
    case bke::pbvh::Type::BMesh: {
      BMVert *v = (BMVert *)vertex.i;
      return vert_has_unique_face_set(v);
    }
    case bke::pbvh::Type::Grids: {
      const CCGKey key = BKE_subdiv_ccg_key_top_level(*ss.subdiv_ccg);
      SubdivCCGCoord coord = SubdivCCGCoord::from_index(key, vertex.i);

      return vert_has_unique_face_set(
          ss.vert_to_face_map, ss.corner_verts, ss.faces, ss.face_sets, *ss.subdiv_ccg, coord);
    }
  }
  return false;
}

bool vert_has_unique_face_set(const GroupedSpan<int> vert_to_face_map,
                              const int *face_sets,
                              int vert)
{
  /* TODO: Move this check higher out of this function & make this function take empty span instead
   * of a raw pointer. */
  if (!face_sets) {
    return true;
  }
  int face_set = -1;
  for (const int face_index : vert_to_face_map[vert]) {
    if (face_set == -1) {
      face_set = face_sets[face_index];
    }
    else {
      if (face_sets[face_index] != face_set) {
        return false;
      }
    }
  }
  return true;
}

/**
 * Checks if the face sets of the adjacent faces to the edge between \a v1 and \a v2
 * in the base mesh are equal.
 */
static bool sculpt_check_unique_face_set_for_edge_in_base_mesh(
    const GroupedSpan<int> vert_to_face_map,
    const int *face_sets,
    const Span<int> corner_verts,
    const OffsetIndices<int> faces,
    int v1,
    int v2)
{
  const Span<int> vert_map = vert_to_face_map[v1];
  int p1 = -1, p2 = -1;
  for (int i = 0; i < vert_map.size(); i++) {
    const int face_i = vert_map[i];
    for (const int corner : faces[face_i]) {
      if (corner_verts[corner] == v2) {
        if (p1 == -1) {
          p1 = vert_map[i];
          break;
        }

        if (p2 == -1) {
          p2 = vert_map[i];
          break;
        }
      }
    }
  }

  if (p1 != -1 && p2 != -1) {
    return face_sets[p1] == face_sets[p2];
  }
  return true;
}

bool vert_has_unique_face_set(const GroupedSpan<int> vert_to_face_map,
                              const Span<int> corner_verts,
                              const OffsetIndices<int> faces,
                              const int *face_sets,
                              const SubdivCCG &subdiv_ccg,
                              SubdivCCGCoord coord)
{
  /* TODO: Move this check higher out of this function & make this function take empty span instead
   * of a raw pointer. */
  if (!face_sets) {
    return true;
  }
  int v1, v2;
  const SubdivCCGAdjacencyType adjacency = BKE_subdiv_ccg_coarse_mesh_adjacency_info_get(
      subdiv_ccg, coord, corner_verts, faces, v1, v2);
  switch (adjacency) {
    case SUBDIV_CCG_ADJACENT_VERTEX:
      return vert_has_unique_face_set(vert_to_face_map, face_sets, v1);
    case SUBDIV_CCG_ADJACENT_EDGE:
      return sculpt_check_unique_face_set_for_edge_in_base_mesh(
          vert_to_face_map, face_sets, corner_verts, faces, v1, v2);
    case SUBDIV_CCG_ADJACENT_NONE:
      return true;
  }
  BLI_assert_unreachable();
  return true;
}

bool vert_has_unique_face_set(const BMVert * /* vert */)
{
  /* TODO: Obviously not fully implemented yet. Needs to be implemented for Relax Face Sets brush
   * to work. */
  return true;
}

}  // namespace face_set

/* Sculpt Neighbor Iterators */

#define SCULPT_VERTEX_NEIGHBOR_FIXED_CAPACITY 256

static void sculpt_vertex_neighbor_add(SculptVertexNeighborIter *iter,
                                       PBVHVertRef neighbor,
                                       int neighbor_index)
{
  if (iter->neighbors.contains(neighbor)) {
    return;
  }

  iter->neighbors.append(neighbor);
  iter->neighbor_indices.append(neighbor_index);
}

static void sculpt_vertex_neighbors_get_bmesh(PBVHVertRef vertex, SculptVertexNeighborIter *iter)
{
  BMVert *v = (BMVert *)vertex.i;
  BMIter liter;
  BMLoop *l;
  iter->num_duplicates = 0;
  iter->neighbors.clear();
  iter->neighbor_indices.clear();

  BM_ITER_ELEM (l, &liter, v, BM_LOOPS_OF_VERT) {
    const BMVert *adj_v[2] = {l->prev->v, l->next->v};
    for (int i = 0; i < ARRAY_SIZE(adj_v); i++) {
      const BMVert *v_other = adj_v[i];
      if (v_other != v) {
        sculpt_vertex_neighbor_add(
            iter, BKE_pbvh_make_vref(intptr_t(v_other)), BM_elem_index_get(v_other));
      }
    }
  }
}

Span<BMVert *> vert_neighbors_get_bmesh(BMVert &vert, Vector<BMVert *, 64> &neighbors)
{
  BMIter liter;
  BMLoop *l;
  BM_ITER_ELEM (l, &liter, &vert, BM_LOOPS_OF_VERT) {
    for (BMVert *other_vert : {l->prev->v, l->next->v}) {
      if (other_vert != &vert) {
        neighbors.append(other_vert);
      }
    }
  }
  return neighbors;
}

Span<BMVert *> vert_neighbors_get_interior_bmesh(BMVert &vert, Vector<BMVert *, 64> &neighbors)
{
  BMIter liter;
  BMLoop *l;
  BM_ITER_ELEM (l, &liter, &vert, BM_LOOPS_OF_VERT) {
    for (BMVert *other_vert : {l->prev->v, l->next->v}) {
      if (other_vert != &vert) {
        neighbors.append(other_vert);
      }
    }
  }

  if (BM_vert_is_boundary(&vert)) {
    if (neighbors.size() == 2) {
      /* Do not include neighbors of corner vertices. */
      neighbors.clear();
    }
    else {
      /* Only include other boundary vertices as neighbors of boundary vertices. */
      neighbors.remove_if([&](const BMVert *vert) { return !BM_vert_is_boundary(vert); });
    }
  }

  return neighbors;
}

static void sculpt_vertex_neighbors_get_faces(const SculptSession &ss,
                                              PBVHVertRef vertex,
                                              SculptVertexNeighborIter *iter)
{
  iter->num_duplicates = 0;
  iter->neighbors.clear();
  iter->neighbor_indices.clear();

  for (const int face_i : ss.vert_to_face_map[vertex.i]) {
    if (ss.hide_poly && ss.hide_poly[face_i]) {
      /* Skip connectivity from hidden faces. */
      continue;
    }
    const IndexRange face = ss.faces[face_i];
    const int2 f_adj_v = bke::mesh::face_find_adjacent_verts(face, ss.corner_verts, vertex.i);
    for (int j = 0; j < 2; j++) {
      if (f_adj_v[j] != vertex.i) {
        sculpt_vertex_neighbor_add(iter, BKE_pbvh_make_vref(f_adj_v[j]), f_adj_v[j]);
      }
    }
  }

  if (ss.fake_neighbors.use_fake_neighbors) {
    BLI_assert(ss.fake_neighbors.fake_neighbor_index != nullptr);
    if (ss.fake_neighbors.fake_neighbor_index[vertex.i] != FAKE_NEIGHBOR_NONE) {
      sculpt_vertex_neighbor_add(
          iter,
          BKE_pbvh_make_vref(ss.fake_neighbors.fake_neighbor_index[vertex.i]),
          ss.fake_neighbors.fake_neighbor_index[vertex.i]);
    }
  }
}

Span<int> vert_neighbors_get_mesh(const int vert,
                                  const OffsetIndices<int> faces,
                                  const Span<int> corner_verts,
                                  const GroupedSpan<int> vert_to_face,
                                  const Span<bool> hide_poly,
                                  Vector<int> &r_neighbors)
{
  r_neighbors.clear();

  for (const int face : vert_to_face[vert]) {
    if (!hide_poly.is_empty() && hide_poly[face]) {
      continue;
    }
    const int2 verts = bke::mesh::face_find_adjacent_verts(faces[face], corner_verts, vert);
    r_neighbors.append_non_duplicates(verts[0]);
    r_neighbors.append_non_duplicates(verts[1]);
  }

  return r_neighbors.as_span();
}

static void sculpt_vertex_neighbors_get_grids(const SculptSession &ss,
                                              const PBVHVertRef vertex,
                                              const bool include_duplicates,
                                              SculptVertexNeighborIter *iter)
{
  /* TODO: optimize this. We could fill #SculptVertexNeighborIter directly,
   * maybe provide coordinate and mask pointers directly rather than converting
   * back and forth between #CCGElem and global index. */
  const CCGKey key = BKE_subdiv_ccg_key_top_level(*ss.subdiv_ccg);
  SubdivCCGCoord coord = SubdivCCGCoord::from_index(key, vertex.i);

  SubdivCCGNeighbors neighbors;
  BKE_subdiv_ccg_neighbor_coords_get(*ss.subdiv_ccg, coord, include_duplicates, neighbors);

  iter->num_duplicates = neighbors.num_duplicates;
  iter->neighbors.clear();
  iter->neighbor_indices.clear();

  for (const int i : neighbors.coords.index_range()) {
    int v = neighbors.coords[i].grid_index * key.grid_area +
            neighbors.coords[i].y * key.grid_size + neighbors.coords[i].x;

    sculpt_vertex_neighbor_add(iter, BKE_pbvh_make_vref(v), v);
  }

  if (ss.fake_neighbors.use_fake_neighbors) {
    BLI_assert(ss.fake_neighbors.fake_neighbor_index != nullptr);
    if (ss.fake_neighbors.fake_neighbor_index[vertex.i] != FAKE_NEIGHBOR_NONE) {
      int v = ss.fake_neighbors.fake_neighbor_index[vertex.i];
      sculpt_vertex_neighbor_add(iter, BKE_pbvh_make_vref(v), v);
    }
  }
}

}  // namespace blender::ed::sculpt_paint

void SCULPT_vertex_neighbors_get(const SculptSession &ss,
                                 const PBVHVertRef vertex,
                                 const bool include_duplicates,
                                 SculptVertexNeighborIter *iter)
{
  using namespace blender::ed::sculpt_paint;
  switch (ss.pbvh->type()) {
    case blender::bke::pbvh::Type::Mesh:
      sculpt_vertex_neighbors_get_faces(ss, vertex, iter);
      return;
    case blender::bke::pbvh::Type::BMesh:
      sculpt_vertex_neighbors_get_bmesh(vertex, iter);
      return;
    case blender::bke::pbvh::Type::Grids:
      sculpt_vertex_neighbors_get_grids(ss, vertex, include_duplicates, iter);
      return;
  }
}

static bool sculpt_check_boundary_vertex_in_base_mesh(const SculptSession &ss, const int index)
{
  return ss.vertex_info.boundary[index];
}

namespace blender::ed::sculpt_paint {

namespace boundary {

bool vert_is_boundary(const SculptSession &ss, const PBVHVertRef vertex)
{
  switch (ss.pbvh->type()) {
    case bke::pbvh::Type::Mesh: {
      if (!hide::vert_all_faces_visible_get(ss, vertex)) {
        return true;
      }
      return sculpt_check_boundary_vertex_in_base_mesh(ss, vertex.i);
    }
    case bke::pbvh::Type::BMesh: {
      BMVert *v = (BMVert *)vertex.i;
      return BM_vert_is_boundary(v);
    }
    case bke::pbvh::Type::Grids: {
      const CCGKey key = BKE_subdiv_ccg_key_top_level(*ss.subdiv_ccg);
      SubdivCCGCoord coord = SubdivCCGCoord::from_index(key, vertex.i);
      int v1, v2;
      const SubdivCCGAdjacencyType adjacency = BKE_subdiv_ccg_coarse_mesh_adjacency_info_get(
          *ss.subdiv_ccg, coord, ss.corner_verts, ss.faces, v1, v2);
      switch (adjacency) {
        case SUBDIV_CCG_ADJACENT_VERTEX:
          return sculpt_check_boundary_vertex_in_base_mesh(ss, v1);
        case SUBDIV_CCG_ADJACENT_EDGE:
          return sculpt_check_boundary_vertex_in_base_mesh(ss, v1) &&
                 sculpt_check_boundary_vertex_in_base_mesh(ss, v2);
        case SUBDIV_CCG_ADJACENT_NONE:
          return false;
      }
    }
  }

  return false;
}

bool vert_is_boundary(const Span<bool> hide_poly,
                      const GroupedSpan<int> vert_to_face_map,
                      const BitSpan boundary,
                      const int vert)
{
  if (!hide::vert_all_faces_visible_get(hide_poly, vert_to_face_map, vert)) {
    return true;
  }
  return boundary[vert].test();
}

bool vert_is_boundary(const SubdivCCG &subdiv_ccg,
                      const Span<bool> /*hide_poly*/,
                      const Span<int> corner_verts,
                      const OffsetIndices<int> faces,
                      const BitSpan boundary,
                      const SubdivCCGCoord vert)
{
  /* TODO: Unlike the base mesh implementation this method does NOT take into account face
   * visibility. Either this should be noted as a intentional limitation or fixed.*/
  int v1, v2;
  const SubdivCCGAdjacencyType adjacency = BKE_subdiv_ccg_coarse_mesh_adjacency_info_get(
      subdiv_ccg, vert, corner_verts, faces, v1, v2);
  switch (adjacency) {
    case SUBDIV_CCG_ADJACENT_VERTEX:
      return boundary[v1].test();
    case SUBDIV_CCG_ADJACENT_EDGE:
      return boundary[v1].test() && boundary[v2].test();
    case SUBDIV_CCG_ADJACENT_NONE:
      return false;
  }
  BLI_assert_unreachable();
  return false;
}

bool vert_is_boundary(BMVert *vert)
{
  /* TODO: Unlike the base mesh implementation this method does NOT take into account face
   * visibility. Either this should be noted as a intentional limitation or fixed.*/
  return BM_vert_is_boundary(vert);
}

}  // namespace boundary

}  // namespace blender::ed::sculpt_paint

/* Utilities */

bool SCULPT_stroke_is_main_symmetry_pass(const blender::ed::sculpt_paint::StrokeCache &cache)
{
  return cache.mirror_symmetry_pass == 0 && cache.radial_symmetry_pass == 0 &&
         cache.tile_pass == 0;
}

bool SCULPT_stroke_is_first_brush_step(const blender::ed::sculpt_paint::StrokeCache &cache)
{
  return cache.first_time && cache.mirror_symmetry_pass == 0 && cache.radial_symmetry_pass == 0 &&
         cache.tile_pass == 0;
}

bool SCULPT_stroke_is_first_brush_step_of_symmetry_pass(
    const blender::ed::sculpt_paint::StrokeCache &cache)
{
  return cache.first_time;
}

bool SCULPT_check_vertex_pivot_symmetry(const float vco[3], const float pco[3], const char symm)
{
  bool is_in_symmetry_area = true;
  for (int i = 0; i < 3; i++) {
    char symm_it = 1 << i;
    if (symm & symm_it) {
      if (pco[i] == 0.0f) {
        if (vco[i] > 0.0f) {
          is_in_symmetry_area = false;
        }
      }
      if (vco[i] * pco[i] < 0.0f) {
        is_in_symmetry_area = false;
      }
    }
  }
  return is_in_symmetry_area;
}

struct NearestVertexData {
  PBVHVertRef nearest_vertex;
  float nearest_vertex_distance_sq;
};

namespace blender::ed::sculpt_paint {

std::optional<int> nearest_vert_calc_mesh(const bke::pbvh::Tree &pbvh,
                                          const Span<float3> vert_positions,
                                          const Span<bool> hide_vert,
                                          const float3 &location,
                                          const float max_distance,
                                          const bool use_original)
{
  const float max_distance_sq = max_distance * max_distance;
  Vector<bke::pbvh::Node *> nodes = bke::pbvh::search_gather(
      const_cast<bke::pbvh::Tree &>(pbvh), [&](bke::pbvh::Node &node) {
        return node_in_sphere(node, location, max_distance_sq, use_original);
      });
  if (nodes.is_empty()) {
    return std::nullopt;
  }

  struct NearestData {
    int vert = -1;
    float distance_sq = std::numeric_limits<float>::max();
  };

  const NearestData nearest = threading::parallel_reduce(
      nodes.index_range(),
      1,
      NearestData(),
      [&](const IndexRange range, NearestData nearest) {
        for (const int i : range) {
          for (const int vert : bke::pbvh::node_unique_verts(*nodes[i])) {
            if (!hide_vert.is_empty() && hide_vert[vert]) {
              continue;
            }
            const float distance_sq = math::distance_squared(vert_positions[vert], location);
            if (distance_sq < nearest.distance_sq) {
              nearest = {vert, distance_sq};
            }
          }
        }
        return nearest;
      },
      [](const NearestData a, const NearestData b) {
        return a.distance_sq < b.distance_sq ? a : b;
      });
  return nearest.vert;
}

std::optional<SubdivCCGCoord> nearest_vert_calc_grids(const bke::pbvh::Tree &pbvh,
                                                      const SubdivCCG &subdiv_ccg,
                                                      const float3 &location,
                                                      const float max_distance,
                                                      const bool use_original)
{
  const float max_distance_sq = max_distance * max_distance;
  Vector<bke::pbvh::Node *> nodes = bke::pbvh::search_gather(
      const_cast<bke::pbvh::Tree &>(pbvh), [&](bke::pbvh::Node &node) {
        return node_in_sphere(node, location, max_distance_sq, use_original);
      });
  if (nodes.is_empty()) {
    return std::nullopt;
  }

  struct NearestData {
    SubdivCCGCoord coord = {};
    float distance_sq = std::numeric_limits<float>::max();
  };

  const BitGroupVector<> grid_hidden = subdiv_ccg.grid_hidden;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const Span<CCGElem *> elems = subdiv_ccg.grids;

  const NearestData nearest = threading::parallel_reduce(
      nodes.index_range(),
      1,
      NearestData(),
      [&](const IndexRange range, NearestData nearest) {
        for (const int i : range) {
          for (const int grid : bke::pbvh::node_grid_indices(*nodes[i])) {
            CCGElem *elem = elems[grid];
            BKE_subdiv_ccg_foreach_visible_grid_vert(key, grid_hidden, grid, [&](const int i) {
              const float distance_sq = math::distance_squared(CCG_elem_offset_co(key, elem, i),
                                                               location);
              if (distance_sq < nearest.distance_sq) {
                SubdivCCGCoord coord{};
                coord.grid_index = grid;
                coord.x = i % key.grid_size;
                coord.y = i / key.grid_size;
                nearest = {coord, distance_sq};
              }
            });
          }
        }
        return nearest;
      },
      [](const NearestData a, const NearestData b) {
        return a.distance_sq < b.distance_sq ? a : b;
      });
  return nearest.coord;
}

std::optional<BMVert *> nearest_vert_calc_bmesh(const bke::pbvh::Tree &pbvh,
                                                const float3 &location,
                                                const float max_distance,
                                                const bool use_original)
{
  const float max_distance_sq = max_distance * max_distance;
  Vector<bke::pbvh::Node *> nodes = bke::pbvh::search_gather(
      const_cast<bke::pbvh::Tree &>(pbvh), [&](bke::pbvh::Node &node) {
        return node_in_sphere(node, location, max_distance_sq, use_original);
      });
  if (nodes.is_empty()) {
    return std::nullopt;
  }

  struct NearestData {
    BMVert *vert = nullptr;
    float distance_sq = std::numeric_limits<float>::max();
  };

  const NearestData nearest = threading::parallel_reduce(
      nodes.index_range(),
      1,
      NearestData(),
      [&](const IndexRange range, NearestData nearest) {
        for (const int i : range) {
          for (BMVert *vert : BKE_pbvh_bmesh_node_unique_verts(nodes[i])) {
            if (BM_elem_flag_test(vert, BM_ELEM_HIDDEN)) {
              continue;
            }
            const float distance_sq = math::distance_squared(float3(vert->co), location);
            if (distance_sq < nearest.distance_sq) {
              nearest = {vert, distance_sq};
            }
          }
        }
        return nearest;
      },
      [](const NearestData a, const NearestData b) {
        return a.distance_sq < b.distance_sq ? a : b;
      });
  return nearest.vert;
}

PBVHVertRef nearest_vert_calc(const Object &object,
                              const float3 &location,
                              const float max_distance,
                              const bool use_original)
{
  const SculptSession &ss = *object.sculpt;
  switch (ss.pbvh->type()) {
    case bke::pbvh::Type::Mesh: {
      const Mesh &mesh = *static_cast<const Mesh *>(object.data);
      const Span<float3> vert_positions = BKE_pbvh_get_vert_positions(*ss.pbvh);
      const bke::AttributeAccessor attributes = mesh.attributes();
      VArraySpan<bool> hide_vert = *attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point);
      const std::optional<int> nearest = nearest_vert_calc_mesh(
          *ss.pbvh, vert_positions, hide_vert, location, max_distance, use_original);
      return nearest ? PBVHVertRef{*nearest} : PBVHVertRef{PBVH_REF_NONE};
    }
    case bke::pbvh::Type::Grids: {
      const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
      const std::optional<SubdivCCGCoord> nearest = nearest_vert_calc_grids(
          *ss.pbvh, subdiv_ccg, location, max_distance, use_original);
      return nearest ? PBVHVertRef{key.grid_area * nearest->grid_index +
                                   CCG_grid_xy_to_index(key.grid_size, nearest->x, nearest->y)} :
                       PBVHVertRef{PBVH_REF_NONE};
    }
    case bke::pbvh::Type::BMesh: {
      const std::optional<BMVert *> nearest = nearest_vert_calc_bmesh(
          *ss.pbvh, location, max_distance, use_original);
      return nearest ? PBVHVertRef{intptr_t(*nearest)} : PBVHVertRef{PBVH_REF_NONE};
    }
  }
  BLI_assert_unreachable();
  return BKE_pbvh_make_vref(PBVH_REF_NONE);
}

}  // namespace blender::ed::sculpt_paint

bool SCULPT_is_symmetry_iteration_valid(char i, char symm)
{
  return i == 0 || (symm & i && (symm != 5 || i != 3) && (symm != 6 || !ELEM(i, 3, 5)));
}

bool SCULPT_is_vertex_inside_brush_radius_symm(const float vertex[3],
                                               const float br_co[3],
                                               float radius,
                                               char symm)
{
  for (char i = 0; i <= symm; ++i) {
    if (!SCULPT_is_symmetry_iteration_valid(i, symm)) {
      continue;
    }
    float3 location = blender::ed::sculpt_paint::symmetry_flip(br_co, ePaintSymmetryFlags(i));
    if (len_squared_v3v3(location, vertex) < radius * radius) {
      return true;
    }
  }
  return false;
}

void SCULPT_tag_update_overlays(bContext *C)
{
  ARegion *region = CTX_wm_region(C);
  ED_region_tag_redraw(region);

  Object &ob = *CTX_data_active_object(C);
  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, &ob);

  DEG_id_tag_update(&ob.id, ID_RECALC_SHADING);

  RegionView3D *rv3d = CTX_wm_region_view3d(C);
  if (!BKE_sculptsession_use_pbvh_draw(&ob, rv3d)) {
    DEG_id_tag_update(&ob.id, ID_RECALC_GEOMETRY);
  }
}

/** \} */

namespace blender::ed::sculpt_paint {

/* -------------------------------------------------------------------- */
/** \name Tool Capabilities
 *
 * Avoid duplicate checks, internal logic only,
 * share logic with #rna_def_sculpt_capabilities where possible.
 * \{ */

static bool sculpt_tool_needs_original(const char sculpt_tool)
{
  return ELEM(sculpt_tool,
              SCULPT_TOOL_GRAB,
              SCULPT_TOOL_ROTATE,
              SCULPT_TOOL_THUMB,
              SCULPT_TOOL_LAYER,
              SCULPT_TOOL_DRAW_SHARP,
              SCULPT_TOOL_ELASTIC_DEFORM,
              SCULPT_TOOL_SMOOTH,
              SCULPT_TOOL_BOUNDARY,
              SCULPT_TOOL_POSE);
}

static bool sculpt_tool_is_proxy_used(const char sculpt_tool)
{
  return ELEM(sculpt_tool,
              SCULPT_TOOL_SMOOTH,
              SCULPT_TOOL_LAYER,
              SCULPT_TOOL_POSE,
              SCULPT_TOOL_DISPLACEMENT_SMEAR,
              SCULPT_TOOL_BOUNDARY,
              SCULPT_TOOL_CLOTH,
              SCULPT_TOOL_PAINT,
              SCULPT_TOOL_SMEAR,
              SCULPT_TOOL_DRAW_FACE_SETS);
}

static bool sculpt_brush_use_topology_rake(const SculptSession &ss, const Brush &brush)
{
  return SCULPT_TOOL_HAS_TOPOLOGY_RAKE(brush.sculpt_tool) && (brush.topology_rake_factor > 0.0f) &&
         (ss.bm != nullptr);
}

/**
 * Test whether the #StrokeCache.sculpt_normal needs update in #do_brush_action
 */
static int sculpt_brush_needs_normal(const SculptSession &ss, const Sculpt &sd, const Brush &brush)
{
  using namespace blender::ed::sculpt_paint;
  const MTex *mask_tex = BKE_brush_mask_texture_get(&brush, OB_MODE_SCULPT);
  return ((SCULPT_TOOL_HAS_NORMAL_WEIGHT(brush.sculpt_tool) && (ss.cache->normal_weight > 0.0f)) ||
          auto_mask::needs_normal(ss, sd, &brush) ||
          ELEM(brush.sculpt_tool,
               SCULPT_TOOL_BLOB,
               SCULPT_TOOL_CREASE,
               SCULPT_TOOL_DRAW,
               SCULPT_TOOL_DRAW_SHARP,
               SCULPT_TOOL_CLOTH,
               SCULPT_TOOL_LAYER,
               SCULPT_TOOL_NUDGE,
               SCULPT_TOOL_ROTATE,
               SCULPT_TOOL_ELASTIC_DEFORM,
               SCULPT_TOOL_THUMB) ||

          (mask_tex->brush_map_mode == MTEX_MAP_MODE_AREA)) ||
         sculpt_brush_use_topology_rake(ss, brush) ||
         BKE_brush_has_cube_tip(&brush, PaintMode::Sculpt);
}

static bool sculpt_brush_needs_rake_rotation(const Brush &brush)
{
  return SCULPT_TOOL_HAS_RAKE(brush.sculpt_tool) && (brush.rake_factor != 0.0f);
}

}  // namespace blender::ed::sculpt_paint

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sculpt Init/Update
 * \{ */

enum StrokeFlags {
  CLIP_X = 1,
  CLIP_Y = 2,
  CLIP_Z = 4,
};

static void orig_vert_data_unode_init(SculptOrigVertData &data,
                                      const blender::ed::sculpt_paint::undo::Node &unode)
{
  data = {};
  data.coords = unode.position.data();
  data.normals = unode.normal.data();
  data.vmasks = unode.mask.data();
  data.colors = unode.col.data();
}

SculptOrigVertData SCULPT_orig_vert_data_init(const Object &ob,
                                              const blender::bke::pbvh::Node &node,
                                              const blender::ed::sculpt_paint::undo::Type type)
{
  using namespace blender::ed::sculpt_paint;
  SculptOrigVertData data;
  data.undo_type = type;
  const SculptSession &ss = *ob.sculpt;
  if (ss.bm) {
    data.bm_log = ss.bm_log;
  }
  else if (const undo::Node *unode = undo::get_node(&node, type)) {
    orig_vert_data_unode_init(data, *unode);
    data.undo_type = type;
  }
  else {
    data = {};
  }
  return data;
}

void SCULPT_orig_vert_data_update(SculptOrigVertData &orig_data, const PBVHVertexIter &iter)
{
  using namespace blender::ed::sculpt_paint;
  if (orig_data.undo_type == undo::Type::Position) {
    if (orig_data.bm_log) {
      BM_log_original_vert_data(orig_data.bm_log, iter.bm_vert, &orig_data.co, &orig_data.no);
    }
    else {
      orig_data.co = orig_data.coords[iter.i];
      orig_data.no = orig_data.normals[iter.i];
    }
  }
  else if (orig_data.undo_type == undo::Type::Color) {
    orig_data.col = orig_data.colors[iter.i];
  }
  else if (orig_data.undo_type == undo::Type::Mask) {
    if (orig_data.bm_log) {
      orig_data.mask = BM_log_original_mask(orig_data.bm_log, iter.bm_vert);
    }
    else {
      orig_data.mask = orig_data.vmasks[iter.i];
    }
  }
}

void SCULPT_orig_vert_data_update(SculptOrigVertData &orig_data, const BMVert &vert)
{
  using namespace blender::ed::sculpt_paint;
  if (orig_data.undo_type == undo::Type::Position) {
    BM_log_original_vert_data(
        orig_data.bm_log, &const_cast<BMVert &>(vert), &orig_data.co, &orig_data.no);
  }
  else if (orig_data.undo_type == undo::Type::Mask) {
    orig_data.mask = BM_log_original_mask(orig_data.bm_log, &const_cast<BMVert &>(vert));
  }
}

void SCULPT_orig_vert_data_update(SculptOrigVertData &orig_data, const int i)
{
  using namespace blender::ed::sculpt_paint;
  if (orig_data.undo_type == undo::Type::Position) {
    orig_data.co = orig_data.coords[i];
    orig_data.no = orig_data.normals[i];
  }
  else if (orig_data.undo_type == undo::Type::Color) {
    orig_data.col = orig_data.colors[i];
  }
  else if (orig_data.undo_type == undo::Type::Mask) {
    orig_data.mask = orig_data.vmasks[i];
  }
}

namespace blender::ed::sculpt_paint {

static void sculpt_rake_data_update(SculptRakeData *srd, const float co[3])
{
  float rake_dist = len_v3v3(srd->follow_co, co);
  if (rake_dist > srd->follow_dist) {
    interp_v3_v3v3(srd->follow_co, srd->follow_co, co, rake_dist - srd->follow_dist);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sculpt Dynamic Topology
 * \{ */

namespace dyntopo {

bool stroke_is_dyntopo(const SculptSession &ss, const Brush &brush)
{
  return ((ss.pbvh->type() == bke::pbvh::Type::BMesh) &&

          (!ss.cache || (!ss.cache->alt_smooth)) &&

          /* Requires mesh restore, which doesn't work with
           * dynamic-topology. */
          !(brush.flag & BRUSH_ANCHORED) && !(brush.flag & BRUSH_DRAG_DOT) &&

          SCULPT_TOOL_HAS_DYNTOPO(brush.sculpt_tool));
}

}  // namespace dyntopo

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sculpt Paint Mesh
 * \{ */

namespace undo {

static void restore_mask_from_undo_step(Object &object)
{
  SculptSession &ss = *object.sculpt;
  Vector<bke::pbvh::Node *> nodes = bke::pbvh::search_gather(*ss.pbvh, {});

  switch (ss.pbvh->type()) {
    case bke::pbvh::Type::Mesh: {
      Mesh &mesh = *static_cast<Mesh *>(object.data);
      bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
      bke::SpanAttributeWriter<float> mask = attributes.lookup_or_add_for_write_span<float>(
          ".sculpt_mask", bke::AttrDomain::Point);
      threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
        for (bke::pbvh::Node *node : nodes.as_span().slice(range)) {
          if (const undo::Node *unode = undo::get_node(node, undo::Type::Mask)) {
            const Span<int> verts = bke::pbvh::node_unique_verts(*node);
            array_utils::scatter(unode->mask.as_span(), verts, mask.span);
            BKE_pbvh_node_mark_update_mask(node);
          }
        }
      });
      mask.finish();
      break;
    }
    case bke::pbvh::Type::BMesh: {
      const int offset = CustomData_get_offset_named(&ss.bm->vdata, CD_PROP_FLOAT, ".sculpt_mask");
      if (offset != -1) {
        for (bke::pbvh::Node *node : nodes) {
          if (undo::get_node(node, undo::Type::Mask)) {
            for (BMVert *vert : BKE_pbvh_bmesh_node_unique_verts(node)) {
              const float orig_mask = BM_log_original_mask(ss.bm_log, vert);
              BM_ELEM_CD_SET_FLOAT(vert, offset, orig_mask);
            }
            BKE_pbvh_node_mark_update_mask(node);
          }
        }
      }
      break;
    }
    case bke::pbvh::Type::Grids: {
      SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      const BitGroupVector<> grid_hidden = subdiv_ccg.grid_hidden;
      const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
      const Span<CCGElem *> grids = subdiv_ccg.grids;
      threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
        for (bke::pbvh::Node *node : nodes.as_span().slice(range)) {
          if (const undo::Node *unode = undo::get_node(node, undo::Type::Mask)) {
            int index = 0;
            for (const int grid : unode->grids) {
              CCGElem *elem = grids[grid];
              for (const int i : IndexRange(key.grid_area)) {
                if (grid_hidden.is_empty() || !grid_hidden[grid][i]) {
                  CCG_elem_offset_mask(key, elem, i) = unode->mask[index];
                }
                index++;
              }
            }
            BKE_pbvh_node_mark_update_mask(node);
          }
        }
      });
      break;
    }
  }
}

static void restore_color_from_undo_step(Object &object)
{
  SculptSession &ss = *object.sculpt;
  Vector<bke::pbvh::Node *> nodes = bke::pbvh::search_gather(*ss.pbvh, {});

  BLI_assert(ss.pbvh->type() == bke::pbvh::Type::Mesh);
  Mesh &mesh = *static_cast<Mesh *>(object.data);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const GroupedSpan<int> vert_to_face_map = ss.vert_to_face_map;
  bke::GSpanAttributeWriter color_attribute = color::active_color_attribute_for_write(mesh);
  threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
    for (bke::pbvh::Node *node : nodes.as_span().slice(range)) {
      if (const undo::Node *unode = undo::get_node(node, undo::Type::Color)) {
        const Span<int> verts = bke::pbvh::node_unique_verts(*node);
        for (const int i : verts.index_range()) {
          color::color_vert_set(faces,
                                corner_verts,
                                vert_to_face_map,
                                color_attribute.domain,
                                verts[i],
                                unode->col[i],
                                color_attribute.span);
        }
      }
    }
  });
  color_attribute.finish();
}

static void restore_face_set_from_undo_step(Object &object)
{
  SculptSession &ss = *object.sculpt;
  Vector<bke::pbvh::Node *> nodes = bke::pbvh::search_gather(*ss.pbvh, {});

  switch (ss.pbvh->type()) {
    case bke::pbvh::Type::Mesh:
    case bke::pbvh::Type::Grids: {
      bke::SpanAttributeWriter<int> attribute = face_set::ensure_face_sets_mesh(object);
      threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
        for (bke::pbvh::Node *node : nodes.as_span().slice(range)) {
          if (const undo::Node *unode = undo::get_node(node, undo::Type::FaceSet)) {
            const Span<int> faces = unode->face_indices;
            const Span<int> face_sets = unode->face_sets;
            blender::array_utils::scatter(face_sets, faces, attribute.span);
            BKE_pbvh_node_mark_update_face_sets(node);
          }
        }
      });
      attribute.finish();
      break;
    }
    case bke::pbvh::Type::BMesh:
      break;
  }
}

void restore_position_from_undo_step(Object &object)
{
  SculptSession &ss = *object.sculpt;
  Vector<bke::pbvh::Node *> nodes = bke::pbvh::search_gather(*ss.pbvh, {});

  switch (ss.pbvh->type()) {
    case bke::pbvh::Type::Mesh: {
      Mesh &mesh = *static_cast<Mesh *>(object.data);
      MutableSpan positions_eval = BKE_pbvh_get_vert_positions(*ss.pbvh);
      MutableSpan positions_orig = mesh.vert_positions_for_write();

      struct LocalData {
        Vector<float3> translations;
      };

      const bool need_translations = !ss.deform_imats.is_empty() ||
                                     BKE_keyblock_from_object(&object);

      threading::EnumerableThreadSpecific<LocalData> all_tls;
      threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
        LocalData &tls = all_tls.local();
        for (bke::pbvh::Node *node : nodes.as_span().slice(range)) {
          if (const undo::Node *unode = undo::get_node(node, undo::Type::Position)) {
            const Span<int> verts = bke::pbvh::node_unique_verts(*node);
            const Span<float3> undo_positions = unode->position.as_span().take_front(verts.size());
            if (need_translations) {
              tls.translations.resize(verts.size());
              translations_from_new_positions(
                  undo_positions, verts, positions_eval, tls.translations);
            }

            array_utils::scatter(undo_positions, verts, positions_eval);

            if (positions_eval.data() != positions_orig.data()) {
              /* When the evaluated positions and original mesh positions don't point to the same
               * array, they must both be updated. */
              if (ss.deform_imats.is_empty()) {
                array_utils::scatter(undo_positions, verts, positions_orig);
              }
              else {
                /* Because brush deformation is calculated for the evaluated deformed positions,
                 * the translations have to be transformed to the original space. */
                apply_crazyspace_to_translations(ss.deform_imats, verts, tls.translations);
                apply_translations(tls.translations, verts, positions_orig);
              }
            }

            if (BKE_keyblock_from_object(&object)) {
              /* Update dependent shape keys back to their original */
              apply_translations_to_shape_keys(object, verts, tls.translations, positions_orig);
            }

            BKE_pbvh_node_mark_positions_update(node);
          }
        }
      });
      break;
    }
    case bke::pbvh::Type::BMesh: {
      if (!undo::get_bmesh_log_entry()) {
        return;
      }
      threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
        for (bke::pbvh::Node *node : nodes.as_span().slice(range)) {
          for (BMVert *vert : BKE_pbvh_bmesh_node_unique_verts(node)) {
            if (const float *orig_co = BM_log_find_original_vert_co(ss.bm_log, vert)) {
              copy_v3_v3(vert->co, orig_co);
            }
          }
          BKE_pbvh_node_mark_positions_update(node);
        }
      });
      break;
    }
    case bke::pbvh::Type::Grids: {
      SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      const BitGroupVector<> grid_hidden = subdiv_ccg.grid_hidden;
      const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
      const Span<CCGElem *> grids = subdiv_ccg.grids;
      threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
        for (bke::pbvh::Node *node : nodes.as_span().slice(range)) {
          if (const undo::Node *unode = undo::get_node(node, undo::Type::Position)) {
            int index = 0;
            for (const int grid : unode->grids) {
              CCGElem *elem = grids[grid];
              for (const int i : IndexRange(key.grid_area)) {
                if (grid_hidden.is_empty() || !grid_hidden[grid][i]) {
                  CCG_elem_offset_co(key, elem, i) = unode->position[index];
                }
                index++;
              }
            }
            BKE_pbvh_node_mark_positions_update(node);
          }
        }
      });
      break;
    }
  }

  /* Update normals for potentially-changed positions. Theoretically this may be unnecessary if
   * the tool restoring to the initial state doesn't use the normals, but we have no easy way to
   * know that from here. */
  bke::pbvh::update_normals(*ss.pbvh, ss.subdiv_ccg);
}

static void restore_from_undo_step(const Sculpt &sd, Object &object)
{
  SculptSession &ss = *object.sculpt;
  const Brush *brush = BKE_paint_brush_for_read(&sd.paint);

  switch (brush->sculpt_tool) {
    case SCULPT_TOOL_MASK:
      restore_mask_from_undo_step(object);
      break;
    case SCULPT_TOOL_PAINT:
    case SCULPT_TOOL_SMEAR:
      restore_color_from_undo_step(object);
      break;
    case SCULPT_TOOL_DRAW_FACE_SETS:
      if (ss.cache->alt_smooth) {
        restore_position_from_undo_step(object);
      }
      else {
        restore_face_set_from_undo_step(object);
      }
      break;
    default:
      restore_position_from_undo_step(object);
      break;
  }
  /* Disable multi-threading when dynamic-topology is enabled. Otherwise,
   * new entries might be inserted by #undo::push_node() into the #GHash
   * used internally by #BM_log_original_vert_co() by a different thread. See #33787. */
}

}  // namespace undo

}  // namespace blender::ed::sculpt_paint

/*** BVH Tree ***/

static void sculpt_extend_redraw_rect_previous(Object &ob, rcti &rect)
{
  /* Expand redraw \a rect with redraw \a rect from previous step to
   * prevent partial-redraw issues caused by fast strokes. This is
   * needed here (not in sculpt_flush_update) as it was before
   * because redraw rectangle should be the same in both of
   * optimized bke::pbvh::Tree draw function and 3d view redraw, if not -- some
   * mesh parts could disappear from screen (sergey). */
  SculptSession &ss = *ob.sculpt;

  if (!ss.cache) {
    return;
  }

  if (BLI_rcti_is_empty(&ss.cache->previous_r)) {
    return;
  }

  BLI_rcti_union(&rect, &ss.cache->previous_r);
}

bool SCULPT_get_redraw_rect(const ARegion &region,
                            const RegionView3D &rv3d,
                            const Object &ob,
                            rcti &rect)
{
  using namespace blender;
  bke::pbvh::Tree *pbvh = ob.sculpt->pbvh.get();
  if (!pbvh) {
    return false;
  }

  const Bounds<float3> bounds = BKE_pbvh_redraw_BB(*pbvh);

  /* Convert 3D bounding box to screen space. */
  if (!paint_convert_bb_to_rect(&rect, bounds.min, bounds.max, region, rv3d, ob)) {
    return false;
  }

  return true;
}

/************************ Brush Testing *******************/

void SCULPT_brush_test_init(const SculptSession &ss, SculptBrushTest &test)
{
  using namespace blender;
  RegionView3D *rv3d = ss.cache ? ss.cache->vc->rv3d : ss.rv3d;
  View3D *v3d = ss.cache ? ss.cache->vc->v3d : ss.v3d;

  test.radius_squared = ss.cache ? ss.cache->radius_squared : ss.cursor_radius * ss.cursor_radius;
  test.radius = std::sqrt(test.radius_squared);

  if (ss.cache) {
    test.location = ss.cache->location;
    test.mirror_symmetry_pass = ss.cache->mirror_symmetry_pass;
    test.radial_symmetry_pass = ss.cache->radial_symmetry_pass;
    test.symm_rot_mat_inv = ss.cache->symm_rot_mat_inv;
  }
  else {
    test.location = ss.cursor_location;
    test.mirror_symmetry_pass = ePaintSymmetryFlags(0);
    test.radial_symmetry_pass = 0;

    test.symm_rot_mat_inv = float4x4::identity();
  }

  /* Just for initialize. */
  test.dist = 0.0f;

  /* Only for 2D projection. */
  zero_v4(test.plane_view);
  zero_v4(test.plane_tool);

  if (RV3D_CLIPPING_ENABLED(v3d, rv3d)) {
    test.clip_rv3d = rv3d;
  }
  else {
    test.clip_rv3d = nullptr;
  }
}

BLI_INLINE bool sculpt_brush_test_clipping(const SculptBrushTest &test, const float co[3])
{
  RegionView3D *rv3d = test.clip_rv3d;
  if (!rv3d) {
    return false;
  }
  float3 symm_co = blender::ed::sculpt_paint::symmetry_flip(co, test.mirror_symmetry_pass);
  if (test.radial_symmetry_pass) {
    mul_m4_v3(test.symm_rot_mat_inv.ptr(), symm_co);
  }
  return ED_view3d_clipping_test(rv3d, symm_co, true);
}

bool SCULPT_brush_test_sphere_sq(SculptBrushTest &test, const float co[3])
{
  float distsq = len_squared_v3v3(co, test.location);

  if (distsq > test.radius_squared) {
    return false;
  }
  if (sculpt_brush_test_clipping(test, co)) {
    return false;
  }
  test.dist = distsq;
  return true;
}

bool SCULPT_brush_test_circle_sq(SculptBrushTest &test, const float co[3])
{
  float co_proj[3];
  closest_to_plane_normalized_v3(co_proj, test.plane_view, co);
  float distsq = len_squared_v3v3(co_proj, test.location);

  if (distsq > test.radius_squared) {
    return false;
  }

  if (sculpt_brush_test_clipping(test, co)) {
    return false;
  }

  test.dist = distsq;
  return true;
}

bool SCULPT_brush_test_cube(SculptBrushTest &test,
                            const float co[3],
                            const float local[4][4],
                            const float roundness,
                            const float /*tip_scale_x*/)
{
  float side = 1.0f;
  float local_co[3];

  if (sculpt_brush_test_clipping(test, co)) {
    return false;
  }

  mul_v3_m4v3(local_co, local, co);

  local_co[0] = fabsf(local_co[0]);
  local_co[1] = fabsf(local_co[1]);
  local_co[2] = fabsf(local_co[2]);

  /* Keep the square and circular brush tips the same size. */
  side += (1.0f - side) * roundness;

  const float hardness = 1.0f - roundness;
  const float constant_side = hardness * side;
  const float falloff_side = roundness * side;

  if (!(local_co[0] <= side && local_co[1] <= side && local_co[2] <= side)) {
    /* Outside the square. */
    return false;
  }
  if (min_ff(local_co[0], local_co[1]) > constant_side) {
    /* Corner, distance to the center of the corner circle. */
    float r_point[3];
    copy_v3_fl(r_point, constant_side);
    test.dist = len_v2v2(r_point, local_co) / falloff_side;
    return true;
  }
  if (max_ff(local_co[0], local_co[1]) > constant_side) {
    /* Side, distance to the square XY axis. */
    test.dist = (max_ff(local_co[0], local_co[1]) - constant_side) / falloff_side;
    return true;
  }

  /* Inside the square, constant distance. */
  test.dist = 0.0f;
  return true;
}

SculptBrushTestFn SCULPT_brush_test_init_with_falloff_shape(const SculptSession &ss,
                                                            SculptBrushTest &test,
                                                            char falloff_shape)
{
  if (!ss.cache && !ss.filter_cache) {
    falloff_shape = PAINT_FALLOFF_SHAPE_SPHERE;
  }

  SCULPT_brush_test_init(ss, test);
  SculptBrushTestFn sculpt_brush_test_sq_fn;
  if (falloff_shape == PAINT_FALLOFF_SHAPE_SPHERE) {
    sculpt_brush_test_sq_fn = SCULPT_brush_test_sphere_sq;
  }
  else {
    BLI_assert(falloff_shape == PAINT_FALLOFF_SHAPE_TUBE);
    const float3 view_normal = ss.cache ? ss.cache->view_normal : ss.filter_cache->view_normal;

    plane_from_point_normal_v3(test.plane_view, test.location, view_normal);
    sculpt_brush_test_sq_fn = SCULPT_brush_test_circle_sq;
  }
  return sculpt_brush_test_sq_fn;
}

const float *SCULPT_brush_frontface_normal_from_falloff_shape(const SculptSession &ss,
                                                              char falloff_shape)
{
  if (falloff_shape == PAINT_FALLOFF_SHAPE_SPHERE) {
    return ss.cache->sculpt_normal_symm;
  }
  BLI_assert(falloff_shape == PAINT_FALLOFF_SHAPE_TUBE);
  return ss.cache->view_normal;
}

static float frontface(const Brush &brush, const float3 &view_normal, const float3 &normal)
{
  using namespace blender;
  if (!(brush.flag & BRUSH_FRONTFACE)) {
    return 1.0f;
  }
  return std::max(math::dot(normal, view_normal), 0.0f);
}

#if 0

static bool sculpt_brush_test_cyl(SculptBrushTest *test,
                                  float co[3],
                                  float location[3],
                                  const float area_no[3])
{
  if (sculpt_brush_test_sphere_fast(test, co)) {
    float t1[3], t2[3], t3[3], dist;

    sub_v3_v3v3(t1, location, co);
    sub_v3_v3v3(t2, x2, location);

    cross_v3_v3v3(t3, area_no, t1);

    dist = len_v3(t3) / len_v3(t2);

    test.dist = dist;

    return true;
  }

  return false;
}

#endif

/* ===== Sculpting =====
 */

static float calc_overlap(const blender::ed::sculpt_paint::StrokeCache &cache,
                          const ePaintSymmetryFlags symm,
                          const char axis,
                          const float angle)
{
  float3 mirror = blender::ed::sculpt_paint::symmetry_flip(cache.true_location, symm);

  if (axis != 0) {
    float mat[3][3];
    axis_angle_to_mat3_single(mat, axis, angle);
    mul_m3_v3(mat, mirror);
  }

  const float distsq = len_squared_v3v3(mirror, cache.true_location);

  if (distsq <= 4.0f * (cache.radius_squared)) {
    return (2.0f * (cache.radius) - sqrtf(distsq)) / (2.0f * (cache.radius));
  }
  return 0.0f;
}

static float calc_radial_symmetry_feather(const Sculpt &sd,
                                          const blender::ed::sculpt_paint::StrokeCache &cache,
                                          const ePaintSymmetryFlags symm,
                                          const char axis)
{
  float overlap = 0.0f;

  for (int i = 1; i < sd.radial_symm[axis - 'X']; i++) {
    const float angle = 2.0f * M_PI * i / sd.radial_symm[axis - 'X'];
    overlap += calc_overlap(cache, symm, axis, angle);
  }

  return overlap;
}

static float calc_symmetry_feather(const Sculpt &sd,
                                   const blender::ed::sculpt_paint::StrokeCache &cache)
{
  if (!(sd.paint.symmetry_flags & PAINT_SYMMETRY_FEATHER)) {
    return 1.0f;
  }
  float overlap;
  const int symm = cache.symmetry;

  overlap = 0.0f;
  for (int i = 0; i <= symm; i++) {
    if (!SCULPT_is_symmetry_iteration_valid(i, symm)) {
      continue;
    }

    overlap += calc_overlap(cache, ePaintSymmetryFlags(i), 0, 0);

    overlap += calc_radial_symmetry_feather(sd, cache, ePaintSymmetryFlags(i), 'X');
    overlap += calc_radial_symmetry_feather(sd, cache, ePaintSymmetryFlags(i), 'Y');
    overlap += calc_radial_symmetry_feather(sd, cache, ePaintSymmetryFlags(i), 'Z');
  }
  return 1.0f / overlap;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Calculate Normal and Center
 *
 * Calculate geometry surrounding the brush center.
 * (optionally using original coordinates).
 *
 * Functions are:
 * - #calc_area_center
 * - #calc_area_normal
 * - #calc_area_normal_and_center
 *
 * \note These are all _very_ similar, when changing one, check others.
 * \{ */

namespace blender::ed::sculpt_paint {

struct AreaNormalCenterData {
  /* 0 = towards view, 1 = flipped */
  std::array<float3, 2> area_cos;
  std::array<int, 2> count_co;

  std::array<float3, 2> area_nos;
  std::array<int, 2> count_no;
};

static float area_normal_and_center_get_normal_radius(const SculptSession &ss, const Brush &brush)
{
  float test_radius = ss.cache ? ss.cache->radius : ss.cursor_radius;
  if (brush.ob_mode == OB_MODE_SCULPT) {
    test_radius *= brush.normal_radius_factor;
  }
  return test_radius;
}

static SculptBrushTestFn area_normal_and_center_get_normal_test(const SculptSession &ss,
                                                                const Brush &brush,
                                                                SculptBrushTest &r_test)
{
  SculptBrushTestFn sculpt_brush_normal_test_sq_fn = SCULPT_brush_test_init_with_falloff_shape(
      ss, r_test, brush.falloff_shape);

  r_test.radius = area_normal_and_center_get_normal_radius(ss, brush);
  r_test.radius_squared = r_test.radius * r_test.radius;

  return sculpt_brush_normal_test_sq_fn;
}

static float area_normal_and_center_get_position_radius(const SculptSession &ss,
                                                        const Brush &brush)
{
  float test_radius = ss.cache ? ss.cache->radius : ss.cursor_radius;
  if (brush.ob_mode == OB_MODE_SCULPT) {
    /* Layer brush produces artifacts with normal and area radius */
    /* Enable area radius control only on Scrape for now */
    if (ELEM(brush.sculpt_tool, SCULPT_TOOL_SCRAPE, SCULPT_TOOL_FILL) &&
        brush.area_radius_factor > 0.0f)
    {
      test_radius *= brush.area_radius_factor;
      if (ss.cache && brush.flag2 & BRUSH_AREA_RADIUS_PRESSURE) {
        test_radius *= ss.cache->pressure;
      }
    }
    else {
      test_radius *= brush.normal_radius_factor;
    }
  }
  return test_radius;
}

static SculptBrushTestFn area_normal_and_center_get_area_test(const SculptSession &ss,
                                                              const Brush &brush,
                                                              SculptBrushTest &r_test)
{
  SculptBrushTestFn sculpt_brush_area_test_sq_fn = SCULPT_brush_test_init_with_falloff_shape(
      ss, r_test, brush.falloff_shape);

  r_test.radius = area_normal_and_center_get_position_radius(ss, brush);
  r_test.radius_squared = r_test.radius * r_test.radius;

  return sculpt_brush_area_test_sq_fn;
}

/* Weight the normals towards the center. */
static float area_normal_calc_weight(const float distance, const float radius)
{
  float p = 1.0f - (std::sqrt(distance) / radius);
  return std::clamp(3.0f * p * p - 2.0f * p * p * p, 0.0f, 1.0f);
}

/* Weight the coordinates towards the center. */
static float3 area_center_calc_weighted(const float3 &test_location,
                                        const float distance,
                                        const float radius,
                                        const float3 &co)
{
  /* Weight the coordinates towards the center. */
  float p = 1.0f - (std::sqrt(distance) / radius);
  const float afactor = std::clamp(3.0f * p * p - 2.0f * p * p * p, 0.0f, 1.0f);

  const float3 disp = (co - test_location) * (1.0f - afactor);
  return test_location + disp;
}

static void calc_area_normal_and_center_node_mesh(const SculptSession &ss,
                                                  const Span<float3> vert_positions,
                                                  const Span<float3> vert_normals,
                                                  const Span<bool> hide_vert,
                                                  const Brush &brush,
                                                  const bool use_area_nos,
                                                  const bool use_area_cos,
                                                  const bke::pbvh::Node &node,
                                                  AreaNormalCenterData &anctd)
{
  const float3 &view_normal = ss.cache ? ss.cache->view_normal : ss.cursor_view_normal;
  SculptBrushTest normal_test;
  SculptBrushTestFn sculpt_brush_normal_test_sq_fn = area_normal_and_center_get_normal_test(
      ss, brush, normal_test);

  SculptBrushTest area_test;
  SculptBrushTestFn sculpt_brush_area_test_sq_fn = area_normal_and_center_get_area_test(
      ss, brush, area_test);

  if (ss.cache && !ss.cache->accum) {
    if (const undo::Node *unode = undo::get_node(&node, undo::Type::Position)) {
      const Span<float3> orig_positions = unode->position;
      const Span<float3> orig_normals = unode->normal;
      const Span<int> verts = bke::pbvh::node_unique_verts(node);
      for (const int i : verts.index_range()) {
        const int vert = verts[i];
        if (!hide_vert.is_empty() && hide_vert[vert]) {
          continue;
        }
        const float3 &co = orig_positions[i];
        const float3 &no = orig_normals[i];

        const bool normal_test_r = sculpt_brush_normal_test_sq_fn(normal_test, co);
        const bool area_test_r = sculpt_brush_area_test_sq_fn(area_test, co);
        if (!normal_test_r && !area_test_r) {
          continue;
        }

        const int flip_index = math::dot(view_normal, no) <= 0.0f;
        if (use_area_cos && area_test_r) {
          anctd.area_cos[flip_index] += area_center_calc_weighted(
              area_test.location, area_test.dist, area_test.radius, co);
          anctd.count_co[flip_index] += 1;
        }
        if (use_area_nos && normal_test_r) {
          anctd.area_nos[flip_index] += no * area_normal_calc_weight(normal_test.dist,
                                                                     normal_test.radius);
          anctd.count_no[flip_index] += 1;
        }
      }
      return;
    }
  }

  const Span<int> verts = bke::pbvh::node_unique_verts(node);
  for (const int i : verts.index_range()) {
    const int vert = verts[i];
    if (!hide_vert.is_empty() && hide_vert[vert]) {
      continue;
    }
    const float3 &co = vert_positions[vert];
    const float3 &no = vert_normals[vert];

    const bool normal_test_r = sculpt_brush_normal_test_sq_fn(normal_test, co);
    const bool area_test_r = sculpt_brush_area_test_sq_fn(area_test, co);
    if (!normal_test_r && !area_test_r) {
      continue;
    }

    const int flip_index = math::dot(view_normal, no) <= 0.0f;
    if (use_area_cos && area_test_r) {
      anctd.area_cos[flip_index] += area_center_calc_weighted(
          area_test.location, area_test.dist, area_test.radius, co);
      anctd.count_co[flip_index] += 1;
    }
    if (use_area_nos && normal_test_r) {
      anctd.area_nos[flip_index] += no *
                                    area_normal_calc_weight(normal_test.dist, normal_test.radius);
      anctd.count_no[flip_index] += 1;
    }
  }
}

static void calc_area_normal_and_center_node_grids(const SculptSession &ss,
                                                   const Brush &brush,
                                                   const bool use_area_nos,
                                                   const bool use_area_cos,
                                                   const bke::pbvh::Node &node,
                                                   AreaNormalCenterData &anctd)
{
  const float3 &view_normal = ss.cache ? ss.cache->view_normal : ss.cursor_view_normal;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(*ss.subdiv_ccg);
  const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
  const Span<CCGElem *> grids = subdiv_ccg.grids;
  const BitGroupVector<> &grid_hidden = subdiv_ccg.grid_hidden;

  SculptBrushTest normal_test;
  SculptBrushTestFn sculpt_brush_normal_test_sq_fn = area_normal_and_center_get_normal_test(
      ss, brush, normal_test);

  SculptBrushTest area_test;
  SculptBrushTestFn sculpt_brush_area_test_sq_fn = area_normal_and_center_get_area_test(
      ss, brush, area_test);

  const undo::Node *unode = nullptr;
  bool use_original = false;
  if (ss.cache && !ss.cache->accum) {
    unode = undo::get_node(&node, undo::Type::Position);
    if (unode) {
      use_original = !unode->position.is_empty();
    }
  }

  int i = 0;
  for (const int grid : bke::pbvh::node_grid_indices(node)) {
    CCGElem *elem = grids[grid];
    for (const int j : IndexRange(key.grid_area)) {
      if (!grid_hidden.is_empty() && grid_hidden[grid][j]) {
        i++;
        continue;
      }
      float3 co;
      float3 no;
      if (use_original) {
        co = unode->position[i];
        no = unode->normal[i];
      }
      else {
        co = CCG_elem_offset_co(key, elem, j);
        no = CCG_elem_offset_no(key, elem, j);
      }

      const bool normal_test_r = sculpt_brush_normal_test_sq_fn(normal_test, co);
      const bool area_test_r = sculpt_brush_area_test_sq_fn(area_test, co);
      if (!normal_test_r && !area_test_r) {
        i++;
        continue;
      }

      const int flip_index = math::dot(view_normal, no) <= 0.0f;
      if (use_area_cos && area_test_r) {
        anctd.area_cos[flip_index] += area_center_calc_weighted(
            area_test.location, area_test.dist, area_test.radius, co);
        anctd.count_co[flip_index] += 1;
      }
      if (use_area_nos && normal_test_r) {
        anctd.area_nos[flip_index] += no * area_normal_calc_weight(normal_test.dist,
                                                                   normal_test.radius);
        anctd.count_no[flip_index] += 1;
      }

      i++;
    }
  }
}

static void calc_area_normal_and_center_node_bmesh(const SculptSession &ss,
                                                   const Brush &brush,
                                                   const bool use_area_nos,
                                                   const bool use_area_cos,
                                                   const bool has_bm_orco,
                                                   const bke::pbvh::Node &node,
                                                   AreaNormalCenterData &anctd)
{
  const float3 &view_normal = ss.cache ? ss.cache->view_normal : ss.cursor_view_normal;
  SculptBrushTest normal_test;
  SculptBrushTestFn sculpt_brush_normal_test_sq_fn = area_normal_and_center_get_normal_test(
      ss, brush, normal_test);

  SculptBrushTest area_test;
  SculptBrushTestFn sculpt_brush_area_test_sq_fn = area_normal_and_center_get_area_test(
      ss, brush, area_test);

  const undo::Node *unode = nullptr;
  bool use_original = false;
  if (ss.cache && !ss.cache->accum) {
    unode = undo::get_node(&node, undo::Type::Position);
    if (unode) {
      use_original = undo::get_bmesh_log_entry() != nullptr;
    }
  }

  /* When the mesh is edited we can't rely on original coords
   * (original mesh may not even have verts in brush radius). */
  if (use_original && has_bm_orco) {
    float(*orco_coords)[3];
    int(*orco_tris)[3];
    int orco_tris_num;
    BKE_pbvh_node_get_bm_orco_data(
        &const_cast<bke::pbvh::Node &>(node), &orco_tris, &orco_tris_num, &orco_coords, nullptr);

    for (int i = 0; i < orco_tris_num; i++) {
      const float *co_tri[3] = {
          orco_coords[orco_tris[i][0]],
          orco_coords[orco_tris[i][1]],
          orco_coords[orco_tris[i][2]],
      };
      float3 co;
      closest_on_tri_to_point_v3(co, normal_test.location, UNPACK3(co_tri));

      const bool normal_test_r = sculpt_brush_normal_test_sq_fn(normal_test, co);
      const bool area_test_r = sculpt_brush_area_test_sq_fn(area_test, co);
      if (!normal_test_r && !area_test_r) {
        continue;
      }

      float3 no;
      normal_tri_v3(no, UNPACK3(co_tri));

      const int flip_index = math::dot(view_normal, no) <= 0.0f;
      if (use_area_cos && area_test_r) {
        anctd.area_cos[flip_index] += area_center_calc_weighted(
            area_test.location, area_test.dist, area_test.radius, co);
        anctd.count_co[flip_index] += 1;
      }
      if (use_area_nos && normal_test_r) {
        anctd.area_nos[flip_index] += no * area_normal_calc_weight(normal_test.dist,
                                                                   normal_test.radius);
        anctd.count_no[flip_index] += 1;
      }
    }
  }
  else {
    for (BMVert *vert : BKE_pbvh_bmesh_node_unique_verts(&const_cast<bke::pbvh::Node &>(node))) {
      if (BM_elem_flag_test(vert, BM_ELEM_HIDDEN)) {
        continue;
      }
      float3 co;
      float3 no;
      if (use_original) {
        const float *temp_co;
        const float *temp_no_s;
        BM_log_original_vert_data(ss.bm_log, vert, &temp_co, &temp_no_s);
        co = temp_co;
        no = temp_no_s;
      }
      else {
        co = vert->co;
        no = vert->no;
      }

      const bool normal_test_r = sculpt_brush_normal_test_sq_fn(normal_test, co);
      const bool area_test_r = sculpt_brush_area_test_sq_fn(area_test, co);
      if (!normal_test_r && !area_test_r) {
        continue;
      }

      const int flip_index = math::dot(view_normal, no) <= 0.0f;
      if (use_area_cos && area_test_r) {
        anctd.area_cos[flip_index] += area_center_calc_weighted(
            area_test.location, area_test.dist, area_test.radius, co);
        anctd.count_co[flip_index] += 1;
      }
      if (use_area_nos && normal_test_r) {
        anctd.area_nos[flip_index] += no * area_normal_calc_weight(normal_test.dist,
                                                                   normal_test.radius);
        anctd.count_no[flip_index] += 1;
      }
    }
  }
}

static AreaNormalCenterData calc_area_normal_and_center_reduce(const AreaNormalCenterData &a,
                                                               const AreaNormalCenterData &b)
{
  AreaNormalCenterData joined{};

  joined.area_cos[0] = a.area_cos[0] + b.area_cos[0];
  joined.area_cos[1] = a.area_cos[1] + b.area_cos[1];
  joined.count_co[0] = a.count_co[0] + b.count_co[0];
  joined.count_co[1] = a.count_co[1] + b.count_co[1];

  joined.area_nos[0] = a.area_nos[0] + b.area_nos[0];
  joined.area_nos[1] = a.area_nos[1] + b.area_nos[1];
  joined.count_no[0] = a.count_no[0] + b.count_no[0];
  joined.count_no[1] = a.count_no[1] + b.count_no[1];

  return joined;
}

void calc_area_center(const Brush &brush,
                      const Object &ob,
                      Span<bke::pbvh::Node *> nodes,
                      float r_area_co[3])
{
  const SculptSession &ss = *ob.sculpt;
  int n;

  AreaNormalCenterData anctd;
  switch (ss.pbvh->type()) {
    case bke::pbvh::Type::Mesh: {
      const Mesh &mesh = *static_cast<const Mesh *>(ob.data);
      const Span<float3> vert_positions = BKE_pbvh_get_vert_positions(*ss.pbvh);
      const Span<float3> vert_normals = BKE_pbvh_get_vert_normals(*ss.pbvh);
      const bke::AttributeAccessor attributes = mesh.attributes();
      const VArraySpan hide_vert = *attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point);

      anctd = threading::parallel_reduce(
          nodes.index_range(),
          1,
          AreaNormalCenterData{},
          [&](const IndexRange range, AreaNormalCenterData anctd) {
            for (const int i : range) {
              calc_area_normal_and_center_node_mesh(ss,
                                                    vert_positions,
                                                    vert_normals,
                                                    hide_vert,
                                                    brush,
                                                    false,
                                                    true,
                                                    *nodes[i],
                                                    anctd);
            }
            return anctd;
          },
          calc_area_normal_and_center_reduce);
      break;
    }
    case bke::pbvh::Type::BMesh: {
      const bool has_bm_orco = ss.bm && dyntopo::stroke_is_dyntopo(ss, brush);

      anctd = threading::parallel_reduce(
          nodes.index_range(),
          1,
          AreaNormalCenterData{},
          [&](const IndexRange range, AreaNormalCenterData anctd) {
            for (const int i : range) {
              calc_area_normal_and_center_node_bmesh(
                  ss, brush, false, true, has_bm_orco, *nodes[i], anctd);
            }
            return anctd;
          },
          calc_area_normal_and_center_reduce);
      break;
    }
    case bke::pbvh::Type::Grids: {
      anctd = threading::parallel_reduce(
          nodes.index_range(),
          1,
          AreaNormalCenterData{},
          [&](const IndexRange range, AreaNormalCenterData anctd) {
            for (const int i : range) {
              calc_area_normal_and_center_node_grids(ss, brush, false, true, *nodes[i], anctd);
            }
            return anctd;
          },
          calc_area_normal_and_center_reduce);
      break;
    }
  }

  /* For flatten center. */
  for (n = 0; n < anctd.area_cos.size(); n++) {
    if (anctd.count_co[n] == 0) {
      continue;
    }

    mul_v3_v3fl(r_area_co, anctd.area_cos[n], 1.0f / anctd.count_co[n]);
    break;
  }

  if (n == 2) {
    zero_v3(r_area_co);
  }

  if (anctd.count_co[0] == 0 && anctd.count_co[1] == 0) {
    if (ss.cache) {
      copy_v3_v3(r_area_co, ss.cache->location);
    }
  }
}

std::optional<float3> calc_area_normal(const Brush &brush,
                                       Object &ob,
                                       Span<bke::pbvh::Node *> nodes)
{
  SculptSession &ss = *ob.sculpt;

  AreaNormalCenterData anctd;
  switch (ss.pbvh->type()) {
    case bke::pbvh::Type::Mesh: {
      const Mesh &mesh = *static_cast<const Mesh *>(ob.data);
      const Span<float3> vert_positions = BKE_pbvh_get_vert_positions(*ss.pbvh);
      const Span<float3> vert_normals = BKE_pbvh_get_vert_normals(*ss.pbvh);
      const bke::AttributeAccessor attributes = mesh.attributes();
      const VArraySpan hide_vert = *attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point);

      anctd = threading::parallel_reduce(
          nodes.index_range(),
          1,
          AreaNormalCenterData{},
          [&](const IndexRange range, AreaNormalCenterData anctd) {
            for (const int i : range) {
              calc_area_normal_and_center_node_mesh(ss,
                                                    vert_positions,
                                                    vert_normals,
                                                    hide_vert,
                                                    brush,
                                                    true,
                                                    false,
                                                    *nodes[i],
                                                    anctd);
            }
            return anctd;
          },
          calc_area_normal_and_center_reduce);
      break;
    }
    case bke::pbvh::Type::BMesh: {
      const bool has_bm_orco = ss.bm && dyntopo::stroke_is_dyntopo(ss, brush);

      anctd = threading::parallel_reduce(
          nodes.index_range(),
          1,
          AreaNormalCenterData{},
          [&](const IndexRange range, AreaNormalCenterData anctd) {
            for (const int i : range) {
              calc_area_normal_and_center_node_bmesh(
                  ss, brush, true, false, has_bm_orco, *nodes[i], anctd);
            }
            return anctd;
          },
          calc_area_normal_and_center_reduce);
      break;
    }
    case bke::pbvh::Type::Grids: {
      anctd = threading::parallel_reduce(
          nodes.index_range(),
          1,
          AreaNormalCenterData{},
          [&](const IndexRange range, AreaNormalCenterData anctd) {
            for (const int i : range) {
              calc_area_normal_and_center_node_grids(ss, brush, true, false, *nodes[i], anctd);
            }
            return anctd;
          },
          calc_area_normal_and_center_reduce);
      break;
    }
  }

  for (const int i : {0, 1}) {
    if (anctd.count_no[i] != 0) {
      if (!math::is_zero(anctd.area_nos[i])) {
        return math::normalize(anctd.area_nos[i]);
      }
    }
  }
  return std::nullopt;
}

void calc_area_normal_and_center(const Brush &brush,
                                 const Object &ob,
                                 Span<bke::pbvh::Node *> nodes,
                                 float r_area_no[3],
                                 float r_area_co[3])
{
  SculptSession &ss = *ob.sculpt;
  int n;

  AreaNormalCenterData anctd;
  switch (ss.pbvh->type()) {
    case bke::pbvh::Type::Mesh: {
      const Mesh &mesh = *static_cast<const Mesh *>(ob.data);
      const Span<float3> vert_positions = BKE_pbvh_get_vert_positions(*ss.pbvh);
      const Span<float3> vert_normals = BKE_pbvh_get_vert_normals(*ss.pbvh);
      const bke::AttributeAccessor attributes = mesh.attributes();
      const VArraySpan hide_vert = *attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point);

      anctd = threading::parallel_reduce(
          nodes.index_range(),
          1,
          AreaNormalCenterData{},
          [&](const IndexRange range, AreaNormalCenterData anctd) {
            for (const int i : range) {
              calc_area_normal_and_center_node_mesh(ss,
                                                    vert_positions,
                                                    vert_normals,
                                                    hide_vert,
                                                    brush,
                                                    true,
                                                    true,
                                                    *nodes[i],
                                                    anctd);
            }
            return anctd;
          },
          calc_area_normal_and_center_reduce);
      break;
    }
    case bke::pbvh::Type::BMesh: {
      const bool has_bm_orco = ss.bm && dyntopo::stroke_is_dyntopo(ss, brush);

      anctd = threading::parallel_reduce(
          nodes.index_range(),
          1,
          AreaNormalCenterData{},
          [&](const IndexRange range, AreaNormalCenterData anctd) {
            for (const int i : range) {
              calc_area_normal_and_center_node_bmesh(
                  ss, brush, true, true, has_bm_orco, *nodes[i], anctd);
            }
            return anctd;
          },
          calc_area_normal_and_center_reduce);
      break;
    }
    case bke::pbvh::Type::Grids: {
      anctd = threading::parallel_reduce(
          nodes.index_range(),
          1,
          AreaNormalCenterData{},
          [&](const IndexRange range, AreaNormalCenterData anctd) {
            for (const int i : range) {
              calc_area_normal_and_center_node_grids(ss, brush, true, true, *nodes[i], anctd);
            }
            return anctd;
          },
          calc_area_normal_and_center_reduce);
      break;
    }
  }

  /* For flatten center. */
  for (n = 0; n < anctd.area_cos.size(); n++) {
    if (anctd.count_co[n] == 0) {
      continue;
    }

    mul_v3_v3fl(r_area_co, anctd.area_cos[n], 1.0f / anctd.count_co[n]);
    break;
  }

  if (n == 2) {
    zero_v3(r_area_co);
  }

  if (anctd.count_co[0] == 0 && anctd.count_co[1] == 0) {
    if (ss.cache) {
      copy_v3_v3(r_area_co, ss.cache->location);
    }
  }

  /* For area normal. */
  for (n = 0; n < anctd.area_nos.size(); n++) {
    if (normalize_v3_v3(r_area_no, anctd.area_nos[n]) != 0.0f) {
      break;
    }
  }
}

}  // namespace blender::ed::sculpt_paint

/** \} */

/* -------------------------------------------------------------------- */
/** \name Generic Brush Utilities
 * \{ */

/**
 * Return modified brush strength. Includes the direction of the brush, positive
 * values pull vertices, negative values push. Uses tablet pressure and a
 * special multiplier found experimentally to scale the strength factor.
 */
static float brush_strength(const Sculpt &sd,
                            const blender::ed::sculpt_paint::StrokeCache &cache,
                            const float feather,
                            const UnifiedPaintSettings &ups,
                            const PaintModeSettings & /*paint_mode_settings*/)
{
  const Scene *scene = cache.vc->scene;
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);

  /* Primary strength input; square it to make lower values more sensitive. */
  const float root_alpha = BKE_brush_alpha_get(scene, &brush);
  const float alpha = root_alpha * root_alpha;
  const float dir = (brush.flag & BRUSH_DIR_IN) ? -1.0f : 1.0f;
  const float pressure = BKE_brush_use_alpha_pressure(&brush) ? cache.pressure : 1.0f;
  const float pen_flip = cache.pen_flip ? -1.0f : 1.0f;
  const float invert = cache.invert ? -1.0f : 1.0f;
  float overlap = ups.overlap_factor;
  /* Spacing is integer percentage of radius, divide by 50 to get
   * normalized diameter. */

  float flip = dir * invert * pen_flip;
  if (brush.flag & BRUSH_INVERT_TO_SCRAPE_FILL) {
    flip = 1.0f;
  }

  /* Pressure final value after being tweaked depending on the brush. */
  float final_pressure;

  switch (brush.sculpt_tool) {
    case SCULPT_TOOL_CLAY:
      final_pressure = pow4f(pressure);
      overlap = (1.0f + overlap) / 2.0f;
      return 0.25f * alpha * flip * final_pressure * overlap * feather;
    case SCULPT_TOOL_DRAW:
    case SCULPT_TOOL_DRAW_SHARP:
    case SCULPT_TOOL_LAYER:
      return alpha * flip * pressure * overlap * feather;
    case SCULPT_TOOL_DISPLACEMENT_ERASER:
      return alpha * pressure * overlap * feather;
    case SCULPT_TOOL_CLOTH:
      if (brush.cloth_deform_type == BRUSH_CLOTH_DEFORM_GRAB) {
        /* Grab deform uses the same falloff as a regular grab brush. */
        return root_alpha * feather;
      }
      else if (brush.cloth_deform_type == BRUSH_CLOTH_DEFORM_SNAKE_HOOK) {
        return root_alpha * feather * pressure * overlap;
      }
      else if (brush.cloth_deform_type == BRUSH_CLOTH_DEFORM_EXPAND) {
        /* Expand is more sensible to strength as it keeps expanding the cloth when sculpting over
         * the same vertices. */
        return 0.1f * alpha * flip * pressure * overlap * feather;
      }
      else {
        /* Multiply by 10 by default to get a larger range of strength depending on the size of the
         * brush and object. */
        return 10.0f * alpha * flip * pressure * overlap * feather;
      }
    case SCULPT_TOOL_DRAW_FACE_SETS:
      return alpha * pressure * overlap * feather;
    case SCULPT_TOOL_SLIDE_RELAX:
      return alpha * pressure * overlap * feather * 2.0f;
    case SCULPT_TOOL_PAINT:
      final_pressure = pressure * pressure;
      return final_pressure * overlap * feather;
    case SCULPT_TOOL_SMEAR:
    case SCULPT_TOOL_DISPLACEMENT_SMEAR:
      return alpha * pressure * overlap * feather;
    case SCULPT_TOOL_CLAY_STRIPS:
      /* Clay Strips needs less strength to compensate the curve. */
      final_pressure = powf(pressure, 1.5f);
      return alpha * flip * final_pressure * overlap * feather * 0.3f;
    case SCULPT_TOOL_CLAY_THUMB:
      final_pressure = pressure * pressure;
      return alpha * flip * final_pressure * overlap * feather * 1.3f;

    case SCULPT_TOOL_MASK:
      overlap = (1.0f + overlap) / 2.0f;
      switch ((BrushMaskTool)brush.mask_tool) {
        case BRUSH_MASK_DRAW:
          return alpha * flip * pressure * overlap * feather;
        case BRUSH_MASK_SMOOTH:
          return alpha * pressure * feather;
      }
      BLI_assert_msg(0, "Not supposed to happen");
      return 0.0f;

    case SCULPT_TOOL_CREASE:
    case SCULPT_TOOL_BLOB:
      return alpha * flip * pressure * overlap * feather;

    case SCULPT_TOOL_INFLATE:
      if (flip > 0.0f) {
        return 0.250f * alpha * flip * pressure * overlap * feather;
      }
      else {
        return 0.125f * alpha * flip * pressure * overlap * feather;
      }

    case SCULPT_TOOL_MULTIPLANE_SCRAPE:
      overlap = (1.0f + overlap) / 2.0f;
      return alpha * flip * pressure * overlap * feather;

    case SCULPT_TOOL_FILL:
    case SCULPT_TOOL_SCRAPE:
    case SCULPT_TOOL_FLATTEN:
      if (flip > 0.0f) {
        overlap = (1.0f + overlap) / 2.0f;
        return alpha * flip * pressure * overlap * feather;
      }
      else {
        /* Reduce strength for DEEPEN, PEAKS, and CONTRAST. */
        return 0.5f * alpha * flip * pressure * overlap * feather;
      }

    case SCULPT_TOOL_SMOOTH:
      return flip * alpha * pressure * feather;

    case SCULPT_TOOL_PINCH:
      if (flip > 0.0f) {
        return alpha * flip * pressure * overlap * feather;
      }
      else {
        return 0.25f * alpha * flip * pressure * overlap * feather;
      }

    case SCULPT_TOOL_NUDGE:
      overlap = (1.0f + overlap) / 2.0f;
      return alpha * pressure * overlap * feather;

    case SCULPT_TOOL_THUMB:
      return alpha * pressure * feather;

    case SCULPT_TOOL_SNAKE_HOOK:
      return root_alpha * feather;

    case SCULPT_TOOL_GRAB:
      return root_alpha * feather;

    case SCULPT_TOOL_ROTATE:
      return alpha * pressure * feather;

    case SCULPT_TOOL_ELASTIC_DEFORM:
    case SCULPT_TOOL_POSE:
    case SCULPT_TOOL_BOUNDARY:
      return root_alpha * feather;

    default:
      return 0.0f;
  }
}

static float sculpt_apply_hardness(const blender::ed::sculpt_paint::StrokeCache &cache,
                                   const float input_len)
{
  float final_len = input_len;
  const float hardness = cache.paint_brush.hardness;
  float p = input_len / cache.radius;
  if (p < hardness) {
    final_len = 0.0f;
  }
  else if (hardness == 1.0f) {
    final_len = cache.radius;
  }
  else {
    p = (p - hardness) / (1.0f - hardness);
    final_len = p * cache.radius;
  }

  return final_len;
}

void sculpt_apply_texture(const SculptSession &ss,
                          const Brush &brush,
                          const float brush_point[3],
                          const int thread_id,
                          float *r_value,
                          float r_rgba[4])
{
  const blender::ed::sculpt_paint::StrokeCache &cache = *ss.cache;
  const Scene *scene = cache.vc->scene;
  const MTex *mtex = BKE_brush_mask_texture_get(&brush, OB_MODE_SCULPT);

  if (!mtex->tex) {
    *r_value = 1.0f;
    copy_v4_fl(r_rgba, 1.0f);
    return;
  }

  float point[3];
  sub_v3_v3v3(point, brush_point, cache.plane_offset);

  if (mtex->brush_map_mode == MTEX_MAP_MODE_3D) {
    /* Get strength by feeding the vertex location directly into a texture. */
    *r_value = BKE_brush_sample_tex_3d(scene, &brush, mtex, point, r_rgba, 0, ss.tex_pool);
  }
  else {
    /* If the active area is being applied for symmetry, flip it
     * across the symmetry axis and rotate it back to the original
     * position in order to project it. This insures that the
     * brush texture will be oriented correctly. */
    if (cache.radial_symmetry_pass) {
      mul_m4_v3(cache.symm_rot_mat_inv.ptr(), point);
    }
    float3 symm_point = blender::ed::sculpt_paint::symmetry_flip(point,
                                                                 cache.mirror_symmetry_pass);

    /* Still no symmetry supported for other paint modes.
     * Sculpt does it DIY. */
    if (mtex->brush_map_mode == MTEX_MAP_MODE_AREA) {
      /* Similar to fixed mode, but projects from brush angle
       * rather than view direction. */

      mul_m4_v3(cache.brush_local_mat.ptr(), symm_point);

      float x = symm_point[0];
      float y = symm_point[1];

      x *= mtex->size[0];
      y *= mtex->size[1];

      x += mtex->ofs[0];
      y += mtex->ofs[1];

      paint_get_tex_pixel(mtex, x, y, ss.tex_pool, thread_id, r_value, r_rgba);

      add_v3_fl(r_rgba, brush.texture_sample_bias);  // v3 -> Ignore alpha
      *r_value -= brush.texture_sample_bias;
    }
    else {
      const blender::float2 point_2d = ED_view3d_project_float_v2_m4(
          cache.vc->region, symm_point, cache.projection_mat);
      const float point_3d[3] = {point_2d[0], point_2d[1], 0.0f};
      *r_value = BKE_brush_sample_tex_3d(scene, &brush, mtex, point_3d, r_rgba, 0, ss.tex_pool);
    }
  }
}

float SCULPT_brush_strength_factor(
    SculptSession &ss,
    const Brush &brush,
    const float brush_point[3],
    float len,
    const float vno[3],
    const float fno[3],
    float mask,
    const PBVHVertRef vertex,
    int thread_id,
    const blender::ed::sculpt_paint::auto_mask::NodeData *automask_data)
{
  using namespace blender::ed::sculpt_paint;
  StrokeCache *cache = ss.cache;

  float avg = 1.0f;
  float rgba[4];
  sculpt_apply_texture(ss, brush, brush_point, thread_id, &avg, rgba);

  /* Hardness. */
  const float final_len = sculpt_apply_hardness(*cache, len);

  /* Falloff curve. */
  avg *= BKE_brush_curve_strength(&brush, final_len, cache->radius);
  avg *= frontface(brush, cache->view_normal, vno ? vno : fno);

  /* Paint mask. */
  avg *= 1.0f - mask;

  /* Auto-masking. */
  avg *= auto_mask::factor_get(cache->automasking.get(), ss, vertex, automask_data);

  return avg;
}

void SCULPT_calc_vertex_displacement(const SculptSession &ss,
                                     const Brush &brush,
                                     float rgba[3],
                                     float r_offset[3])
{
  mul_v3_fl(rgba, ss.cache->bstrength);
  /* Handle brush inversion */
  if (ss.cache->bstrength < 0) {
    rgba[0] *= -1;
    rgba[1] *= -1;
  }

  /* Apply texture size */
  for (int i = 0; i < 3; ++i) {
    rgba[i] *= blender::math::safe_divide(1.0f, pow2f(brush.mtex.size[i]));
  }

  /* Transform vector to object space */
  mul_mat3_m4_v3(ss.cache->brush_local_mat_inv.ptr(), rgba);

  /* Handle symmetry */
  if (ss.cache->radial_symmetry_pass) {
    mul_m4_v3(ss.cache->symm_rot_mat.ptr(), rgba);
  }
  copy_v3_v3(r_offset,
             blender::ed::sculpt_paint::symmetry_flip(rgba, ss.cache->mirror_symmetry_pass));
}

namespace blender::ed::sculpt_paint {

bool node_fully_masked_or_hidden(const bke::pbvh::Node &node)
{
  if (BKE_pbvh_node_fully_hidden_get(&node)) {
    return true;
  }
  if (BKE_pbvh_node_fully_masked_get(&node)) {
    return true;
  }
  return false;
}

bool node_in_sphere(const bke::pbvh::Node &node,
                    const float3 &location,
                    const float radius_sq,
                    const bool original)
{
  const Bounds<float3> bounds = original ? BKE_pbvh_node_get_original_BB(&node) :
                                           bke::pbvh::node_bounds(node);
  const float3 nearest = math::clamp(location, bounds.min, bounds.max);
  return math::distance_squared(location, nearest) < radius_sq;
}

bool node_in_cylinder(const DistRayAABB_Precalc &ray_dist_precalc,
                      const bke::pbvh::Node &node,
                      const float radius_sq,
                      const bool original)
{
  const Bounds<float3> bounds = (original) ? BKE_pbvh_node_get_original_BB(&node) :
                                             bke::pbvh::node_bounds(node);

  float dummy_co[3], dummy_depth;
  const float dist_sq = dist_squared_ray_to_aabb_v3(
      &ray_dist_precalc, bounds.min, bounds.max, dummy_co, &dummy_depth);

  /* TODO: Solve issues and enable distance check. */
  return dist_sq < radius_sq || true;
}

static Vector<bke::pbvh::Node *> sculpt_pbvh_gather_cursor_update(Object &ob, bool use_original)
{
  SculptSession &ss = *ob.sculpt;
  const float3 center = ss.cache ? ss.cache->location : ss.cursor_location;
  return bke::pbvh::search_gather(*ss.pbvh, [&](bke::pbvh::Node &node) {
    return node_in_sphere(node, center, ss.cursor_radius, use_original);
  });
}

/** \return All nodes that are potentially within the cursor or brush's area of influence. */
static Vector<bke::pbvh::Node *> sculpt_pbvh_gather_generic_intern(
    Object &ob, const Brush &brush, bool use_original, float radius_scale, PBVHNodeFlags flag)
{
  SculptSession &ss = *ob.sculpt;

  PBVHNodeFlags leaf_flag = PBVH_Leaf;
  if (flag & PBVH_TexLeaf) {
    leaf_flag = PBVH_TexLeaf;
  }

  const float3 center = ss.cache->location;
  const float radius_sq = math::square(ss.cache->radius * radius_scale);
  const bool ignore_ineffective = brush.sculpt_tool != SCULPT_TOOL_MASK;
  switch (brush.falloff_shape) {
    case PAINT_FALLOFF_SHAPE_SPHERE: {
      return bke::pbvh::search_gather(
          *ss.pbvh,
          [&](bke::pbvh::Node &node) {
            if (ignore_ineffective && node_fully_masked_or_hidden(node)) {
              return false;
            }
            return node_in_sphere(node, center, radius_sq, use_original);
          },
          leaf_flag);
    }

    case PAINT_FALLOFF_SHAPE_TUBE: {
      const DistRayAABB_Precalc ray_dist_precalc = dist_squared_ray_to_aabb_v3_precalc(
          center, ss.cache->view_normal);
      return bke::pbvh::search_gather(
          *ss.pbvh,
          [&](bke::pbvh::Node &node) {
            if (ignore_ineffective && node_fully_masked_or_hidden(node)) {
              return false;
            }
            return node_in_cylinder(ray_dist_precalc, node, radius_sq, use_original);
          },
          leaf_flag);
    }
  }

  return {};
}

static Vector<bke::pbvh::Node *> sculpt_pbvh_gather_generic(Object &ob,
                                                            const Brush &brush,
                                                            const bool use_original,
                                                            const float radius_scale)
{
  return sculpt_pbvh_gather_generic_intern(ob, brush, use_original, radius_scale, PBVH_Leaf);
}

static Vector<bke::pbvh::Node *> sculpt_pbvh_gather_texpaint(Object &ob,
                                                             const Brush &brush,
                                                             const bool use_original,
                                                             const float radius_scale)
{
  return sculpt_pbvh_gather_generic_intern(ob, brush, use_original, radius_scale, PBVH_TexLeaf);
}

/* Calculate primary direction of movement for many brushes. */
static float3 calc_sculpt_normal(const Sculpt &sd, Object &ob, Span<bke::pbvh::Node *> nodes)
{
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  const SculptSession &ss = *ob.sculpt;
  switch (brush.sculpt_plane) {
    case SCULPT_DISP_DIR_AREA:
      return calc_area_normal(brush, ob, nodes).value_or(float3(0));
    case SCULPT_DISP_DIR_VIEW:
      return ss.cache->true_view_normal;
    case SCULPT_DISP_DIR_X:
      return float3(1, 0, 0);
    case SCULPT_DISP_DIR_Y:
      return float3(0, 1, 0);
    case SCULPT_DISP_DIR_Z:
      return float3(0, 0, 1);
  }
  BLI_assert_unreachable();
  return {};
}

static void update_sculpt_normal(const Sculpt &sd, Object &ob, Span<bke::pbvh::Node *> nodes)
{
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  StrokeCache &cache = *ob.sculpt->cache;
  /* Grab brush does not update the sculpt normal during a stroke. */
  const bool update_normal =
      !(brush.flag & BRUSH_ORIGINAL_NORMAL) && !(brush.sculpt_tool == SCULPT_TOOL_GRAB) &&
      !(brush.sculpt_tool == SCULPT_TOOL_THUMB && !(brush.flag & BRUSH_ANCHORED)) &&
      !(brush.sculpt_tool == SCULPT_TOOL_ELASTIC_DEFORM) &&
      !(brush.sculpt_tool == SCULPT_TOOL_SNAKE_HOOK && cache.normal_weight > 0.0f);

  if (cache.mirror_symmetry_pass == 0 && cache.radial_symmetry_pass == 0 &&
      (SCULPT_stroke_is_first_brush_step_of_symmetry_pass(cache) || update_normal))
  {
    cache.sculpt_normal = calc_sculpt_normal(sd, ob, nodes);
    if (brush.falloff_shape == PAINT_FALLOFF_SHAPE_TUBE) {
      project_plane_v3_v3v3(cache.sculpt_normal, cache.sculpt_normal, cache.view_normal);
      normalize_v3(cache.sculpt_normal);
    }
    copy_v3_v3(cache.sculpt_normal_symm, cache.sculpt_normal);
  }
  else {
    cache.sculpt_normal_symm = symmetry_flip(cache.sculpt_normal, cache.mirror_symmetry_pass);
    mul_m4_v3(cache.symm_rot_mat.ptr(), cache.sculpt_normal_symm);
  }
}

static void calc_local_from_screen(const ViewContext &vc,
                                   const float center[3],
                                   const float screen_dir[2],
                                   float r_local_dir[3])
{
  Object &ob = *vc.obact;
  float loc[3];

  mul_v3_m4v3(loc, ob.object_to_world().ptr(), center);
  const float zfac = ED_view3d_calc_zfac(vc.rv3d, loc);

  ED_view3d_win_to_delta(vc.region, screen_dir, zfac, r_local_dir);
  normalize_v3(r_local_dir);

  add_v3_v3(r_local_dir, ob.loc);
  mul_m4_v3(ob.world_to_object().ptr(), r_local_dir);
}

static void calc_brush_local_mat(const float rotation,
                                 const Object &ob,
                                 float local_mat[4][4],
                                 float local_mat_inv[4][4])
{
  const StrokeCache *cache = ob.sculpt->cache;
  float tmat[4][4];
  float mat[4][4];
  float scale[4][4];
  float angle, v[3];

  /* Ensure `ob.world_to_object` is up to date. */
  invert_m4_m4(ob.runtime->world_to_object.ptr(), ob.object_to_world().ptr());

  /* Initialize last column of matrix. */
  mat[0][3] = 0.0f;
  mat[1][3] = 0.0f;
  mat[2][3] = 0.0f;
  mat[3][3] = 1.0f;

  /* Read rotation (user angle, rake, etc.) to find the view's movement direction (negative X of
   * the brush). */
  angle = rotation + cache->special_rotation;
  /* By convention, motion direction points down the brush's Y axis, the angle represents the X
   * axis, normal is a 90 deg CCW rotation of the motion direction. */
  float motion_normal_screen[2];
  motion_normal_screen[0] = cosf(angle);
  motion_normal_screen[1] = sinf(angle);
  /* Convert view's brush transverse direction to object-space,
   * i.e. the normal of the plane described by the motion */
  float motion_normal_local[3];
  calc_local_from_screen(*cache->vc, cache->location, motion_normal_screen, motion_normal_local);

  /* Calculate the movement direction for the local matrix.
   * Note that there is a deliberate prioritization here: Our calculations are
   * designed such that the _motion vector_ gets projected into the tangent space;
   * in most cases this will be more intuitive than projecting the transverse
   * direction (which is orthogonal to the motion direction and therefore less
   * apparent to the user).
   * The Y-axis of the brush-local frame has to lie in the intersection of the tangent plane
   * and the motion plane. */

  cross_v3_v3v3(v, cache->sculpt_normal, motion_normal_local);
  normalize_v3_v3(mat[1], v);

  /* Get other axes. */
  cross_v3_v3v3(mat[0], mat[1], cache->sculpt_normal);
  copy_v3_v3(mat[2], cache->sculpt_normal);

  /* Set location. */
  copy_v3_v3(mat[3], cache->location);

  /* Scale by brush radius. */
  float radius = cache->radius;

  normalize_m4(mat);
  scale_m4_fl(scale, radius);
  mul_m4_m4m4(tmat, mat, scale);

  /* Return tmat as is (for converting from local area coords to model-space coords). */
  copy_m4_m4(local_mat_inv, tmat);
  /* Return inverse (for converting from model-space coords to local area coords). */
  invert_m4_m4(local_mat, tmat);
}

}  // namespace blender::ed::sculpt_paint

#define SCULPT_TILT_SENSITIVITY 0.7f
void SCULPT_tilt_apply_to_normal(float r_normal[3],
                                 blender::ed::sculpt_paint::StrokeCache *cache,
                                 const float tilt_strength)
{
  if (!U.experimental.use_sculpt_tools_tilt) {
    return;
  }
  const float rot_max = M_PI_2 * tilt_strength * SCULPT_TILT_SENSITIVITY;
  mul_v3_mat3_m4v3(r_normal, cache->vc->obact->object_to_world().ptr(), r_normal);
  float normal_tilt_y[3];
  rotate_v3_v3v3fl(normal_tilt_y, r_normal, cache->vc->rv3d->viewinv[0], cache->y_tilt * rot_max);
  float normal_tilt_xy[3];
  rotate_v3_v3v3fl(
      normal_tilt_xy, normal_tilt_y, cache->vc->rv3d->viewinv[1], cache->x_tilt * rot_max);
  mul_v3_mat3_m4v3(r_normal, cache->vc->obact->world_to_object().ptr(), normal_tilt_xy);
  normalize_v3(r_normal);
}

void SCULPT_tilt_effective_normal_get(const SculptSession &ss, const Brush &brush, float r_no[3])
{
  copy_v3_v3(r_no, ss.cache->sculpt_normal_symm);
  SCULPT_tilt_apply_to_normal(r_no, ss.cache, brush.tilt_strength_factor);
}

static void update_brush_local_mat(const Sculpt &sd, Object &ob)
{
  using namespace blender::ed::sculpt_paint;
  StrokeCache *cache = ob.sculpt->cache;

  if (cache->mirror_symmetry_pass == 0 && cache->radial_symmetry_pass == 0) {
    const Brush *brush = BKE_paint_brush_for_read(&sd.paint);
    const MTex *mask_tex = BKE_brush_mask_texture_get(brush, OB_MODE_SCULPT);
    calc_brush_local_mat(
        mask_tex->rot, ob, cache->brush_local_mat.ptr(), cache->brush_local_mat_inv.ptr());
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Texture painting
 * \{ */

static bool sculpt_needs_pbvh_pixels(PaintModeSettings &paint_mode_settings,
                                     const Brush &brush,
                                     Object &ob)
{
  if (brush.sculpt_tool == SCULPT_TOOL_PAINT && U.experimental.use_sculpt_texture_paint) {
    Image *image;
    ImageUser *image_user;
    return SCULPT_paint_image_canvas_get(paint_mode_settings, ob, &image, &image_user);
  }

  return false;
}

static void sculpt_pbvh_update_pixels(PaintModeSettings &paint_mode_settings,
                                      SculptSession &ss,
                                      Object &ob)
{
  using namespace blender;
  BLI_assert(ob.type == OB_MESH);
  Mesh *mesh = (Mesh *)ob.data;

  Image *image;
  ImageUser *image_user;
  if (!SCULPT_paint_image_canvas_get(paint_mode_settings, ob, &image, &image_user)) {
    return;
  }

  bke::pbvh::build_pixels(*ss.pbvh, mesh, image, image_user);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Generic Brush Plane & Symmetry Utilities
 * \{ */

struct SculptRaycastData {
  SculptSession *ss;
  const float *ray_start;
  const float *ray_normal;
  bool hit;
  float depth;
  bool original;
  Span<int> corner_verts;
  Span<blender::int3> corner_tris;
  Span<int> corner_tri_faces;
  blender::VArraySpan<bool> hide_poly;

  PBVHVertRef active_vertex;
  float *face_normal;

  int active_face_grid_index;

  IsectRayPrecalc isect_precalc;
};

struct SculptFindNearestToRayData {
  SculptSession *ss;
  const float *ray_start, *ray_normal;
  bool hit;
  float depth;
  float dist_sq_to_ray;
  bool original;
  Span<int> corner_verts;
  Span<blender::int3> corner_tris;
  Span<int> corner_tri_faces;
  blender::VArraySpan<bool> hide_poly;
};

ePaintSymmetryAreas SCULPT_get_vertex_symm_area(const float co[3])
{
  ePaintSymmetryAreas symm_area = ePaintSymmetryAreas(PAINT_SYMM_AREA_DEFAULT);
  if (co[0] < 0.0f) {
    symm_area |= PAINT_SYMM_AREA_X;
  }
  if (co[1] < 0.0f) {
    symm_area |= PAINT_SYMM_AREA_Y;
  }
  if (co[2] < 0.0f) {
    symm_area |= PAINT_SYMM_AREA_Z;
  }
  return symm_area;
}

static void flip_qt_qt(float out[4], const float in[4], const ePaintSymmetryFlags symm)
{
  float axis[3], angle;

  quat_to_axis_angle(axis, &angle, in);
  normalize_v3(axis);

  if (symm & PAINT_SYMM_X) {
    axis[0] *= -1.0f;
    angle *= -1.0f;
  }
  if (symm & PAINT_SYMM_Y) {
    axis[1] *= -1.0f;
    angle *= -1.0f;
  }
  if (symm & PAINT_SYMM_Z) {
    axis[2] *= -1.0f;
    angle *= -1.0f;
  }

  axis_angle_normalized_to_quat(out, axis, angle);
}

static void flip_qt(float quat[4], const ePaintSymmetryFlags symm)
{
  flip_qt_qt(quat, quat, symm);
}

float3 SCULPT_flip_v3_by_symm_area(const float3 &vector,
                                   const ePaintSymmetryFlags symm,
                                   const ePaintSymmetryAreas symmarea,
                                   const float3 &pivot)
{
  float3 result = vector;
  for (int i = 0; i < 3; i++) {
    ePaintSymmetryFlags symm_it = ePaintSymmetryFlags(1 << i);
    if (!(symm & symm_it)) {
      continue;
    }
    if (symmarea & symm_it) {
      result = blender::ed::sculpt_paint::symmetry_flip(result, symm_it);
    }
    if (pivot[i] < 0.0f) {
      result = blender::ed::sculpt_paint::symmetry_flip(result, symm_it);
    }
  }
  return result;
}

void SCULPT_flip_quat_by_symm_area(float quat[4],
                                   const ePaintSymmetryFlags symm,
                                   const ePaintSymmetryAreas symmarea,
                                   const float pivot[3])
{
  for (int i = 0; i < 3; i++) {
    ePaintSymmetryFlags symm_it = ePaintSymmetryFlags(1 << i);
    if (!(symm & symm_it)) {
      continue;
    }
    if (symmarea & symm_it) {
      flip_qt(quat, symm_it);
    }
    if (pivot[i] < 0.0f) {
      flip_qt(quat, symm_it);
    }
  }
}

bool SCULPT_tool_needs_all_pbvh_nodes(const Brush &brush)
{
  if (brush.sculpt_tool == SCULPT_TOOL_ELASTIC_DEFORM) {
    /* Elastic deformations in any brush need all nodes to avoid artifacts as the effect
     * of the Kelvinlet is not constrained by the radius. */
    return true;
  }

  if (brush.sculpt_tool == SCULPT_TOOL_POSE) {
    /* Pose needs all nodes because it applies all symmetry iterations at the same time
     * and the IK chain can grow to any area of the model. */
    /* TODO: This can be optimized by filtering the nodes after calculating the chain. */
    return true;
  }

  if (brush.sculpt_tool == SCULPT_TOOL_BOUNDARY) {
    /* Boundary needs all nodes because it is not possible to know where the boundary
     * deformation is going to be propagated before calculating it. */
    /* TODO: after calculating the boundary info in the first iteration, it should be
     * possible to get the nodes that have vertices included in any boundary deformation
     * and cache them. */
    return true;
  }

  if (brush.sculpt_tool == SCULPT_TOOL_SNAKE_HOOK &&
      brush.snake_hook_deform_type == BRUSH_SNAKE_HOOK_DEFORM_ELASTIC)
  {
    /* Snake hook in elastic deform type has same requirements as the elastic deform tool. */
    return true;
  }
  return false;
}

namespace blender::ed::sculpt_paint {

void calc_brush_plane(const Brush &brush,
                      Object &ob,
                      Span<bke::pbvh::Node *> nodes,
                      float3 &r_area_no,
                      float3 &r_area_co)
{
  const SculptSession &ss = *ob.sculpt;

  zero_v3(r_area_co);
  zero_v3(r_area_no);

  if (SCULPT_stroke_is_main_symmetry_pass(*ss.cache) &&
      (SCULPT_stroke_is_first_brush_step_of_symmetry_pass(*ss.cache) ||
       !(brush.flag & BRUSH_ORIGINAL_PLANE) || !(brush.flag & BRUSH_ORIGINAL_NORMAL)))
  {
    switch (brush.sculpt_plane) {
      case SCULPT_DISP_DIR_VIEW:
        copy_v3_v3(r_area_no, ss.cache->true_view_normal);
        break;

      case SCULPT_DISP_DIR_X:
        ARRAY_SET_ITEMS(r_area_no, 1.0f, 0.0f, 0.0f);
        break;

      case SCULPT_DISP_DIR_Y:
        ARRAY_SET_ITEMS(r_area_no, 0.0f, 1.0f, 0.0f);
        break;

      case SCULPT_DISP_DIR_Z:
        ARRAY_SET_ITEMS(r_area_no, 0.0f, 0.0f, 1.0f);
        break;

      case SCULPT_DISP_DIR_AREA:
        calc_area_normal_and_center(brush, ob, nodes, r_area_no, r_area_co);
        if (brush.falloff_shape == PAINT_FALLOFF_SHAPE_TUBE) {
          project_plane_v3_v3v3(r_area_no, r_area_no, ss.cache->view_normal);
          normalize_v3(r_area_no);
        }
        break;

      default:
        break;
    }

    /* For flatten center. */
    /* Flatten center has not been calculated yet if we are not using the area normal. */
    if (brush.sculpt_plane != SCULPT_DISP_DIR_AREA) {
      calc_area_center(brush, ob, nodes, r_area_co);
    }

    /* For area normal. */
    if (!SCULPT_stroke_is_first_brush_step_of_symmetry_pass(*ss.cache) &&
        (brush.flag & BRUSH_ORIGINAL_NORMAL))
    {
      copy_v3_v3(r_area_no, ss.cache->sculpt_normal);
    }
    else {
      copy_v3_v3(ss.cache->sculpt_normal, r_area_no);
    }

    /* For flatten center. */
    if (!SCULPT_stroke_is_first_brush_step_of_symmetry_pass(*ss.cache) &&
        (brush.flag & BRUSH_ORIGINAL_PLANE))
    {
      copy_v3_v3(r_area_co, ss.cache->last_center);
    }
    else {
      copy_v3_v3(ss.cache->last_center, r_area_co);
    }
  }
  else {
    /* For area normal. */
    copy_v3_v3(r_area_no, ss.cache->sculpt_normal);

    /* For flatten center. */
    copy_v3_v3(r_area_co, ss.cache->last_center);

    /* For area normal. */
    r_area_no = symmetry_flip(r_area_no, ss.cache->mirror_symmetry_pass);

    /* For flatten center. */
    r_area_co = symmetry_flip(r_area_co, ss.cache->mirror_symmetry_pass);

    /* For area normal. */
    mul_m4_v3(ss.cache->symm_rot_mat.ptr(), r_area_no);

    /* For flatten center. */
    mul_m4_v3(ss.cache->symm_rot_mat.ptr(), r_area_co);

    /* Shift the plane for the current tile. */
    add_v3_v3(r_area_co, ss.cache->plane_offset);
  }
}

}  // namespace blender::ed::sculpt_paint

float SCULPT_brush_plane_offset_get(const Sculpt &sd, const SculptSession &ss)
{
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);

  float rv = brush.plane_offset;

  if (brush.flag & BRUSH_OFFSET_PRESSURE) {
    rv *= ss.cache->pressure;
  }

  return rv;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Sculpt Brush Utilities
 * \{ */

void SCULPT_vertcos_to_key(Object &ob, KeyBlock *kb, const Span<float3> vertCos)
{
  Mesh *mesh = (Mesh *)ob.data;
  float(*ofs)[3] = nullptr;
  int a, currkey_i;
  const int kb_act_idx = ob.shapenr - 1;

  /* For relative keys editing of base should update other keys. */
  if (bool *dependent = BKE_keyblock_get_dependent_keys(mesh->key, kb_act_idx)) {
    ofs = BKE_keyblock_convert_to_vertcos(&ob, kb);

    /* Calculate key coord offsets (from previous location). */
    for (a = 0; a < mesh->verts_num; a++) {
      sub_v3_v3v3(ofs[a], vertCos[a], ofs[a]);
    }

    /* Apply offsets on other keys. */
    LISTBASE_FOREACH_INDEX (KeyBlock *, currkey, &mesh->key->block, currkey_i) {
      if ((currkey != kb) && dependent[currkey_i]) {
        BKE_keyblock_update_from_offset(&ob, currkey, ofs);
      }
    }

    MEM_freeN(ofs);
    MEM_freeN(dependent);
  }

  /* Modifying of basis key should update mesh. */
  if (kb == mesh->key->refkey) {
    mesh->vert_positions_for_write().copy_from(vertCos);
    mesh->tag_positions_changed();
  }

  /* Apply new coords on active key block, no need to re-allocate kb->data here! */
  BKE_keyblock_update_from_vertcos(&ob, kb, reinterpret_cast<const float(*)[3]>(vertCos.data()));
}

namespace blender::ed::sculpt_paint {

static void sculpt_topology_update(const Scene & /*scene*/,
                                   const Sculpt &sd,
                                   Object &ob,
                                   const Brush &brush,
                                   UnifiedPaintSettings & /*ups*/,
                                   PaintModeSettings & /*paint_mode_settings*/)
{
  SculptSession &ss = *ob.sculpt;

  /* Build a list of all nodes that are potentially within the brush's area of influence. */
  const bool use_original = sculpt_tool_needs_original(brush.sculpt_tool) ? true :
                                                                            !ss.cache->accum;
  const float radius_scale = 1.25f;
  Vector<bke::pbvh::Node *> nodes = sculpt_pbvh_gather_generic(
      ob, brush, use_original, radius_scale);

  /* Only act if some verts are inside the brush area. */
  if (nodes.is_empty()) {
    return;
  }

  /* Free index based vertex info as it will become invalid after modifying the topology during the
   * stroke. */
  ss.vertex_info.boundary.clear();

  PBVHTopologyUpdateMode mode = PBVHTopologyUpdateMode(0);
  float location[3];

  if (!(sd.flags & SCULPT_DYNTOPO_DETAIL_MANUAL)) {
    if (sd.flags & SCULPT_DYNTOPO_SUBDIVIDE) {
      mode |= PBVH_Subdivide;
    }

    if ((sd.flags & SCULPT_DYNTOPO_COLLAPSE) || (brush.sculpt_tool == SCULPT_TOOL_SIMPLIFY)) {
      mode |= PBVH_Collapse;
    }
  }

  for (bke::pbvh::Node *node : nodes) {
    undo::push_node(
        ob, node, brush.sculpt_tool == SCULPT_TOOL_MASK ? undo::Type::Mask : undo::Type::Position);
    BKE_pbvh_node_mark_update(node);

    BKE_pbvh_node_mark_topology_update(node);
    BKE_pbvh_bmesh_node_save_orig(ss.bm, ss.bm_log, node, false);
  }

  bke::pbvh::bmesh_update_topology(*ss.pbvh,
                                   *ss.bm_log,
                                   mode,
                                   ss.cache->location,
                                   ss.cache->view_normal,
                                   ss.cache->radius,
                                   (brush.flag & BRUSH_FRONTFACE) != 0,
                                   (brush.falloff_shape != PAINT_FALLOFF_SHAPE_SPHERE));

  /* Update average stroke position. */
  copy_v3_v3(location, ss.cache->true_location);
  mul_m4_v3(ob.object_to_world().ptr(), location);
}

static void push_undo_nodes(Object &ob, const Brush &brush, const Span<bke::pbvh::Node *> nodes)
{
  SculptSession &ss = *ob.sculpt;

  bool need_coords = ss.cache->supports_gravity;

  if (brush.sculpt_tool == SCULPT_TOOL_DRAW_FACE_SETS) {
    for (bke::pbvh::Node *node : nodes) {
      BKE_pbvh_node_mark_update_face_sets(node);
    }

    /* Draw face sets in smooth mode moves the vertices. */
    if (ss.cache->alt_smooth) {
      need_coords = true;
    }
    else {
      undo::push_nodes(ob, nodes, undo::Type::FaceSet);
    }
  }
  else if (brush.sculpt_tool == SCULPT_TOOL_MASK) {
    undo::push_nodes(ob, nodes, undo::Type::Mask);
    for (bke::pbvh::Node *node : nodes) {
      BKE_pbvh_node_mark_update_mask(node);
    }
  }
  else if (SCULPT_tool_is_paint(brush.sculpt_tool)) {
    const Mesh &mesh = *static_cast<const Mesh *>(ob.data);
    if (const bke::GAttributeReader attr = color::active_color_attribute(mesh)) {
      if (attr.domain == bke::AttrDomain::Corner) {
        BKE_pbvh_ensure_node_loops(*ss.pbvh, mesh.corner_tris());
      }
    }
    undo::push_nodes(ob, nodes, undo::Type::Color);
    for (bke::pbvh::Node *node : nodes) {
      BKE_pbvh_node_mark_update_color(node);
    }
  }
  else {
    need_coords = true;
  }

  if (need_coords) {
    undo::push_nodes(ob, nodes, undo::Type::Position);
    for (bke::pbvh::Node *node : nodes) {
      BKE_pbvh_node_mark_positions_update(node);
    }
  }
}

static void do_brush_action(const Scene &scene,
                            const Sculpt &sd,
                            Object &ob,
                            const Brush &brush,
                            UnifiedPaintSettings &ups,
                            PaintModeSettings &paint_mode_settings)
{
  SculptSession &ss = *ob.sculpt;
  Vector<bke::pbvh::Node *> nodes, texnodes;

  const bool use_original = sculpt_tool_needs_original(brush.sculpt_tool) ? true :
                                                                            !ss.cache->accum;
  const bool use_pixels = sculpt_needs_pbvh_pixels(paint_mode_settings, brush, ob);

  if (sculpt_needs_pbvh_pixels(paint_mode_settings, brush, ob)) {
    sculpt_pbvh_update_pixels(paint_mode_settings, ss, ob);

    texnodes = sculpt_pbvh_gather_texpaint(ob, brush, use_original, 1.0f);

    if (texnodes.is_empty()) {
      return;
    }
  }

  /* Build a list of all nodes that are potentially within the brush's area of influence */

  if (SCULPT_tool_needs_all_pbvh_nodes(brush)) {
    /* These brushes need to update all nodes as they are not constrained by the brush radius */
    nodes = bke::pbvh::search_gather(*ss.pbvh, {});
  }
  else if (brush.sculpt_tool == SCULPT_TOOL_CLOTH) {
    nodes = cloth::brush_affected_nodes_gather(ss, brush);
  }
  else {
    float radius_scale = 1.0f;

    /* Corners of square brushes can go outside the brush radius. */
    if (BKE_brush_has_cube_tip(&brush, PaintMode::Sculpt)) {
      radius_scale = M_SQRT2;
    }

    /* With these options enabled not all required nodes are inside the original brush radius, so
     * the brush can produce artifacts in some situations. */
    if (brush.sculpt_tool == SCULPT_TOOL_DRAW && brush.flag & BRUSH_ORIGINAL_NORMAL) {
      radius_scale = 2.0f;
    }
    nodes = sculpt_pbvh_gather_generic(ob, brush, use_original, radius_scale);
  }

  /* Draw Face Sets in draw mode makes a single undo push, in alt-smooth mode deforms the
   * vertices and uses regular coords undo. */
  /* It also assigns the paint_face_set here as it needs to be done regardless of the stroke type
   * and the number of nodes under the brush influence. */
  if (brush.sculpt_tool == SCULPT_TOOL_DRAW_FACE_SETS &&
      SCULPT_stroke_is_first_brush_step(*ss.cache) && !ss.cache->alt_smooth)
  {
    if (ss.cache->invert) {
      /* When inverting the brush, pick the paint face mask ID from the mesh. */
      ss.cache->paint_face_set = face_set::active_face_set_get(ss);
    }
    else {
      /* By default create a new Face Sets. */
      ss.cache->paint_face_set = face_set::find_next_available_id(ob);
    }
  }

  /* For anchored brushes with spherical falloff, we start off with zero radius, thus we have no
   * bke::pbvh::Tree nodes on the first brush step. */
  if (!nodes.is_empty() ||
      ((brush.falloff_shape == PAINT_FALLOFF_SHAPE_SPHERE) && (brush.flag & BRUSH_ANCHORED)))
  {
    if (SCULPT_stroke_is_first_brush_step(*ss.cache)) {
      /* Initialize auto-masking cache. */
      if (auto_mask::is_enabled(sd, &ss, &brush)) {
        ss.cache->automasking = auto_mask::cache_init(sd, &brush, ob);
        ss.last_automasking_settings_hash = auto_mask::settings_hash(ob, *ss.cache->automasking);
      }
      /* Initialize surface smooth cache. */
      if ((brush.sculpt_tool == SCULPT_TOOL_SMOOTH) &&
          (brush.smooth_deform_type == BRUSH_SMOOTH_DEFORM_SURFACE))
      {
        BLI_assert(ss.cache->surface_smooth_laplacian_disp.is_empty());
        ss.cache->surface_smooth_laplacian_disp = Array<float3>(SCULPT_vertex_count_get(ss),
                                                                float3(0));
      }
    }
  }

  /* Only act if some verts are inside the brush area. */
  if (nodes.is_empty()) {
    return;
  }
  float location[3];

  if (!use_pixels) {
    push_undo_nodes(ob, brush, nodes);
  }

  if (sculpt_brush_needs_normal(ss, sd, brush)) {
    update_sculpt_normal(sd, ob, nodes);
  }

  update_brush_local_mat(sd, ob);

  if (brush.sculpt_tool == SCULPT_TOOL_POSE && SCULPT_stroke_is_first_brush_step(*ss.cache)) {
    pose::pose_brush_init(ob, ss, brush);
  }

  if (brush.deform_target == BRUSH_DEFORM_TARGET_CLOTH_SIM) {
    if (!ss.cache->cloth_sim) {
      ss.cache->cloth_sim = cloth::brush_simulation_create(ob, 1.0f, 0.0f, 0.0f, false, true);
    }
    cloth::brush_store_simulation_state(ss, *ss.cache->cloth_sim);
    cloth::ensure_nodes_constraints(
        sd, ob, nodes, *ss.cache->cloth_sim, ss.cache->location, FLT_MAX);
  }

  bool invert = ss.cache->pen_flip || ss.cache->invert;
  if (brush.flag & BRUSH_DIR_IN) {
    invert = !invert;
  }

  /* Apply one type of brush action. */
  switch (brush.sculpt_tool) {
    case SCULPT_TOOL_DRAW: {
      const bool use_vector_displacement = (brush.flag2 & BRUSH_USE_COLOR_AS_DISPLACEMENT &&
                                            (brush.mtex.brush_map_mode == MTEX_MAP_MODE_AREA));
      if (use_vector_displacement) {
        do_draw_vector_displacement_brush(sd, ob, nodes);
      }
      else {
        do_draw_brush(sd, ob, nodes);
      }
      break;
    }
    case SCULPT_TOOL_SMOOTH:
      if (brush.smooth_deform_type == BRUSH_SMOOTH_DEFORM_LAPLACIAN) {
        /* NOTE: The enhance brush needs to initialize its state on the first brush step. The
         * stroke strength can become 0 during the stroke, but it can not change sign (the sign is
         * determined in the beginning of the stroke. So here it is important to not switch to
         * enhance brush in the middle of the stroke. */
        if (ss.cache->bstrength < 0.0f) {
          /* Invert mode, intensify details. */
          do_enhance_details_brush(sd, ob, nodes);
        }
        else {
          do_smooth_brush(sd, ob, nodes, std::clamp(ss.cache->bstrength, 0.0f, 1.0f));
        }
      }
      else if (brush.smooth_deform_type == BRUSH_SMOOTH_DEFORM_SURFACE) {
        do_surface_smooth_brush(sd, ob, nodes);
      }
      break;
    case SCULPT_TOOL_CREASE:
      do_crease_brush(scene, sd, ob, nodes);
      break;
    case SCULPT_TOOL_BLOB:
      do_blob_brush(scene, sd, ob, nodes);
      break;
    case SCULPT_TOOL_PINCH:
      do_pinch_brush(sd, ob, nodes);
      break;
    case SCULPT_TOOL_INFLATE:
      do_inflate_brush(sd, ob, nodes);
      break;
    case SCULPT_TOOL_GRAB:
      do_grab_brush(sd, ob, nodes);
      break;
    case SCULPT_TOOL_ROTATE:
      do_rotate_brush(sd, ob, nodes);
      break;
    case SCULPT_TOOL_SNAKE_HOOK:
      do_snake_hook_brush(sd, ob, nodes);
      break;
    case SCULPT_TOOL_NUDGE:
      do_nudge_brush(sd, ob, nodes);
      break;
    case SCULPT_TOOL_THUMB:
      do_thumb_brush(sd, ob, nodes);
      break;
    case SCULPT_TOOL_LAYER:
      do_layer_brush(sd, ob, nodes);
      break;
    case SCULPT_TOOL_FLATTEN:
      do_flatten_brush(sd, ob, nodes);
      break;
    case SCULPT_TOOL_CLAY:
      do_clay_brush(sd, ob, nodes);
      break;
    case SCULPT_TOOL_CLAY_STRIPS:
      do_clay_strips_brush(sd, ob, nodes);
      break;
    case SCULPT_TOOL_MULTIPLANE_SCRAPE:
      do_multiplane_scrape_brush(sd, ob, nodes);
      break;
    case SCULPT_TOOL_CLAY_THUMB:
      do_clay_thumb_brush(sd, ob, nodes);
      break;
    case SCULPT_TOOL_FILL:
      if (invert && brush.flag & BRUSH_INVERT_TO_SCRAPE_FILL) {
        do_scrape_brush(sd, ob, nodes);
      }
      else {
        do_fill_brush(sd, ob, nodes);
      }
      break;
    case SCULPT_TOOL_SCRAPE:
      if (invert && brush.flag & BRUSH_INVERT_TO_SCRAPE_FILL) {
        do_fill_brush(sd, ob, nodes);
      }
      else {
        do_scrape_brush(sd, ob, nodes);
      }
      break;
    case SCULPT_TOOL_MASK:
      switch ((BrushMaskTool)brush.mask_tool) {
        case BRUSH_MASK_DRAW:
          do_mask_brush(sd, ob, nodes);
          break;
        case BRUSH_MASK_SMOOTH:
          do_smooth_mask_brush(sd, ob, nodes, ss.cache->bstrength);
          break;
      }
      break;
    case SCULPT_TOOL_POSE:
      pose::do_pose_brush(sd, ob, nodes);
      break;
    case SCULPT_TOOL_DRAW_SHARP:
      do_draw_sharp_brush(sd, ob, nodes);
      break;
    case SCULPT_TOOL_ELASTIC_DEFORM:
      do_elastic_deform_brush(sd, ob, nodes);
      break;
    case SCULPT_TOOL_SLIDE_RELAX:
      if (ss.cache->alt_smooth) {
        do_topology_relax_brush(sd, ob, nodes);
      }
      else {
        do_topology_slide_brush(sd, ob, nodes);
      }
      break;
    case SCULPT_TOOL_BOUNDARY:
      boundary::do_boundary_brush(sd, ob, nodes);
      break;
    case SCULPT_TOOL_CLOTH:
      cloth::do_cloth_brush(sd, ob, nodes);
      break;
    case SCULPT_TOOL_DRAW_FACE_SETS:
      if (!ss.cache->alt_smooth) {
        do_draw_face_sets_brush(sd, ob, nodes);
      }
      else {
        do_relax_face_sets_brush(sd, ob, nodes);
      }
      break;
    case SCULPT_TOOL_DISPLACEMENT_ERASER:
      do_displacement_eraser_brush(sd, ob, nodes);
      break;
    case SCULPT_TOOL_DISPLACEMENT_SMEAR:
      do_displacement_smear_brush(sd, ob, nodes);
      break;
    case SCULPT_TOOL_PAINT:
      color::do_paint_brush(paint_mode_settings, sd, ob, nodes, texnodes);
      break;
    case SCULPT_TOOL_SMEAR:
      color::do_smear_brush(sd, ob, nodes);
      break;
  }

  if (!ELEM(brush.sculpt_tool, SCULPT_TOOL_SMOOTH, SCULPT_TOOL_MASK) &&
      brush.autosmooth_factor > 0)
  {
    if (brush.flag & BRUSH_INVERSE_SMOOTH_PRESSURE) {
      do_smooth_brush(sd, ob, nodes, brush.autosmooth_factor * (1.0f - ss.cache->pressure));
    }
    else {
      do_smooth_brush(sd, ob, nodes, brush.autosmooth_factor);
    }
  }

  if (sculpt_brush_use_topology_rake(ss, brush)) {
    do_bmesh_topology_rake_brush(sd, ob, nodes, brush.topology_rake_factor);
  }

  if (!auto_mask::tool_can_reuse_automask(brush.sculpt_tool) ||
      (ss.cache->supports_gravity && sd.gravity_factor > 0.0f))
  {
    /* Clear cavity mask cache. */
    ss.last_automasking_settings_hash = 0;
  }

  /* The cloth brush adds the gravity as a regular force and it is processed in the solver. */
  if (ss.cache->supports_gravity &&
      !ELEM(
          brush.sculpt_tool, SCULPT_TOOL_CLOTH, SCULPT_TOOL_DRAW_FACE_SETS, SCULPT_TOOL_BOUNDARY))
  {
    do_gravity_brush(sd, ob, nodes);
  }

  if (brush.deform_target == BRUSH_DEFORM_TARGET_CLOTH_SIM) {
    if (SCULPT_stroke_is_main_symmetry_pass(*ss.cache)) {
      cloth::sim_activate_nodes(*ss.cache->cloth_sim, nodes);
      cloth::do_simulation_step(sd, ob, *ss.cache->cloth_sim, nodes);
    }
  }

  /* Update average stroke position. */
  copy_v3_v3(location, ss.cache->true_location);
  mul_m4_v3(ob.object_to_world().ptr(), location);

  add_v3_v3(ups.average_stroke_accum, location);
  ups.average_stroke_counter++;
  /* Update last stroke position. */
  ups.last_stroke_valid = true;
}

/* Flush displacement from deformed bke::pbvh::Tree vertex to original mesh. */
static void sculpt_flush_pbvhvert_deform(SculptSession &ss,
                                         const PBVHVertexIter &vd,
                                         MutableSpan<float3> positions)
{
  float disp[3], newco[3];
  int index = vd.vert_indices[vd.i];

  sub_v3_v3v3(disp, vd.co, ss.deform_cos[index]);
  mul_m3_v3(ss.deform_imats[index].ptr(), disp);
  add_v3_v3v3(newco, disp, ss.orig_cos[index]);

  ss.deform_cos[index] = vd.co;
  ss.orig_cos[index] = newco;

  if (!ss.shapekey_active) {
    copy_v3_v3(positions[index], newco);
  }
}

}  // namespace blender::ed::sculpt_paint

/**
 * Copy the modified vertices from the #bke::pbvh::Tree to the active key.
 */
static void sculpt_update_keyblock(Object &ob)
{
  SculptSession &ss = *ob.sculpt;

  /* Key-block update happens after handling deformation caused by modifiers,
   * so ss.orig_cos would be updated with new stroke. */
  if (!ss.orig_cos.is_empty()) {
    SCULPT_vertcos_to_key(ob, ss.shapekey_active, ss.orig_cos);
  }
  else {
    SCULPT_vertcos_to_key(ob, ss.shapekey_active, BKE_pbvh_get_vert_positions(*ss.pbvh));
  }
}

void SCULPT_flush_stroke_deform(const Sculpt & /*sd*/, Object &ob, bool is_proxy_used)
{
  using namespace blender;
  using namespace blender::ed::sculpt_paint;
  SculptSession &ss = *ob.sculpt;

  if (is_proxy_used && ss.deform_modifiers_active) {
    /* This brushes aren't using proxies, so sculpt_combine_proxies() wouldn't propagate needed
     * deformation to original base. */

    Mesh *mesh = (Mesh *)ob.data;
    Vector<bke::pbvh::Node *> nodes;
    Array<float3> vertCos;

    if (ss.shapekey_active) {
      /* Mesh could have isolated verts which wouldn't be in BVH, to deal with this we copy old
       * coordinates over new ones and then update coordinates for all vertices from BVH. */
      vertCos = ss.orig_cos;
    }

    nodes = bke::pbvh::search_gather(*ss.pbvh, {});

    MutableSpan<float3> positions = mesh->vert_positions_for_write();

    threading::parallel_for(nodes.index_range(), 1, [&](IndexRange range) {
      for (const int i : range) {
        PBVHVertexIter vd;
        BKE_pbvh_vertex_iter_begin (*ss.pbvh, nodes[i], vd, PBVH_ITER_UNIQUE) {
          sculpt_flush_pbvhvert_deform(ss, vd, positions);

          if (vertCos.is_empty()) {
            continue;
          }

          int index = vd.vert_indices[vd.i];
          copy_v3_v3(vertCos[index], ss.orig_cos[index]);
        }
        BKE_pbvh_vertex_iter_end;
      }
    });

    if (!vertCos.is_empty()) {
      SCULPT_vertcos_to_key(ob, ss.shapekey_active, vertCos);
    }
  }
  else if (ss.shapekey_active) {
    sculpt_update_keyblock(ob);
  }
}

void SCULPT_cache_calc_brushdata_symm(blender::ed::sculpt_paint::StrokeCache &cache,
                                      const ePaintSymmetryFlags symm,
                                      const char axis,
                                      const float angle)
{
  using namespace blender;
  cache.location = ed::sculpt_paint::symmetry_flip(cache.true_location, symm);
  cache.last_location = ed::sculpt_paint::symmetry_flip(cache.true_last_location, symm);
  cache.grab_delta_symmetry = ed::sculpt_paint::symmetry_flip(cache.grab_delta, symm);
  cache.view_normal = ed::sculpt_paint::symmetry_flip(cache.true_view_normal, symm);

  cache.initial_location = ed::sculpt_paint::symmetry_flip(cache.true_initial_location, symm);
  cache.initial_normal = ed::sculpt_paint::symmetry_flip(cache.true_initial_normal, symm);

  /* XXX This reduces the length of the grab delta if it approaches the line of symmetry
   * XXX However, a different approach appears to be needed. */
#if 0
  if (sd->paint.symmetry_flags & PAINT_SYMMETRY_FEATHER) {
    float frac = 1.0f / max_overlap_count(sd);
    float reduce = (feather - frac) / (1.0f - frac);

    printf("feather: %f frac: %f reduce: %f\n", feather, frac, reduce);

    if (frac < 1.0f) {
      mul_v3_fl(cache.grab_delta_symmetry, reduce);
    }
  }
#endif

  cache.symm_rot_mat = float4x4::identity();
  cache.symm_rot_mat_inv = float4x4::identity();
  zero_v3(cache.plane_offset);

  /* Expects XYZ. */
  if (axis) {
    rotate_m4(cache.symm_rot_mat.ptr(), axis, angle);
    rotate_m4(cache.symm_rot_mat_inv.ptr(), axis, -angle);
  }

  mul_m4_v3(cache.symm_rot_mat.ptr(), cache.location);
  mul_m4_v3(cache.symm_rot_mat.ptr(), cache.grab_delta_symmetry);

  if (cache.supports_gravity) {
    cache.gravity_direction = ed::sculpt_paint::symmetry_flip(cache.true_gravity_direction, symm);
    mul_m4_v3(cache.symm_rot_mat.ptr(), cache.gravity_direction);
  }

  if (cache.is_rake_rotation_valid) {
    flip_qt_qt(cache.rake_rotation_symmetry, cache.rake_rotation, symm);
  }
}

namespace blender::ed::sculpt_paint {

using BrushActionFunc = void (*)(const Scene &scene,
                                 const Sculpt &sd,
                                 Object &ob,
                                 const Brush &brush,
                                 UnifiedPaintSettings &ups,
                                 PaintModeSettings &paint_mode_settings);

static void do_tiled(const Scene &scene,
                     const Sculpt &sd,
                     Object &ob,
                     const Brush &brush,
                     UnifiedPaintSettings &ups,
                     PaintModeSettings &paint_mode_settings,
                     const BrushActionFunc action)
{
  SculptSession &ss = *ob.sculpt;
  StrokeCache *cache = ss.cache;
  const float radius = cache->radius;
  const Bounds<float3> bb = *BKE_object_boundbox_get(&ob);
  const float *bbMin = bb.min;
  const float *bbMax = bb.max;
  const float *step = sd.paint.tile_offset;

  /* These are integer locations, for real location: multiply with step and add orgLoc.
   * So 0,0,0 is at orgLoc. */
  int start[3];
  int end[3];
  int cur[3];

  /* Position of the "prototype" stroke for tiling. */
  float orgLoc[3];
  float original_initial_location[3];
  copy_v3_v3(orgLoc, cache->location);
  copy_v3_v3(original_initial_location, cache->initial_location);

  for (int dim = 0; dim < 3; dim++) {
    if ((sd.paint.symmetry_flags & (PAINT_TILE_X << dim)) && step[dim] > 0) {
      start[dim] = (bbMin[dim] - orgLoc[dim] - radius) / step[dim];
      end[dim] = (bbMax[dim] - orgLoc[dim] + radius) / step[dim];
    }
    else {
      start[dim] = end[dim] = 0;
    }
  }

  /* First do the "un-tiled" position to initialize the stroke for this location. */
  cache->tile_pass = 0;
  action(scene, sd, ob, brush, ups, paint_mode_settings);

  /* Now do it for all the tiles. */
  copy_v3_v3_int(cur, start);
  for (cur[0] = start[0]; cur[0] <= end[0]; cur[0]++) {
    for (cur[1] = start[1]; cur[1] <= end[1]; cur[1]++) {
      for (cur[2] = start[2]; cur[2] <= end[2]; cur[2]++) {
        if (!cur[0] && !cur[1] && !cur[2]) {
          /* Skip tile at orgLoc, this was already handled before all others. */
          continue;
        }

        ++cache->tile_pass;

        for (int dim = 0; dim < 3; dim++) {
          cache->location[dim] = cur[dim] * step[dim] + orgLoc[dim];
          cache->plane_offset[dim] = cur[dim] * step[dim];
          cache->initial_location[dim] = cur[dim] * step[dim] + original_initial_location[dim];
        }
        action(scene, sd, ob, brush, ups, paint_mode_settings);
      }
    }
  }
}

static void do_radial_symmetry(const Scene &scene,
                               const Sculpt &sd,
                               Object &ob,
                               const Brush &brush,
                               UnifiedPaintSettings &ups,
                               PaintModeSettings &paint_mode_settings,
                               const BrushActionFunc action,
                               const ePaintSymmetryFlags symm,
                               const int axis,
                               const float /*feather*/)
{
  SculptSession &ss = *ob.sculpt;

  for (int i = 1; i < sd.radial_symm[axis - 'X']; i++) {
    const float angle = 2.0f * M_PI * i / sd.radial_symm[axis - 'X'];
    ss.cache->radial_symmetry_pass = i;
    SCULPT_cache_calc_brushdata_symm(*ss.cache, symm, axis, angle);
    do_tiled(scene, sd, ob, brush, ups, paint_mode_settings, action);
  }
}

/**
 * Noise texture gives different values for the same input coord; this
 * can tear a multi-resolution mesh during sculpting so do a stitch in this case.
 */
static void sculpt_fix_noise_tear(const Sculpt &sd, Object &ob)
{
  SculptSession &ss = *ob.sculpt;
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  const MTex *mtex = BKE_brush_mask_texture_get(&brush, OB_MODE_SCULPT);

  if (ss.multires.active && mtex->tex && mtex->tex->type == TEX_NOISE) {
    multires_stitch_grids(&ob);
  }
}

static void do_symmetrical_brush_actions(const Scene &scene,
                                         const Sculpt &sd,
                                         Object &ob,
                                         const BrushActionFunc action,
                                         UnifiedPaintSettings &ups,
                                         PaintModeSettings &paint_mode_settings)
{
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  SculptSession &ss = *ob.sculpt;
  StrokeCache &cache = *ss.cache;
  const char symm = SCULPT_mesh_symmetry_xyz_get(ob);

  float feather = calc_symmetry_feather(sd, *ss.cache);

  cache.bstrength = brush_strength(sd, cache, feather, ups, paint_mode_settings);
  cache.symmetry = symm;

  /* `symm` is a bit combination of XYZ -
   * 1 is mirror X; 2 is Y; 3 is XY; 4 is Z; 5 is XZ; 6 is YZ; 7 is XYZ */
  for (int i = 0; i <= symm; i++) {
    if (!SCULPT_is_symmetry_iteration_valid(i, symm)) {
      continue;
    }
    const ePaintSymmetryFlags symm = ePaintSymmetryFlags(i);
    cache.mirror_symmetry_pass = symm;
    cache.radial_symmetry_pass = 0;

    SCULPT_cache_calc_brushdata_symm(cache, symm, 0, 0);
    do_tiled(scene, sd, ob, brush, ups, paint_mode_settings, action);

    do_radial_symmetry(scene, sd, ob, brush, ups, paint_mode_settings, action, symm, 'X', feather);
    do_radial_symmetry(scene, sd, ob, brush, ups, paint_mode_settings, action, symm, 'Y', feather);
    do_radial_symmetry(scene, sd, ob, brush, ups, paint_mode_settings, action, symm, 'Z', feather);
  }
}

}  // namespace blender::ed::sculpt_paint

bool SCULPT_mode_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  return ob && ob->mode & OB_MODE_SCULPT;
}

bool SCULPT_mode_poll_view3d(bContext *C)
{
  using namespace blender::ed::sculpt_paint;
  return (SCULPT_mode_poll(C) && CTX_wm_region_view3d(C) && !ED_gpencil_session_active());
}

bool SCULPT_poll(bContext *C)
{
  using namespace blender::ed::sculpt_paint;
  return SCULPT_mode_poll(C) && blender::ed::sculpt_paint::paint_brush_tool_poll(C);
}

/**
 * While most non-brush tools in sculpt mode do not use the brush cursor, the trim tools
 * and the filter tools are expected to have the cursor visible so that some functionality is
 * easier to visually estimate.
 *
 * See: #122856
 */
static bool is_brush_related_tool(bContext *C)
{
  Paint *paint = BKE_paint_get_active_from_context(C);
  Object *ob = CTX_data_active_object(C);
  ScrArea *area = CTX_wm_area(C);
  ARegion *region = CTX_wm_region(C);

  if (paint && ob && BKE_paint_brush(paint) &&
      (area && ELEM(area->spacetype, SPACE_VIEW3D, SPACE_IMAGE)) &&
      (region && region->regiontype == RGN_TYPE_WINDOW))
  {
    bToolRef *tref = area->runtime.tool;
    if (tref && tref->runtime && tref->runtime->keymap[0]) {
      std::array<wmOperatorType *, 7> trim_operators = {
          WM_operatortype_find("SCULPT_OT_trim_box_gesture", false),
          WM_operatortype_find("SCULPT_OT_trim_lasso_gesture", false),
          WM_operatortype_find("SCULPT_OT_trim_line_gesture", false),
          WM_operatortype_find("SCULPT_OT_trim_polyline_gesture", false),
          WM_operatortype_find("SCULPT_OT_mesh_filter", false),
          WM_operatortype_find("SCULPT_OT_cloth_filter", false),
          WM_operatortype_find("SCULPT_OT_color_filter", false),
      };

      return std::any_of(trim_operators.begin(), trim_operators.end(), [tref](wmOperatorType *ot) {
        PointerRNA ptr;
        return WM_toolsystem_ref_properties_get_from_operator(tref, ot, &ptr);
      });
    }
  }
  return false;
}

bool SCULPT_brush_cursor_poll(bContext *C)
{
  using namespace blender::ed::sculpt_paint;
  return SCULPT_mode_poll(C) && (paint_brush_tool_poll(C) || is_brush_related_tool(C));
}

static const char *sculpt_tool_name(const Sculpt &sd)
{
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);

  switch (eBrushSculptTool(brush.sculpt_tool)) {
    case SCULPT_TOOL_DRAW:
      return "Draw Brush";
    case SCULPT_TOOL_SMOOTH:
      return "Smooth Brush";
    case SCULPT_TOOL_CREASE:
      return "Crease Brush";
    case SCULPT_TOOL_BLOB:
      return "Blob Brush";
    case SCULPT_TOOL_PINCH:
      return "Pinch Brush";
    case SCULPT_TOOL_INFLATE:
      return "Inflate Brush";
    case SCULPT_TOOL_GRAB:
      return "Grab Brush";
    case SCULPT_TOOL_NUDGE:
      return "Nudge Brush";
    case SCULPT_TOOL_THUMB:
      return "Thumb Brush";
    case SCULPT_TOOL_LAYER:
      return "Layer Brush";
    case SCULPT_TOOL_FLATTEN:
      return "Flatten Brush";
    case SCULPT_TOOL_CLAY:
      return "Clay Brush";
    case SCULPT_TOOL_CLAY_STRIPS:
      return "Clay Strips Brush";
    case SCULPT_TOOL_CLAY_THUMB:
      return "Clay Thumb Brush";
    case SCULPT_TOOL_FILL:
      return "Fill Brush";
    case SCULPT_TOOL_SCRAPE:
      return "Scrape Brush";
    case SCULPT_TOOL_SNAKE_HOOK:
      return "Snake Hook Brush";
    case SCULPT_TOOL_ROTATE:
      return "Rotate Brush";
    case SCULPT_TOOL_MASK:
      return "Mask Brush";
    case SCULPT_TOOL_SIMPLIFY:
      return "Simplify Brush";
    case SCULPT_TOOL_DRAW_SHARP:
      return "Draw Sharp Brush";
    case SCULPT_TOOL_ELASTIC_DEFORM:
      return "Elastic Deform Brush";
    case SCULPT_TOOL_POSE:
      return "Pose Brush";
    case SCULPT_TOOL_MULTIPLANE_SCRAPE:
      return "Multi-plane Scrape Brush";
    case SCULPT_TOOL_SLIDE_RELAX:
      return "Slide/Relax Brush";
    case SCULPT_TOOL_BOUNDARY:
      return "Boundary Brush";
    case SCULPT_TOOL_CLOTH:
      return "Cloth Brush";
    case SCULPT_TOOL_DRAW_FACE_SETS:
      return "Draw Face Sets";
    case SCULPT_TOOL_DISPLACEMENT_ERASER:
      return "Multires Displacement Eraser";
    case SCULPT_TOOL_DISPLACEMENT_SMEAR:
      return "Multires Displacement Smear";
    case SCULPT_TOOL_PAINT:
      return "Paint Brush";
    case SCULPT_TOOL_SMEAR:
      return "Smear Brush";
  }

  return "Sculpting";
}

namespace blender::ed::sculpt_paint {

StrokeCache::~StrokeCache()
{
  MEM_SAFE_FREE(this->dial);
}

}  // namespace blender::ed::sculpt_paint

namespace blender::ed::sculpt_paint {

/* Initialize mirror modifier clipping. */
static void sculpt_init_mirror_clipping(Object &ob, SculptSession &ss)
{
  ss.cache->clip_mirror_mtx = float4x4::identity();

  LISTBASE_FOREACH (ModifierData *, md, &ob.modifiers) {
    if (!(md->type == eModifierType_Mirror && (md->mode & eModifierMode_Realtime))) {
      continue;
    }
    MirrorModifierData *mmd = (MirrorModifierData *)md;

    if (!(mmd->flag & MOD_MIR_CLIPPING)) {
      continue;
    }
    /* Check each axis for mirroring. */
    for (int i = 0; i < 3; i++) {
      if (!(mmd->flag & (MOD_MIR_AXIS_X << i))) {
        continue;
      }
      /* Enable sculpt clipping. */
      ss.cache->flag |= CLIP_X << i;

      /* Update the clip tolerance. */
      if (mmd->tolerance > ss.cache->clip_tolerance[i]) {
        ss.cache->clip_tolerance[i] = mmd->tolerance;
      }

      /* Store matrix for mirror object clipping. */
      if (mmd->mirror_ob) {
        float imtx_mirror_ob[4][4];
        invert_m4_m4(imtx_mirror_ob, mmd->mirror_ob->object_to_world().ptr());
        mul_m4_m4m4(ss.cache->clip_mirror_mtx.ptr(), imtx_mirror_ob, ob.object_to_world().ptr());
      }
    }
  }
}

static void smooth_brush_toggle_on(const bContext *C, Paint *paint, StrokeCache *cache)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  Brush *cur_brush = BKE_paint_brush(paint);

  if (cur_brush->sculpt_tool == SCULPT_TOOL_MASK) {
    cache->saved_mask_brush_tool = cur_brush->mask_tool;
    cur_brush->mask_tool = BRUSH_MASK_SMOOTH;
    return;
  }

  if (ELEM(cur_brush->sculpt_tool,
           SCULPT_TOOL_SLIDE_RELAX,
           SCULPT_TOOL_DRAW_FACE_SETS,
           SCULPT_TOOL_PAINT,
           SCULPT_TOOL_SMEAR))
  {
    /* Do nothing, this tool has its own smooth mode. */
    return;
  }

  /* Switch to the smooth brush if possible. */
  BKE_paint_brush_set_essentials(bmain, paint, "Smooth");
  Brush *smooth_brush = BKE_paint_brush(paint);

  if (!smooth_brush) {
    BKE_paint_brush_set(paint, cur_brush);
    CLOG_WARN(&LOG, "Switching to the smooth brush not possible, corresponding brush not");
    cache->saved_active_brush = nullptr;
    return;
  }

  int cur_brush_size = BKE_brush_size_get(scene, cur_brush);

  cache->saved_active_brush = cur_brush;

  cache->saved_smooth_size = BKE_brush_size_get(scene, smooth_brush);
  BKE_brush_size_set(scene, smooth_brush, cur_brush_size);
  BKE_curvemapping_init(smooth_brush->curve);
}

static void smooth_brush_toggle_off(const bContext *C, Paint *paint, StrokeCache *cache)
{
  Brush &brush = *BKE_paint_brush(paint);

  if (brush.sculpt_tool == SCULPT_TOOL_MASK) {
    brush.mask_tool = cache->saved_mask_brush_tool;
    return;
  }

  if (ELEM(brush.sculpt_tool,
           SCULPT_TOOL_SLIDE_RELAX,
           SCULPT_TOOL_DRAW_FACE_SETS,
           SCULPT_TOOL_PAINT,
           SCULPT_TOOL_SMEAR))
  {
    /* Do nothing. */
    return;
  }

  /* If saved_active_brush is not set, brush was not switched/affected in
   * smooth_brush_toggle_on(). */
  if (cache->saved_active_brush) {
    Scene *scene = CTX_data_scene(C);
    BKE_brush_size_set(scene, &brush, cache->saved_smooth_size);
    BKE_paint_brush_set(paint, cache->saved_active_brush);
    cache->saved_active_brush = nullptr;
  }
}

/* Initialize the stroke cache invariants from operator properties. */
static void sculpt_update_cache_invariants(
    bContext *C, Sculpt &sd, SculptSession &ss, wmOperator *op, const float mval[2])
{
  StrokeCache *cache = MEM_new<StrokeCache>(__func__);
  ToolSettings *tool_settings = CTX_data_tool_settings(C);
  UnifiedPaintSettings *ups = &tool_settings->unified_paint_settings;
  const Brush *brush = BKE_paint_brush_for_read(&sd.paint);
  ViewContext *vc = paint_stroke_view_context(static_cast<PaintStroke *>(op->customdata));
  Object &ob = *CTX_data_active_object(C);
  float mat[3][3];
  float viewDir[3] = {0.0f, 0.0f, 1.0f};
  float max_scale;
  int mode;

  ss.cache = cache;

  /* Set scaling adjustment. */
  max_scale = 0.0f;
  for (int i = 0; i < 3; i++) {
    max_scale = max_ff(max_scale, fabsf(ob.scale[i]));
  }
  cache->scale[0] = max_scale / ob.scale[0];
  cache->scale[1] = max_scale / ob.scale[1];
  cache->scale[2] = max_scale / ob.scale[2];

  cache->plane_trim_squared = brush->plane_trim * brush->plane_trim;

  cache->flag = 0;

  sculpt_init_mirror_clipping(ob, ss);

  /* Initial mouse location. */
  if (mval) {
    copy_v2_v2(cache->initial_mouse, mval);
  }
  else {
    zero_v2(cache->initial_mouse);
  }

  copy_v3_v3(cache->initial_location, ss.cursor_location);
  copy_v3_v3(cache->true_initial_location, ss.cursor_location);

  copy_v3_v3(cache->initial_normal, ss.cursor_normal);
  copy_v3_v3(cache->true_initial_normal, ss.cursor_normal);

  mode = RNA_enum_get(op->ptr, "mode");
  cache->invert = mode == BRUSH_STROKE_INVERT;
  cache->alt_smooth = mode == BRUSH_STROKE_SMOOTH;
  cache->normal_weight = brush->normal_weight;

  /* Interpret invert as following normal, for grab brushes. */
  if (SCULPT_TOOL_HAS_NORMAL_WEIGHT(brush->sculpt_tool)) {
    if (cache->invert) {
      cache->invert = false;
      cache->normal_weight = (cache->normal_weight == 0.0f);
    }
  }

  /* Not very nice, but with current events system implementation
   * we can't handle brush appearance inversion hotkey separately (sergey). */
  if (cache->invert) {
    ups->draw_inverted = true;
  }
  else {
    ups->draw_inverted = false;
  }

  /* Alt-Smooth. */
  if (cache->alt_smooth) {
    smooth_brush_toggle_on(C, &sd.paint, cache);
    /* Refresh the brush pointer in case we switched brush in the toggle function. */
    brush = BKE_paint_brush(&sd.paint);
  }

  copy_v2_v2(cache->mouse, cache->initial_mouse);
  copy_v2_v2(cache->mouse_event, cache->initial_mouse);
  copy_v2_v2(ups->tex_mouse, cache->initial_mouse);

  /* Truly temporary data that isn't stored in properties. */
  cache->vc = vc;
  cache->brush = brush;

  /* Cache projection matrix. */
  cache->projection_mat = ED_view3d_ob_project_mat_get(cache->vc->rv3d, &ob);

  invert_m4_m4(ob.runtime->world_to_object.ptr(), ob.object_to_world().ptr());
  copy_m3_m4(mat, cache->vc->rv3d->viewinv);
  mul_m3_v3(mat, viewDir);
  copy_m3_m4(mat, ob.world_to_object().ptr());
  mul_m3_v3(mat, viewDir);
  normalize_v3_v3(cache->true_view_normal, viewDir);

  cache->supports_gravity = (!ELEM(brush->sculpt_tool,
                                   SCULPT_TOOL_MASK,
                                   SCULPT_TOOL_SMOOTH,
                                   SCULPT_TOOL_SIMPLIFY,
                                   SCULPT_TOOL_DISPLACEMENT_SMEAR,
                                   SCULPT_TOOL_DISPLACEMENT_ERASER) &&
                             (sd.gravity_factor > 0.0f));
  /* Get gravity vector in world space. */
  if (cache->supports_gravity) {
    if (sd.gravity_object) {
      Object *gravity_object = sd.gravity_object;

      copy_v3_v3(cache->true_gravity_direction, gravity_object->object_to_world().ptr()[2]);
    }
    else {
      cache->true_gravity_direction[0] = cache->true_gravity_direction[1] = 0.0f;
      cache->true_gravity_direction[2] = 1.0f;
    }

    /* Transform to sculpted object space. */
    mul_m3_v3(mat, cache->true_gravity_direction);
    normalize_v3(cache->true_gravity_direction);
  }

  cache->accum = true;

  /* Make copies of the mesh vertex locations and normals for some tools. */
  if (brush->flag & BRUSH_ANCHORED) {
    cache->accum = false;
  }

  /* Draw sharp does not need the original coordinates to produce the accumulate effect, so it
   * should work the opposite way. */
  if (brush->sculpt_tool == SCULPT_TOOL_DRAW_SHARP) {
    cache->accum = false;
  }

  if (SCULPT_TOOL_HAS_ACCUMULATE(brush->sculpt_tool)) {
    if (!(brush->flag & BRUSH_ACCUMULATE)) {
      cache->accum = false;
      if (brush->sculpt_tool == SCULPT_TOOL_DRAW_SHARP) {
        cache->accum = true;
      }
    }
  }

  /* Original coordinates require the sculpt undo system, which isn't used
   * for image brushes. It's also not necessary, just disable it. */
  if (brush && brush->sculpt_tool == SCULPT_TOOL_PAINT &&
      SCULPT_use_image_paint_brush(tool_settings->paint_mode, ob))
  {
    cache->accum = true;
  }

  cache->first_time = true;

#define PIXEL_INPUT_THRESHHOLD 5
  if (brush->sculpt_tool == SCULPT_TOOL_ROTATE) {
    cache->dial = BLI_dial_init(cache->initial_mouse, PIXEL_INPUT_THRESHHOLD);
  }

#undef PIXEL_INPUT_THRESHHOLD
}

static float sculpt_brush_dynamic_size_get(const Brush &brush,
                                           const StrokeCache &cache,
                                           float initial_size)
{
  switch (brush.sculpt_tool) {
    case SCULPT_TOOL_CLAY:
      return max_ff(initial_size * 0.20f, initial_size * pow3f(cache.pressure));
    case SCULPT_TOOL_CLAY_STRIPS:
      return max_ff(initial_size * 0.30f, initial_size * powf(cache.pressure, 1.5f));
    case SCULPT_TOOL_CLAY_THUMB: {
      float clay_stabilized_pressure = clay_thumb_get_stabilized_pressure(cache);
      return initial_size * clay_stabilized_pressure;
    }
    default:
      return initial_size * cache.pressure;
  }
}

/* In these brushes the grab delta is calculated always from the initial stroke location, which is
 * generally used to create grab deformations. */
static bool sculpt_needs_delta_from_anchored_origin(const Brush &brush)
{
  if (brush.sculpt_tool == SCULPT_TOOL_SMEAR && (brush.flag & BRUSH_ANCHORED)) {
    return true;
  }

  if (ELEM(brush.sculpt_tool,
           SCULPT_TOOL_GRAB,
           SCULPT_TOOL_POSE,
           SCULPT_TOOL_BOUNDARY,
           SCULPT_TOOL_THUMB,
           SCULPT_TOOL_ELASTIC_DEFORM))
  {
    return true;
  }
  if (brush.sculpt_tool == SCULPT_TOOL_CLOTH && brush.cloth_deform_type == BRUSH_CLOTH_DEFORM_GRAB)
  {
    return true;
  }
  return false;
}

/* In these brushes the grab delta is calculated from the previous stroke location, which is used
 * to calculate to orientate the brush tip and deformation towards the stroke direction. */
static bool sculpt_needs_delta_for_tip_orientation(const Brush &brush)
{
  if (brush.sculpt_tool == SCULPT_TOOL_CLOTH) {
    return brush.cloth_deform_type != BRUSH_CLOTH_DEFORM_GRAB;
  }
  return ELEM(brush.sculpt_tool,
              SCULPT_TOOL_CLAY_STRIPS,
              SCULPT_TOOL_PINCH,
              SCULPT_TOOL_MULTIPLANE_SCRAPE,
              SCULPT_TOOL_CLAY_THUMB,
              SCULPT_TOOL_NUDGE,
              SCULPT_TOOL_SNAKE_HOOK);
}

static void sculpt_update_brush_delta(UnifiedPaintSettings &ups, Object &ob, const Brush &brush)
{
  SculptSession &ss = *ob.sculpt;
  StrokeCache *cache = ss.cache;
  const float mval[2] = {
      cache->mouse_event[0],
      cache->mouse_event[1],
  };
  int tool = brush.sculpt_tool;

  if (!ELEM(tool,
            SCULPT_TOOL_PAINT,
            SCULPT_TOOL_GRAB,
            SCULPT_TOOL_ELASTIC_DEFORM,
            SCULPT_TOOL_CLOTH,
            SCULPT_TOOL_NUDGE,
            SCULPT_TOOL_CLAY_STRIPS,
            SCULPT_TOOL_PINCH,
            SCULPT_TOOL_MULTIPLANE_SCRAPE,
            SCULPT_TOOL_CLAY_THUMB,
            SCULPT_TOOL_SNAKE_HOOK,
            SCULPT_TOOL_POSE,
            SCULPT_TOOL_BOUNDARY,
            SCULPT_TOOL_SMEAR,
            SCULPT_TOOL_THUMB) &&
      !sculpt_brush_use_topology_rake(ss, brush))
  {
    return;
  }
  float grab_location[3], imat[4][4], delta[3], loc[3];

  if (SCULPT_stroke_is_first_brush_step_of_symmetry_pass(*ss.cache)) {
    if (tool == SCULPT_TOOL_GRAB && brush.flag & BRUSH_GRAB_ACTIVE_VERTEX) {
      copy_v3_v3(cache->orig_grab_location,
                 SCULPT_vertex_co_for_grab_active_get(ss, SCULPT_active_vertex_get(ss)));
    }
    else {
      copy_v3_v3(cache->orig_grab_location, cache->true_location);
    }
  }
  else if (tool == SCULPT_TOOL_SNAKE_HOOK ||
           (tool == SCULPT_TOOL_CLOTH && brush.cloth_deform_type == BRUSH_CLOTH_DEFORM_SNAKE_HOOK))
  {
    add_v3_v3(cache->true_location, cache->grab_delta);
  }

  /* Compute 3d coordinate at same z from original location + mval. */
  mul_v3_m4v3(loc, ob.object_to_world().ptr(), cache->orig_grab_location);
  ED_view3d_win_to_3d(cache->vc->v3d, cache->vc->region, loc, mval, grab_location);

  /* Compute delta to move verts by. */
  if (!SCULPT_stroke_is_first_brush_step_of_symmetry_pass(*ss.cache)) {
    if (sculpt_needs_delta_from_anchored_origin(brush)) {
      sub_v3_v3v3(delta, grab_location, cache->old_grab_location);
      invert_m4_m4(imat, ob.object_to_world().ptr());
      mul_mat3_m4_v3(imat, delta);
      add_v3_v3(cache->grab_delta, delta);
    }
    else if (sculpt_needs_delta_for_tip_orientation(brush)) {
      if (brush.flag & BRUSH_ANCHORED) {
        float orig[3];
        mul_v3_m4v3(orig, ob.object_to_world().ptr(), cache->orig_grab_location);
        sub_v3_v3v3(cache->grab_delta, grab_location, orig);
      }
      else {
        sub_v3_v3v3(cache->grab_delta, grab_location, cache->old_grab_location);
      }
      invert_m4_m4(imat, ob.object_to_world().ptr());
      mul_mat3_m4_v3(imat, cache->grab_delta);
    }
    else {
      /* Use for 'Brush.topology_rake_factor'. */
      sub_v3_v3v3(cache->grab_delta, grab_location, cache->old_grab_location);
    }
  }
  else {
    zero_v3(cache->grab_delta);
  }

  if (brush.falloff_shape == PAINT_FALLOFF_SHAPE_TUBE) {
    project_plane_v3_v3v3(cache->grab_delta, cache->grab_delta, ss.cache->true_view_normal);
  }

  copy_v3_v3(cache->old_grab_location, grab_location);

  if (tool == SCULPT_TOOL_GRAB) {
    if (brush.flag & BRUSH_GRAB_ACTIVE_VERTEX) {
      copy_v3_v3(cache->anchored_location, cache->orig_grab_location);
    }
    else {
      copy_v3_v3(cache->anchored_location, cache->true_location);
    }
  }
  else if (tool == SCULPT_TOOL_ELASTIC_DEFORM || cloth::is_cloth_deform_brush(brush)) {
    copy_v3_v3(cache->anchored_location, cache->true_location);
  }
  else if (tool == SCULPT_TOOL_THUMB) {
    copy_v3_v3(cache->anchored_location, cache->orig_grab_location);
  }

  if (sculpt_needs_delta_from_anchored_origin(brush)) {
    /* Location stays the same for finding vertices in brush radius. */
    copy_v3_v3(cache->true_location, cache->orig_grab_location);

    ups.draw_anchored = true;
    copy_v2_v2(ups.anchored_initial_mouse, cache->initial_mouse);
    ups.anchored_size = ups.pixel_radius;
  }

  /* Handle 'rake' */
  cache->is_rake_rotation_valid = false;

  invert_m4_m4(imat, ob.object_to_world().ptr());
  mul_mat3_m4_v3(imat, grab_location);

  if (SCULPT_stroke_is_first_brush_step_of_symmetry_pass(*ss.cache)) {
    copy_v3_v3(cache->rake_data.follow_co, grab_location);
  }

  if (!sculpt_brush_needs_rake_rotation(brush)) {
    return;
  }
  cache->rake_data.follow_dist = cache->radius * SCULPT_RAKE_BRUSH_FACTOR;

  if (!is_zero_v3(cache->grab_delta)) {
    const float eps = 0.00001f;

    float v1[3], v2[3];

    copy_v3_v3(v1, cache->rake_data.follow_co);
    copy_v3_v3(v2, cache->rake_data.follow_co);
    sub_v3_v3(v2, cache->grab_delta);

    sub_v3_v3(v1, grab_location);
    sub_v3_v3(v2, grab_location);

    if ((normalize_v3(v2) > eps) && (normalize_v3(v1) > eps) && (len_squared_v3v3(v1, v2) > eps)) {
      const float rake_dist_sq = len_squared_v3v3(cache->rake_data.follow_co, grab_location);
      const float rake_fade = (rake_dist_sq > square_f(cache->rake_data.follow_dist)) ?
                                  1.0f :
                                  sqrtf(rake_dist_sq) / cache->rake_data.follow_dist;

      float axis[3], angle;
      float tquat[4];

      rotation_between_vecs_to_quat(tquat, v1, v2);

      /* Use axis-angle to scale rotation since the factor may be above 1. */
      quat_to_axis_angle(axis, &angle, tquat);
      normalize_v3(axis);

      angle *= brush.rake_factor * rake_fade;
      axis_angle_normalized_to_quat(cache->rake_rotation, axis, angle);
      cache->is_rake_rotation_valid = true;
    }
  }
  sculpt_rake_data_update(&cache->rake_data, grab_location);
}

static void sculpt_update_cache_paint_variants(StrokeCache &cache, const Brush &brush)
{
  cache.paint_brush.hardness = brush.hardness;
  if (brush.paint_flags & BRUSH_PAINT_HARDNESS_PRESSURE) {
    cache.paint_brush.hardness *= brush.paint_flags & BRUSH_PAINT_HARDNESS_PRESSURE_INVERT ?
                                      1.0f - cache.pressure :
                                      cache.pressure;
  }

  cache.paint_brush.flow = brush.flow;
  if (brush.paint_flags & BRUSH_PAINT_FLOW_PRESSURE) {
    cache.paint_brush.flow *= brush.paint_flags & BRUSH_PAINT_FLOW_PRESSURE_INVERT ?
                                  1.0f - cache.pressure :
                                  cache.pressure;
  }

  cache.paint_brush.wet_mix = brush.wet_mix;
  if (brush.paint_flags & BRUSH_PAINT_WET_MIX_PRESSURE) {
    cache.paint_brush.wet_mix *= brush.paint_flags & BRUSH_PAINT_WET_MIX_PRESSURE_INVERT ?
                                     1.0f - cache.pressure :
                                     cache.pressure;

    /* This makes wet mix more sensible in higher values, which allows to create brushes that have
     * a wider pressure range were they only blend colors without applying too much of the brush
     * color. */
    cache.paint_brush.wet_mix = 1.0f - pow2f(1.0f - cache.paint_brush.wet_mix);
  }

  cache.paint_brush.wet_persistence = brush.wet_persistence;
  if (brush.paint_flags & BRUSH_PAINT_WET_PERSISTENCE_PRESSURE) {
    cache.paint_brush.wet_persistence = brush.paint_flags &
                                                BRUSH_PAINT_WET_PERSISTENCE_PRESSURE_INVERT ?
                                            1.0f - cache.pressure :
                                            cache.pressure;
  }

  cache.paint_brush.density = brush.density;
  if (brush.paint_flags & BRUSH_PAINT_DENSITY_PRESSURE) {
    cache.paint_brush.density = brush.paint_flags & BRUSH_PAINT_DENSITY_PRESSURE_INVERT ?
                                    1.0f - cache.pressure :
                                    cache.pressure;
  }
}

/* Initialize the stroke cache variants from operator properties. */
static void sculpt_update_cache_variants(bContext *C, Sculpt &sd, Object &ob, PointerRNA *ptr)
{
  Scene &scene = *CTX_data_scene(C);
  UnifiedPaintSettings &ups = scene.toolsettings->unified_paint_settings;
  SculptSession &ss = *ob.sculpt;
  StrokeCache &cache = *ss.cache;
  Brush &brush = *BKE_paint_brush(&sd.paint);

  if (SCULPT_stroke_is_first_brush_step_of_symmetry_pass(cache) ||
      !((brush.flag & BRUSH_ANCHORED) || (brush.sculpt_tool == SCULPT_TOOL_SNAKE_HOOK) ||
        (brush.sculpt_tool == SCULPT_TOOL_ROTATE) || cloth::is_cloth_deform_brush(brush)))
  {
    RNA_float_get_array(ptr, "location", cache.true_location);
  }

  cache.pen_flip = RNA_boolean_get(ptr, "pen_flip");
  RNA_float_get_array(ptr, "mouse", cache.mouse);
  RNA_float_get_array(ptr, "mouse_event", cache.mouse_event);

  /* XXX: Use pressure value from first brush step for brushes which don't support strokes (grab,
   * thumb). They depends on initial state and brush coord/pressure/etc.
   * It's more an events design issue, which doesn't split coordinate/pressure/angle changing
   * events. We should avoid this after events system re-design. */
  if (paint_supports_dynamic_size(brush, PaintMode::Sculpt) || cache.first_time) {
    cache.pressure = RNA_float_get(ptr, "pressure");
  }

  cache.x_tilt = RNA_float_get(ptr, "x_tilt");
  cache.y_tilt = RNA_float_get(ptr, "y_tilt");

  /* Truly temporary data that isn't stored in properties. */
  if (SCULPT_stroke_is_first_brush_step_of_symmetry_pass(*ss.cache)) {
    cache.initial_radius = sculpt_calc_radius(*cache.vc, brush, scene, cache.true_location);

    if (!BKE_brush_use_locked_size(&scene, &brush)) {
      BKE_brush_unprojected_radius_set(&scene, &brush, cache.initial_radius);
    }
  }

  /* Clay stabilized pressure. */
  if (brush.sculpt_tool == SCULPT_TOOL_CLAY_THUMB) {
    if (SCULPT_stroke_is_first_brush_step_of_symmetry_pass(*ss.cache)) {
      for (int i = 0; i < SCULPT_CLAY_STABILIZER_LEN; i++) {
        ss.cache->clay_pressure_stabilizer[i] = 0.0f;
      }
      ss.cache->clay_pressure_stabilizer_index = 0;
    }
    else {
      cache.clay_pressure_stabilizer[cache.clay_pressure_stabilizer_index] = cache.pressure;
      cache.clay_pressure_stabilizer_index += 1;
      if (cache.clay_pressure_stabilizer_index >= SCULPT_CLAY_STABILIZER_LEN) {
        cache.clay_pressure_stabilizer_index = 0;
      }
    }
  }

  if (BKE_brush_use_size_pressure(&brush) && paint_supports_dynamic_size(brush, PaintMode::Sculpt))
  {
    cache.radius = sculpt_brush_dynamic_size_get(brush, cache, cache.initial_radius);
    cache.dyntopo_pixel_radius = sculpt_brush_dynamic_size_get(
        brush, cache, ups.initial_pixel_radius);
  }
  else {
    cache.radius = cache.initial_radius;
    cache.dyntopo_pixel_radius = ups.initial_pixel_radius;
  }

  sculpt_update_cache_paint_variants(cache, brush);

  cache.radius_squared = cache.radius * cache.radius;

  if (brush.flag & BRUSH_ANCHORED) {
    /* True location has been calculated as part of the stroke system already here. */
    if (brush.flag & BRUSH_EDGE_TO_EDGE) {
      RNA_float_get_array(ptr, "location", cache.true_location);
    }

    cache.radius = paint_calc_object_space_radius(
        *cache.vc, cache.true_location, ups.pixel_radius);
    cache.radius_squared = cache.radius * cache.radius;

    copy_v3_v3(cache.anchored_location, cache.true_location);
  }

  sculpt_update_brush_delta(ups, ob, brush);

  if (brush.sculpt_tool == SCULPT_TOOL_ROTATE) {
    cache.vertex_rotation = -BLI_dial_angle(cache.dial, cache.mouse) * cache.bstrength;

    ups.draw_anchored = true;
    copy_v2_v2(ups.anchored_initial_mouse, cache.initial_mouse);
    copy_v3_v3(cache.anchored_location, cache.true_location);
    ups.anchored_size = ups.pixel_radius;
  }

  cache.special_rotation = ups.brush_rotation;

  cache.iteration_count++;
}

/* Returns true if any of the smoothing modes are active (currently
 * one of smooth brush, autosmooth, mask smooth, or shift-key
 * smooth). */
static bool sculpt_needs_connectivity_info(const Sculpt &sd,
                                           const Brush &brush,
                                           const SculptSession &ss,
                                           int stroke_mode)
{
  if (ss.pbvh && auto_mask::is_enabled(sd, &ss, &brush)) {
    return true;
  }
  return ((stroke_mode == BRUSH_STROKE_SMOOTH) || (ss.cache && ss.cache->alt_smooth) ||
          (brush.sculpt_tool == SCULPT_TOOL_SMOOTH) || (brush.autosmooth_factor > 0) ||
          ((brush.sculpt_tool == SCULPT_TOOL_MASK) && (brush.mask_tool == BRUSH_MASK_SMOOTH)) ||
          (brush.sculpt_tool == SCULPT_TOOL_POSE) || (brush.sculpt_tool == SCULPT_TOOL_BOUNDARY) ||
          (brush.sculpt_tool == SCULPT_TOOL_SLIDE_RELAX) ||
          SCULPT_tool_is_paint(brush.sculpt_tool) || (brush.sculpt_tool == SCULPT_TOOL_CLOTH) ||
          (brush.sculpt_tool == SCULPT_TOOL_SMEAR) ||
          (brush.sculpt_tool == SCULPT_TOOL_DRAW_FACE_SETS) ||
          (brush.sculpt_tool == SCULPT_TOOL_DISPLACEMENT_SMEAR) ||
          (brush.sculpt_tool == SCULPT_TOOL_PAINT));
}

}  // namespace blender::ed::sculpt_paint

void SCULPT_stroke_modifiers_check(const bContext *C, Object &ob, const Brush &brush)
{
  using namespace blender::ed::sculpt_paint;
  SculptSession &ss = *ob.sculpt;
  RegionView3D *rv3d = CTX_wm_region_view3d(C);
  const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;

  bool need_pmap = sculpt_needs_connectivity_info(sd, brush, ss, 0);
  if (ss.shapekey_active || ss.deform_modifiers_active ||
      (!BKE_sculptsession_use_pbvh_draw(&ob, rv3d) && need_pmap))
  {
    Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
    BKE_sculpt_update_object_for_edit(depsgraph, &ob, SCULPT_tool_is_paint(brush.sculpt_tool));
  }
}

static void sculpt_raycast_cb(blender::bke::pbvh::Node &node, SculptRaycastData &srd, float *tmin)
{
  using namespace blender;
  using namespace blender::ed::sculpt_paint;
  if (BKE_pbvh_node_get_tmin(&node) >= *tmin) {
    return;
  }
  const float(*origco)[3] = nullptr;
  bool use_origco = false;

  if (srd.original && srd.ss->cache) {
    if (srd.ss->pbvh->type() == bke::pbvh::Type::BMesh) {
      use_origco = true;
    }
    else {
      /* Intersect with coordinates from before we started stroke. */
      const undo::Node *unode = undo::get_node(&node, undo::Type::Position);
      origco = (unode) ? reinterpret_cast<const float(*)[3]>(unode->position.data()) : nullptr;
      use_origco = origco ? true : false;
    }
  }

  if (bke::pbvh::raycast_node(*srd.ss->pbvh,
                              node,
                              origco,
                              use_origco,
                              srd.corner_verts,
                              srd.corner_tris,
                              srd.corner_tri_faces,
                              srd.hide_poly,
                              srd.ray_start,
                              srd.ray_normal,
                              &srd.isect_precalc,
                              &srd.depth,
                              &srd.active_vertex,
                              &srd.active_face_grid_index,
                              srd.face_normal))
  {
    srd.hit = true;
    *tmin = srd.depth;
  }
}

static void sculpt_find_nearest_to_ray_cb(blender::bke::pbvh::Node &node,
                                          SculptFindNearestToRayData &srd,
                                          float *tmin)
{
  using namespace blender;
  using namespace blender::ed::sculpt_paint;
  if (BKE_pbvh_node_get_tmin(&node) >= *tmin) {
    return;
  }
  const float(*origco)[3] = nullptr;
  bool use_origco = false;

  if (srd.original && srd.ss->cache) {
    if (srd.ss->pbvh->type() == bke::pbvh::Type::BMesh) {
      use_origco = true;
    }
    else {
      /* Intersect with coordinates from before we started stroke. */
      const undo::Node *unode = undo::get_node(&node, undo::Type::Position);
      origco = (unode) ? reinterpret_cast<const float(*)[3]>(unode->position.data()) : nullptr;
      use_origco = origco ? true : false;
    }
  }

  if (bke::pbvh::find_nearest_to_ray_node(*srd.ss->pbvh,
                                          node,
                                          origco,
                                          use_origco,
                                          srd.corner_verts,
                                          srd.corner_tris,
                                          srd.corner_tri_faces,
                                          srd.hide_poly,
                                          srd.ray_start,
                                          srd.ray_normal,
                                          &srd.depth,
                                          &srd.dist_sq_to_ray))
  {
    srd.hit = true;
    *tmin = srd.dist_sq_to_ray;
  }
}

float SCULPT_raycast_init(ViewContext *vc,
                          const float mval[2],
                          float ray_start[3],
                          float ray_end[3],
                          float ray_normal[3],
                          bool original)
{
  using namespace blender;
  float obimat[4][4];
  float dist;
  Object &ob = *vc->obact;
  RegionView3D *rv3d = vc->rv3d;
  View3D *v3d = vc->v3d;

  /* TODO: what if the segment is totally clipped? (return == 0). */
  ED_view3d_win_to_segment_clipped(
      vc->depsgraph, vc->region, vc->v3d, mval, ray_start, ray_end, true);

  invert_m4_m4(obimat, ob.object_to_world().ptr());
  mul_m4_v3(obimat, ray_start);
  mul_m4_v3(obimat, ray_end);

  sub_v3_v3v3(ray_normal, ray_end, ray_start);
  dist = normalize_v3(ray_normal);

  /* If the ray is clipped, don't adjust its start/end. */
  if ((rv3d->is_persp == false) && !RV3D_CLIPPING_ENABLED(v3d, rv3d)) {
    /* Get the view origin without the addition
     * of -ray_normal * clip_start that
     * ED_view3d_win_to_segment_clipped gave us.
     * This is necessary to avoid floating point overflow.
     */
    ED_view3d_win_to_origin(vc->region, mval, ray_start);
    mul_m4_v3(obimat, ray_start);

    bke::pbvh::clip_ray_ortho(*ob.sculpt->pbvh, original, ray_start, ray_end, ray_normal);

    dist = len_v3v3(ray_start, ray_end);
  }

  return dist;
}

bool SCULPT_cursor_geometry_info_update(bContext *C,
                                        SculptCursorGeometryInfo *out,
                                        const float mval[2],
                                        bool use_sampled_normal)
{
  using namespace blender;
  using namespace blender::ed::sculpt_paint;
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  Scene *scene = CTX_data_scene(C);
  const Brush &brush = *BKE_paint_brush_for_read(BKE_paint_get_active_from_context(C));
  float ray_start[3], ray_end[3], ray_normal[3], depth, face_normal[3], mat[3][3];
  float viewDir[3] = {0.0f, 0.0f, 1.0f};
  bool original = false;

  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);

  Object &ob = *vc.obact;
  SculptSession &ss = *ob.sculpt;

  const View3D *v3d = CTX_wm_view3d(C);
  const Base *base = CTX_data_active_base(C);

  if (!ss.pbvh || !vc.rv3d || !BKE_base_is_visible(v3d, base)) {
    zero_v3(out->location);
    zero_v3(out->normal);
    zero_v3(out->active_vertex_co);
    return false;
  }

  /* bke::pbvh::Tree raycast to get active vertex and face normal. */
  depth = SCULPT_raycast_init(&vc, mval, ray_start, ray_end, ray_normal, original);
  SCULPT_stroke_modifiers_check(C, ob, brush);

  SculptRaycastData srd{};
  srd.original = original;
  srd.ss = ob.sculpt;
  srd.hit = false;
  if (ss.pbvh->type() == bke::pbvh::Type::Mesh) {
    const Mesh &mesh = *static_cast<const Mesh *>(ob.data);
    srd.corner_verts = mesh.corner_verts();
    srd.corner_tris = mesh.corner_tris();
    srd.corner_tri_faces = mesh.corner_tri_faces();
    const bke::AttributeAccessor attributes = mesh.attributes();
    srd.hide_poly = *attributes.lookup<bool>(".hide_poly", bke::AttrDomain::Face);
  }
  srd.ray_start = ray_start;
  srd.ray_normal = ray_normal;
  srd.depth = depth;
  srd.face_normal = face_normal;

  isect_ray_tri_watertight_v3_precalc(&srd.isect_precalc, ray_normal);
  bke::pbvh::raycast(
      *ss.pbvh,
      [&](bke::pbvh::Node &node, float *tmin) { sculpt_raycast_cb(node, srd, tmin); },
      ray_start,
      ray_normal,
      srd.original);

  /* Cursor is not over the mesh, return default values. */
  if (!srd.hit) {
    zero_v3(out->location);
    zero_v3(out->normal);
    zero_v3(out->active_vertex_co);
    return false;
  }

  /* Update the active vertex of the SculptSession. */
  ss.active_vertex = srd.active_vertex;
  SCULPT_vertex_random_access_ensure(ss);
  copy_v3_v3(out->active_vertex_co, SCULPT_active_vertex_co_get(ss));

  switch (ss.pbvh->type()) {
    case bke::pbvh::Type::Mesh:
      ss.active_face_index = srd.active_face_grid_index;
      ss.active_grid_index = 0;
      break;
    case bke::pbvh::Type::Grids:
      ss.active_face_index = 0;
      ss.active_grid_index = srd.active_face_grid_index;
      break;
    case bke::pbvh::Type::BMesh:
      ss.active_face_index = 0;
      ss.active_grid_index = 0;
      break;
  }

  copy_v3_v3(out->location, ray_normal);
  mul_v3_fl(out->location, srd.depth);
  add_v3_v3(out->location, ray_start);

  /* Option to return the face normal directly for performance o accuracy reasons. */
  if (!use_sampled_normal) {
    copy_v3_v3(out->normal, srd.face_normal);
    return srd.hit;
  }

  /* Sampled normal calculation. */
  float radius;

  /* Update cursor data in SculptSession. */
  invert_m4_m4(ob.runtime->world_to_object.ptr(), ob.object_to_world().ptr());
  copy_m3_m4(mat, vc.rv3d->viewinv);
  mul_m3_v3(mat, viewDir);
  copy_m3_m4(mat, ob.world_to_object().ptr());
  mul_m3_v3(mat, viewDir);
  normalize_v3_v3(ss.cursor_view_normal, viewDir);
  copy_v3_v3(ss.cursor_normal, srd.face_normal);
  copy_v3_v3(ss.cursor_location, out->location);
  ss.rv3d = vc.rv3d;
  ss.v3d = vc.v3d;

  if (!BKE_brush_use_locked_size(scene, &brush)) {
    radius = paint_calc_object_space_radius(vc, out->location, BKE_brush_size_get(scene, &brush));
  }
  else {
    radius = BKE_brush_unprojected_radius_get(scene, &brush);
  }
  ss.cursor_radius = radius;

  Vector<bke::pbvh::Node *> nodes = sculpt_pbvh_gather_cursor_update(ob, original);

  /* In case there are no nodes under the cursor, return the face normal. */
  if (nodes.is_empty()) {
    copy_v3_v3(out->normal, srd.face_normal);
    return true;
  }

  /* Calculate the sampled normal. */
  if (const std::optional<float3> sampled_normal = calc_area_normal(brush, ob, nodes)) {
    copy_v3_v3(out->normal, *sampled_normal);
    copy_v3_v3(ss.cursor_sampled_normal, *sampled_normal);
  }
  else {
    /* Use face normal when there are no vertices to sample inside the cursor radius. */
    copy_v3_v3(out->normal, srd.face_normal);
  }
  return true;
}

bool SCULPT_stroke_get_location(bContext *C,
                                float out[3],
                                const float mval[2],
                                bool force_original)
{
  const Brush *brush = BKE_paint_brush(BKE_paint_get_active_from_context(C));
  bool check_closest = brush->falloff_shape == PAINT_FALLOFF_SHAPE_TUBE;

  return SCULPT_stroke_get_location_ex(C, out, mval, force_original, check_closest, true);
}

bool SCULPT_stroke_get_location_ex(bContext *C,
                                   float out[3],
                                   const float mval[2],
                                   bool force_original,
                                   bool check_closest,
                                   bool limit_closest_radius)
{
  using namespace blender;
  using namespace blender::ed::sculpt_paint;
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  float ray_start[3], ray_end[3], ray_normal[3], depth, face_normal[3];

  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);

  Object &ob = *vc.obact;

  SculptSession &ss = *ob.sculpt;
  StrokeCache *cache = ss.cache;
  bool original = force_original || ((cache) ? !cache->accum : false);

  const Brush &brush = *BKE_paint_brush(BKE_paint_get_active_from_context(C));

  SCULPT_stroke_modifiers_check(C, ob, brush);

  depth = SCULPT_raycast_init(&vc, mval, ray_start, ray_end, ray_normal, original);

  if (ss.pbvh->type() == bke::pbvh::Type::BMesh) {
    BM_mesh_elem_table_ensure(ss.bm, BM_VERT);
    BM_mesh_elem_index_ensure(ss.bm, BM_VERT);
  }

  bool hit = false;
  {
    SculptRaycastData srd;
    srd.ss = ob.sculpt;
    srd.ray_start = ray_start;
    srd.ray_normal = ray_normal;
    srd.hit = false;
    if (ss.pbvh->type() == bke::pbvh::Type::Mesh) {
      const Mesh &mesh = *static_cast<const Mesh *>(ob.data);
      srd.corner_verts = mesh.corner_verts();
      srd.corner_tris = mesh.corner_tris();
      srd.corner_tri_faces = mesh.corner_tri_faces();
      const bke::AttributeAccessor attributes = mesh.attributes();
      srd.hide_poly = *attributes.lookup<bool>(".hide_poly", bke::AttrDomain::Face);
    }
    srd.depth = depth;
    srd.original = original;
    srd.face_normal = face_normal;
    isect_ray_tri_watertight_v3_precalc(&srd.isect_precalc, ray_normal);

    bke::pbvh::raycast(
        *ss.pbvh,
        [&](bke::pbvh::Node &node, float *tmin) { sculpt_raycast_cb(node, srd, tmin); },
        ray_start,
        ray_normal,
        srd.original);
    if (srd.hit) {
      hit = true;
      copy_v3_v3(out, ray_normal);
      mul_v3_fl(out, srd.depth);
      add_v3_v3(out, ray_start);
    }
  }

  if (hit || !check_closest) {
    return hit;
  }

  SculptFindNearestToRayData srd{};
  srd.original = original;
  srd.ss = ob.sculpt;
  srd.hit = false;
  if (ss.pbvh->type() == bke::pbvh::Type::Mesh) {
    const Mesh &mesh = *static_cast<const Mesh *>(ob.data);
    srd.corner_verts = mesh.corner_verts();
    srd.corner_tris = mesh.corner_tris();
    srd.corner_tri_faces = mesh.corner_tri_faces();
    const bke::AttributeAccessor attributes = mesh.attributes();
    srd.hide_poly = *attributes.lookup<bool>(".hide_poly", bke::AttrDomain::Face);
  }
  srd.ray_start = ray_start;
  srd.ray_normal = ray_normal;
  srd.depth = FLT_MAX;
  srd.dist_sq_to_ray = FLT_MAX;

  bke::pbvh::find_nearest_to_ray(
      *ss.pbvh,
      [&](bke::pbvh::Node &node, float *tmin) { sculpt_find_nearest_to_ray_cb(node, srd, tmin); },
      ray_start,
      ray_normal,
      srd.original);
  if (srd.hit && srd.dist_sq_to_ray) {
    hit = true;
    copy_v3_v3(out, ray_normal);
    mul_v3_fl(out, srd.depth);
    add_v3_v3(out, ray_start);
  }

  float closest_radius_sq = FLT_MAX;
  if (limit_closest_radius) {
    closest_radius_sq = sculpt_calc_radius(vc, brush, *CTX_data_scene(C), out);
    closest_radius_sq *= closest_radius_sq;
  }

  return hit && srd.dist_sq_to_ray < closest_radius_sq;
}

static void sculpt_brush_init_tex(const Sculpt &sd, SculptSession &ss)
{
  const Brush *brush = BKE_paint_brush_for_read(&sd.paint);
  const MTex *mask_tex = BKE_brush_mask_texture_get(brush, OB_MODE_SCULPT);

  /* Init mtex nodes. */
  if (mask_tex->tex && mask_tex->tex->nodetree) {
    /* Has internal flag to detect it only does it once. */
    ntreeTexBeginExecTree(mask_tex->tex->nodetree);
  }

  if (ss.tex_pool == nullptr) {
    ss.tex_pool = BKE_image_pool_new();
  }
}

static void sculpt_brush_stroke_init(bContext *C)
{
  Object &ob = *CTX_data_active_object(C);
  ToolSettings *tool_settings = CTX_data_tool_settings(C);
  const Sculpt &sd = *tool_settings->sculpt;
  SculptSession &ss = *CTX_data_active_object(C)->sculpt;
  const Brush *brush = BKE_paint_brush_for_read(&sd.paint);

  if (!G.background) {
    view3d_operator_needs_opengl(C);
  }
  sculpt_brush_init_tex(sd, ss);

  const bool needs_colors = SCULPT_tool_is_paint(brush->sculpt_tool) &&
                            !SCULPT_use_image_paint_brush(tool_settings->paint_mode, ob);

  if (needs_colors) {
    BKE_sculpt_color_layer_create_if_needed(&ob);
  }

  /* CTX_data_ensure_evaluated_depsgraph should be used at the end to include the updates of
   * earlier steps modifying the data. */
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  BKE_sculpt_update_object_for_edit(depsgraph, &ob, SCULPT_tool_is_paint(brush->sculpt_tool));

  ED_paint_tool_update_sticky_shading_color(C, &ob);
}

static void sculpt_restore_mesh(const Sculpt &sd, Object &ob)
{
  using namespace blender::ed::sculpt_paint;
  SculptSession &ss = *ob.sculpt;
  const Brush *brush = BKE_paint_brush_for_read(&sd.paint);

  /* Brushes that also use original coordinates and will need a "restore" step.
   *  - SCULPT_TOOL_BOUNDARY
   * TODO: Investigate removing this step using the same technique as the layer brush-- in the
   * brush, apply the translation between the result of the last brush step and the result of the
   * latest brush step.
   */
  if (ELEM(brush->sculpt_tool,
           SCULPT_TOOL_ELASTIC_DEFORM,
           SCULPT_TOOL_GRAB,
           SCULPT_TOOL_THUMB,
           SCULPT_TOOL_ROTATE,
           SCULPT_TOOL_POSE))
  {
    undo::restore_from_undo_step(sd, ob);
    return;
  }

  /* For the cloth brush it makes more sense to not restore the mesh state to keep running the
   * simulation from the previous state. */
  if (brush->sculpt_tool == SCULPT_TOOL_CLOTH) {
    return;
  }

  /* Restore the mesh before continuing with anchored stroke. */
  if ((brush->flag & BRUSH_ANCHORED) ||
      (ELEM(brush->sculpt_tool, SCULPT_TOOL_GRAB, SCULPT_TOOL_ELASTIC_DEFORM) &&
       BKE_brush_use_size_pressure(brush)) ||
      (brush->flag & BRUSH_DRAG_DOT))
  {

    undo::restore_from_undo_step(sd, ob);

    if (ss.cache) {
      ss.cache->layer_displacement_factor = {};
    }
  }
}

namespace blender::ed::sculpt_paint {

void flush_update_step(bContext *C, UpdateType update_type)
{
  Depsgraph &depsgraph = *CTX_data_depsgraph_pointer(C);
  Object &ob = *CTX_data_active_object(C);
  SculptSession &ss = *ob.sculpt;
  ARegion &region = *CTX_wm_region(C);
  MultiresModifierData *mmd = ss.multires.modifier;
  RegionView3D *rv3d = CTX_wm_region_view3d(C);
  Mesh *mesh = static_cast<Mesh *>(ob.data);

  const bool use_pbvh_draw = BKE_sculptsession_use_pbvh_draw(&ob, rv3d);

  if (rv3d) {
    /* Mark for faster 3D viewport redraws. */
    rv3d->rflag |= RV3D_PAINTING;
  }

  if (mmd != nullptr) {
    multires_mark_as_modified(&depsgraph, &ob, MULTIRES_COORDS_MODIFIED);
  }

  if ((update_type == UpdateType::Image) != 0) {
    ED_region_tag_redraw(&region);
    if (update_type == UpdateType::Image) {
      /* Early exit when only need to update the images. We don't want to tag any geometry updates
       * that would rebuild the bke::pbvh::Tree. */
      return;
    }
  }

  DEG_id_tag_update(&ob.id, ID_RECALC_SHADING);

  /* Only current viewport matters, slower update for all viewports will
   * be done in sculpt_flush_update_done. */
  if (!use_pbvh_draw) {
    /* Slow update with full dependency graph update and all that comes with it.
     * Needed when there are modifiers or full shading in the 3D viewport. */
    DEG_id_tag_update(&ob.id, ID_RECALC_GEOMETRY);
    ED_region_tag_redraw(&region);
  }
  else {
    /* Fast path where we just update the BVH nodes that changed, and redraw
     * only the part of the 3D viewport where changes happened. */
    rcti r;

    if (update_type == UpdateType::Position) {
      bke::pbvh::update_bounds(*ss.pbvh);
    }

    RegionView3D *rv3d = CTX_wm_region_view3d(C);
    if (rv3d && SCULPT_get_redraw_rect(region, *rv3d, ob, r)) {
      if (ss.cache) {
        ss.cache->current_r = r;
      }

      /* previous is not set in the current cache else
       * the partial rect will always grow */
      sculpt_extend_redraw_rect_previous(ob, r);

      r.xmin += region.winrct.xmin - 2;
      r.xmax += region.winrct.xmin + 2;
      r.ymin += region.winrct.ymin - 2;
      r.ymax += region.winrct.ymin + 2;
      ED_region_tag_redraw_partial(&region, &r, true);
    }
  }

  if (update_type == UpdateType::Position && !ss.shapekey_active) {
    if (ss.pbvh->type() == bke::pbvh::Type::Mesh) {
      /* Updating mesh positions without marking caches dirty is generally not good, but since
       * sculpt mode has special requirements and is expected to have sole ownership of the mesh it
       * modifies, it's generally okay. */
      if (use_pbvh_draw) {
        /* When drawing from bke::pbvh::Tree is used, vertex and face normals are updated
         * later in #bke::pbvh::update_normals. However, we update the mesh's bounds eagerly here
         * since they are trivial to access from the bke::pbvh::Tree. Updating the
         * object's evaluated geometry bounding box is necessary because sculpt strokes don't cause
         * an object reevaluation. */
        mesh->tag_positions_changed_no_normals();
        /* Sculpt mode does not use or recalculate face corner normals, so they are cleared. */
        mesh->runtime->corner_normals_cache.tag_dirty();
      }
      else {
        /* Drawing happens from the modifier stack evaluation result.
         * Tag both coordinates and normals as modified, as both needed for proper drawing and the
         * modifier stack is not guaranteed to tag normals for update. */
        mesh->tag_positions_changed();
      }

      mesh->bounds_set_eager(bke::pbvh::bounds_get(*ob.sculpt->pbvh));
      if (ob.runtime->bounds_eval) {
        ob.runtime->bounds_eval = mesh->bounds_min_max();
      }
    }
  }
}

void flush_update_done(const bContext *C, Object &ob, UpdateType update_type)
{
  /* After we are done drawing the stroke, check if we need to do a more
   * expensive depsgraph tag to update geometry. */
  wmWindowManager *wm = CTX_wm_manager(C);
  RegionView3D *current_rv3d = CTX_wm_region_view3d(C);
  SculptSession &ss = *ob.sculpt;
  Mesh *mesh = static_cast<Mesh *>(ob.data);

  /* Always needed for linked duplicates. */
  bool need_tag = (ID_REAL_USERS(&mesh->id) > 1);

  if (current_rv3d) {
    current_rv3d->rflag &= ~RV3D_PAINTING;
  }

  LISTBASE_FOREACH (wmWindow *, win, &wm->windows) {
    bScreen *screen = WM_window_get_active_screen(win);
    LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
      SpaceLink *sl = static_cast<SpaceLink *>(area->spacedata.first);
      if (sl->spacetype != SPACE_VIEW3D) {
        continue;
      }

      /* Tag all 3D viewports for redraw now that we are done. Others
       * viewports did not get a full redraw, and anti-aliasing for the
       * current viewport was deactivated. */
      LISTBASE_FOREACH (ARegion *, region, &area->regionbase) {
        if (region->regiontype == RGN_TYPE_WINDOW) {
          RegionView3D *rv3d = static_cast<RegionView3D *>(region->regiondata);
          if (rv3d != current_rv3d) {
            need_tag |= !BKE_sculptsession_use_pbvh_draw(&ob, rv3d);
          }

          ED_region_tag_redraw(region);
        }
      }
    }

    if (update_type == UpdateType::Image) {
      LISTBASE_FOREACH (ScrArea *, area, &screen->areabase) {
        SpaceLink *sl = static_cast<SpaceLink *>(area->spacedata.first);
        if (sl->spacetype != SPACE_IMAGE) {
          continue;
        }
        ED_area_tag_redraw_regiontype(area, RGN_TYPE_WINDOW);
      }
    }
  }

  if (update_type == UpdateType::Position) {
    bke::pbvh::store_bounds_orig(*ss.pbvh);

    /* Coordinates were modified, so fake neighbors are not longer valid. */
    SCULPT_fake_neighbors_free(ob);
  }

  if (update_type == UpdateType::Mask) {
    bke::pbvh::update_mask(*ss.pbvh);
  }

  BKE_sculpt_attributes_destroy_temporary_stroke(&ob);

  if (update_type == UpdateType::Position) {
    if (ss.pbvh->type() == bke::pbvh::Type::BMesh) {
      BKE_pbvh_bmesh_after_stroke(*ss.pbvh);
    }

    /* Optimization: if there is locked key and active modifiers present in */
    /* the stack, keyblock is updating at each step. otherwise we could update */
    /* keyblock only when stroke is finished. */
    if (ss.shapekey_active && !ss.deform_modifiers_active) {
      sculpt_update_keyblock(ob);
    }
  }

  if (need_tag) {
    DEG_id_tag_update(&ob.id, ID_RECALC_GEOMETRY);
  }
}

}  // namespace blender::ed::sculpt_paint

/* Returns whether the mouse/stylus is over the mesh (1)
 * or over the background (0). */
static bool over_mesh(bContext *C, wmOperator * /*op*/, const float mval[2])
{
  float co_dummy[3];
  Sculpt *sd = CTX_data_tool_settings(C)->sculpt;
  Brush *brush = BKE_paint_brush(&sd->paint);

  bool check_closest = brush->falloff_shape == PAINT_FALLOFF_SHAPE_TUBE;

  return SCULPT_stroke_get_location_ex(C, co_dummy, mval, false, check_closest, true);
}

static void sculpt_stroke_undo_begin(const bContext *C, wmOperator *op)
{
  using namespace blender::ed::sculpt_paint;
  Object &ob = *CTX_data_active_object(C);
  const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  const Brush *brush = BKE_paint_brush_for_read(&sd.paint);
  ToolSettings *tool_settings = CTX_data_tool_settings(C);

  /* Setup the correct undo system. Image painting and sculpting are mutual exclusive.
   * Color attributes are part of the sculpting undo system. */
  if (brush && brush->sculpt_tool == SCULPT_TOOL_PAINT &&
      SCULPT_use_image_paint_brush(tool_settings->paint_mode, ob))
  {
    ED_image_undo_push_begin(op->type->name, PaintMode::Sculpt);
  }
  else {
    undo::push_begin_ex(ob, sculpt_tool_name(sd));
  }
}

static void sculpt_stroke_undo_end(const bContext *C, Brush *brush)
{
  using namespace blender::ed::sculpt_paint;
  Object &ob = *CTX_data_active_object(C);
  ToolSettings *tool_settings = CTX_data_tool_settings(C);

  if (brush && brush->sculpt_tool == SCULPT_TOOL_PAINT &&
      SCULPT_use_image_paint_brush(tool_settings->paint_mode, ob))
  {
    ED_image_undo_push_end();
  }
  else {
    undo::push_end(ob);
  }
}

bool SCULPT_handles_colors_report(SculptSession &ss, ReportList *reports)
{
  switch (ss.pbvh->type()) {
    case blender::bke::pbvh::Type::Mesh:
      return true;
    case blender::bke::pbvh::Type::BMesh:
      BKE_report(reports, RPT_ERROR, "Not supported in dynamic topology mode");
      return false;
    case blender::bke::pbvh::Type::Grids:
      BKE_report(reports, RPT_ERROR, "Not supported in multiresolution mode");
      return false;
  }
  BLI_assert_unreachable();
  return false;
}

namespace blender::ed::sculpt_paint {

static bool sculpt_stroke_test_start(bContext *C, wmOperator *op, const float mval[2])
{
  /* Don't start the stroke until `mval` goes over the mesh.
   * NOTE: `mval` will only be null when re-executing the saved stroke.
   * We have exception for 'exec' strokes since they may not set `mval`,
   * only 'location', see: #52195. */
  if (((op->flag & OP_IS_INVOKE) == 0) || (mval == nullptr) || over_mesh(C, op, mval)) {
    Object &ob = *CTX_data_active_object(C);
    SculptSession &ss = *ob.sculpt;
    Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
    Brush *brush = BKE_paint_brush(&sd.paint);
    ToolSettings *tool_settings = CTX_data_tool_settings(C);

    /* NOTE: This should be removed when paint mode is available. Paint mode can force based on the
     * canvas it is painting on. (ref. use_sculpt_texture_paint). */
    if (brush && SCULPT_tool_is_paint(brush->sculpt_tool) &&
        !SCULPT_use_image_paint_brush(tool_settings->paint_mode, ob))
    {
      View3D *v3d = CTX_wm_view3d(C);
      if (v3d->shading.type == OB_SOLID) {
        v3d->shading.color_type = V3D_SHADING_VERTEX_COLOR;
      }
    }

    ED_view3d_init_mats_rv3d(&ob, CTX_wm_region_view3d(C));

    sculpt_update_cache_invariants(C, sd, ss, op, mval);

    SculptCursorGeometryInfo sgi;
    SCULPT_cursor_geometry_info_update(C, &sgi, mval, false);

    sculpt_stroke_undo_begin(C, op);

    SCULPT_stroke_id_next(ob);
    ss.cache->stroke_id = ss.stroke_id;

    return true;
  }
  return false;
}

static void sculpt_stroke_update_step(bContext *C,
                                      wmOperator * /*op*/,
                                      PaintStroke *stroke,
                                      PointerRNA *itemptr)
{
  UnifiedPaintSettings &ups = CTX_data_tool_settings(C)->unified_paint_settings;
  const Scene &scene = *CTX_data_scene(C);
  Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  Object &ob = *CTX_data_active_object(C);
  SculptSession &ss = *ob.sculpt;
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);
  ToolSettings &tool_settings = *CTX_data_tool_settings(C);
  StrokeCache *cache = ss.cache;
  cache->stroke_distance = paint_stroke_distance_get(stroke);

  SCULPT_stroke_modifiers_check(C, ob, brush);
  sculpt_update_cache_variants(C, sd, ob, itemptr);
  sculpt_restore_mesh(sd, ob);

  if (sd.flags & (SCULPT_DYNTOPO_DETAIL_CONSTANT | SCULPT_DYNTOPO_DETAIL_MANUAL)) {
    BKE_pbvh_bmesh_detail_size_set(
        *ss.pbvh, dyntopo::detail_size::constant_to_detail_size(sd.constant_detail, ob));
  }
  else if (sd.flags & SCULPT_DYNTOPO_DETAIL_BRUSH) {
    BKE_pbvh_bmesh_detail_size_set(
        *ss.pbvh, dyntopo::detail_size::brush_to_detail_size(sd.detail_percent, ss.cache->radius));
  }
  else {
    BKE_pbvh_bmesh_detail_size_set(
        *ss.pbvh,
        dyntopo::detail_size::relative_to_detail_size(
            sd.detail_size, ss.cache->radius, ss.cache->dyntopo_pixel_radius, U.pixelsize));
  }

  if (dyntopo::stroke_is_dyntopo(ss, brush)) {
    do_symmetrical_brush_actions(
        scene, sd, ob, sculpt_topology_update, ups, tool_settings.paint_mode);
  }

  do_symmetrical_brush_actions(scene, sd, ob, do_brush_action, ups, tool_settings.paint_mode);

  /* Hack to fix noise texture tearing mesh. */
  sculpt_fix_noise_tear(sd, ob);

  /* TODO(sergey): This is not really needed for the solid shading,
   * which does use pBVH drawing anyway, but texture and wireframe
   * requires this.
   *
   * Could be optimized later, but currently don't think it's so
   * much common scenario.
   *
   * Same applies to the DEG_id_tag_update() invoked from
   * sculpt_flush_update_step().
   *
   * For some brushes, flushing is done in the brush code itself.
   */
  if ((ELEM(brush.sculpt_tool, SCULPT_TOOL_BOUNDARY) || ss.pbvh->type() != bke::pbvh::Type::Mesh))
  {
    if (ss.deform_modifiers_active) {
      SCULPT_flush_stroke_deform(sd, ob, sculpt_tool_is_proxy_used(brush.sculpt_tool));
    }
    else if (ss.shapekey_active) {
      sculpt_update_keyblock(ob);
    }
  }

  ss.cache->first_time = false;
  copy_v3_v3(ss.cache->true_last_location, ss.cache->true_location);

  /* Cleanup. */
  if (brush.sculpt_tool == SCULPT_TOOL_MASK) {
    flush_update_step(C, UpdateType::Mask);
  }
  else if (SCULPT_tool_is_paint(brush.sculpt_tool)) {
    if (SCULPT_use_image_paint_brush(tool_settings.paint_mode, ob)) {
      flush_update_step(C, UpdateType::Image);
    }
    else {
      flush_update_step(C, UpdateType::Color);
    }
  }
  else {
    flush_update_step(C, UpdateType::Position);
  }
}

static void sculpt_brush_exit_tex(Sculpt &sd)
{
  Brush *brush = BKE_paint_brush(&sd.paint);
  const MTex *mask_tex = BKE_brush_mask_texture_get(brush, OB_MODE_SCULPT);

  if (mask_tex->tex && mask_tex->tex->nodetree) {
    ntreeTexEndExecTree(mask_tex->tex->nodetree->runtime->execdata);
  }
}

static void sculpt_stroke_done(const bContext *C, PaintStroke * /*stroke*/)
{
  Object &ob = *CTX_data_active_object(C);
  SculptSession &ss = *ob.sculpt;
  Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  ToolSettings *tool_settings = CTX_data_tool_settings(C);

  /* Finished. */
  if (!ss.cache) {
    sculpt_brush_exit_tex(sd);
    return;
  }
  UnifiedPaintSettings *ups = &CTX_data_tool_settings(C)->unified_paint_settings;
  Brush *brush = BKE_paint_brush(&sd.paint);
  BLI_assert(brush == ss.cache->brush); /* const, so we shouldn't change. */
  ups->draw_inverted = false;

  SCULPT_stroke_modifiers_check(C, ob, *brush);

  /* Alt-Smooth. */
  if (ss.cache->alt_smooth) {
    smooth_brush_toggle_off(C, &sd.paint, ss.cache);
    /* Refresh the brush pointer in case we switched brush in the toggle function. */
    brush = BKE_paint_brush(&sd.paint);
  }

  MEM_delete(ss.cache);
  ss.cache = nullptr;

  sculpt_stroke_undo_end(C, brush);

  if (brush->sculpt_tool == SCULPT_TOOL_MASK) {
    flush_update_done(C, ob, UpdateType::Mask);
  }
  else if (brush->sculpt_tool == SCULPT_TOOL_PAINT) {
    if (SCULPT_use_image_paint_brush(tool_settings->paint_mode, ob)) {
      flush_update_done(C, ob, UpdateType::Image);
    }
    else {
      BKE_sculpt_attributes_destroy_temporary_stroke(&ob);
      flush_update_done(C, ob, UpdateType::Color);
    }
  }
  else {
    flush_update_done(C, ob, UpdateType::Position);
  }

  WM_event_add_notifier(C, NC_OBJECT | ND_DRAW, &ob);
  sculpt_brush_exit_tex(sd);
}

static int sculpt_brush_stroke_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  PaintStroke *stroke;
  int ignore_background_click;
  int retval;
  Object &ob = *CTX_data_active_object(C);

  const View3D *v3d = CTX_wm_view3d(C);
  const Base *base = CTX_data_active_base(C);
  /* Test that ob is visible; otherwise we won't be able to get evaluated data
   * from the depsgraph. We do this here instead of SCULPT_mode_poll
   * to avoid falling through to the translate operator in the
   * global view3d keymap. */
  if (!BKE_base_is_visible(v3d, base)) {
    return OPERATOR_CANCELLED;
  }

  sculpt_brush_stroke_init(C);

  Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  Brush &brush = *BKE_paint_brush(&sd.paint);
  SculptSession &ss = *ob.sculpt;

  if (SCULPT_tool_is_paint(brush.sculpt_tool) &&
      !SCULPT_handles_colors_report(*ob.sculpt, op->reports))
  {
    return OPERATOR_CANCELLED;
  }
  if (SCULPT_tool_is_mask(brush.sculpt_tool)) {
    MultiresModifierData *mmd = BKE_sculpt_multires_active(ss.scene, &ob);
    BKE_sculpt_mask_layers_ensure(CTX_data_depsgraph_pointer(C), CTX_data_main(C), &ob, mmd);
  }
  if (!SCULPT_tool_is_attribute_only(brush.sculpt_tool) &&
      ED_sculpt_report_if_shape_key_is_locked(ob, op->reports))
  {
    return OPERATOR_CANCELLED;
  }
  if (ELEM(brush.sculpt_tool, SCULPT_TOOL_DISPLACEMENT_SMEAR, SCULPT_TOOL_DISPLACEMENT_ERASER)) {
    if (!ss.pbvh || ss.pbvh->type() != bke::pbvh::Type::Grids) {
      BKE_report(op->reports, RPT_ERROR, "Only supported in multiresolution mode");
      return OPERATOR_CANCELLED;
    }
  }

  stroke = paint_stroke_new(C,
                            op,
                            SCULPT_stroke_get_location,
                            sculpt_stroke_test_start,
                            sculpt_stroke_update_step,
                            nullptr,
                            sculpt_stroke_done,
                            event->type);

  op->customdata = stroke;

  /* For tablet rotation. */
  ignore_background_click = RNA_boolean_get(op->ptr, "ignore_background_click");
  const float mval[2] = {float(event->mval[0]), float(event->mval[1])};
  if (ignore_background_click && !over_mesh(C, op, mval)) {
    paint_stroke_free(C, op, static_cast<PaintStroke *>(op->customdata));
    return OPERATOR_PASS_THROUGH;
  }

  retval = op->type->modal(C, op, event);
  if (ELEM(retval, OPERATOR_FINISHED, OPERATOR_CANCELLED)) {
    paint_stroke_free(C, op, static_cast<PaintStroke *>(op->customdata));
    return retval;
  }
  /* Add modal handler. */
  WM_event_add_modal_handler(C, op);

  OPERATOR_RETVAL_CHECK(retval);
  BLI_assert(retval == OPERATOR_RUNNING_MODAL);

  return OPERATOR_RUNNING_MODAL;
}

static int sculpt_brush_stroke_exec(bContext *C, wmOperator *op)
{
  sculpt_brush_stroke_init(C);

  op->customdata = paint_stroke_new(C,
                                    op,
                                    SCULPT_stroke_get_location,
                                    sculpt_stroke_test_start,
                                    sculpt_stroke_update_step,
                                    nullptr,
                                    sculpt_stroke_done,
                                    0);

  /* Frees op->customdata. */
  paint_stroke_exec(C, op, static_cast<PaintStroke *>(op->customdata));

  return OPERATOR_FINISHED;
}

static void sculpt_brush_stroke_cancel(bContext *C, wmOperator *op)
{
  using namespace blender::ed::sculpt_paint;
  Object &ob = *CTX_data_active_object(C);
  SculptSession &ss = *ob.sculpt;
  Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);

  /* XXX Canceling strokes that way does not work with dynamic topology,
   *     user will have to do real undo for now. See #46456. */
  if (ss.cache && !dyntopo::stroke_is_dyntopo(ss, brush)) {
    undo::restore_from_undo_step(sd, ob);
  }

  paint_stroke_cancel(C, op, static_cast<PaintStroke *>(op->customdata));

  MEM_delete(ss.cache);
  ss.cache = nullptr;

  sculpt_brush_exit_tex(sd);
}

static int sculpt_brush_stroke_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  return paint_stroke_modal(C, op, event, (PaintStroke **)&op->customdata);
}

static void sculpt_redo_empty_ui(bContext * /*C*/, wmOperator * /*op*/) {}

void SCULPT_OT_brush_stroke(wmOperatorType *ot)
{
  /* Identifiers. */
  ot->name = "Sculpt";
  ot->idname = "SCULPT_OT_brush_stroke";
  ot->description = "Sculpt a stroke into the geometry";

  /* API callbacks. */
  ot->invoke = sculpt_brush_stroke_invoke;
  ot->modal = sculpt_brush_stroke_modal;
  ot->exec = sculpt_brush_stroke_exec;
  ot->poll = SCULPT_poll;
  ot->cancel = sculpt_brush_stroke_cancel;
  ot->ui = sculpt_redo_empty_ui;

  /* Flags (sculpt does its own undo? (ton)). */
  ot->flag = OPTYPE_BLOCKING;

  /* Properties. */

  paint_stroke_operator_properties(ot);

  RNA_def_boolean(ot->srna,
                  "ignore_background_click",
                  false,
                  "Ignore Background Click",
                  "Clicks on the background do not start the stroke");
}

/* Fake Neighbors. */
/* This allows the sculpt tools to work on meshes with multiple connected components as they had
 * only one connected component. When initialized and enabled, the sculpt API will return extra
 * connectivity neighbors that are not in the real mesh. These neighbors are calculated for each
 * vertex using the minimum distance to a vertex that is in a different connected component. */

/* The fake neighbors first need to be ensured to be initialized.
 * After that tools which needs fake neighbors functionality need to
 * temporarily enable it:
 *
 *   void my_awesome_sculpt_tool() {
 *     SCULPT_fake_neighbors_ensure(object, brush->disconnected_distance_max);
 *     SCULPT_fake_neighbors_enable(ob);
 *
 *     ... Logic of the tool ...
 *     SCULPT_fake_neighbors_disable(ob);
 *   }
 *
 * Such approach allows to keep all the connectivity information ready for reuse
 * (without having lag prior to every stroke), but also makes it so the affect
 * is localized to a specific brushes and tools only. */

enum {
  SCULPT_TOPOLOGY_ID_NONE,
  SCULPT_TOPOLOGY_ID_DEFAULT,
};

static void fake_neighbor_init(SculptSession &ss, const float max_dist)
{
  const int totvert = SCULPT_vertex_count_get(ss);
  ss.fake_neighbors.fake_neighbor_index = static_cast<int *>(
      MEM_malloc_arrayN(totvert, sizeof(int), "fake neighbor"));
  for (int i = 0; i < totvert; i++) {
    ss.fake_neighbors.fake_neighbor_index[i] = FAKE_NEIGHBOR_NONE;
  }

  ss.fake_neighbors.current_max_distance = max_dist;
}

static void fake_neighbor_add(SculptSession &ss, PBVHVertRef v_a, PBVHVertRef v_b)
{
  int v_index_a = BKE_pbvh_vertex_to_index(*ss.pbvh, v_a);
  int v_index_b = BKE_pbvh_vertex_to_index(*ss.pbvh, v_b);

  if (ss.fake_neighbors.fake_neighbor_index[v_index_a] == FAKE_NEIGHBOR_NONE) {
    ss.fake_neighbors.fake_neighbor_index[v_index_a] = v_index_b;
    ss.fake_neighbors.fake_neighbor_index[v_index_b] = v_index_a;
  }
}

static void sculpt_pose_fake_neighbors_free(SculptSession &ss)
{
  MEM_SAFE_FREE(ss.fake_neighbors.fake_neighbor_index);
}

struct NearestVertexFakeNeighborData {
  PBVHVertRef nearest_vertex;
  float nearest_vertex_distance_sq;
  int current_topology_id;
};

static void do_fake_neighbor_search_task(SculptSession &ss,
                                         const float nearest_vertex_search_co[3],
                                         const float max_distance_sq,
                                         bke::pbvh::Node *node,
                                         NearestVertexFakeNeighborData *nvtd)
{
  PBVHVertexIter vd;
  BKE_pbvh_vertex_iter_begin (*ss.pbvh, node, vd, PBVH_ITER_UNIQUE) {
    int vd_topology_id = islands::vert_id_get(ss, vd.vertex);
    if (vd_topology_id != nvtd->current_topology_id &&
        ss.fake_neighbors.fake_neighbor_index[vd.index] == FAKE_NEIGHBOR_NONE)
    {
      float distance_squared = len_squared_v3v3(vd.co, nearest_vertex_search_co);
      if (distance_squared < nvtd->nearest_vertex_distance_sq &&
          distance_squared < max_distance_sq)
      {
        nvtd->nearest_vertex = vd.vertex;
        nvtd->nearest_vertex_distance_sq = distance_squared;
      }
    }
  }
  BKE_pbvh_vertex_iter_end;
}

static PBVHVertRef fake_neighbor_search(Object &ob, const PBVHVertRef vertex, float max_distance)
{
  SculptSession &ss = *ob.sculpt;

  const float3 center = SCULPT_vertex_co_get(ss, vertex);
  const float max_distance_sq = max_distance * max_distance;

  Vector<bke::pbvh::Node *> nodes = bke::pbvh::search_gather(*ss.pbvh, [&](bke::pbvh::Node &node) {
    return node_in_sphere(node, center, max_distance_sq, false);
  });
  if (nodes.is_empty()) {
    return BKE_pbvh_make_vref(PBVH_REF_NONE);
  }

  const float3 nearest_vertex_search_co = SCULPT_vertex_co_get(ss, vertex);

  NearestVertexFakeNeighborData nvtd;
  nvtd.nearest_vertex.i = -1;
  nvtd.nearest_vertex_distance_sq = FLT_MAX;
  nvtd.current_topology_id = islands::vert_id_get(ss, vertex);

  nvtd = threading::parallel_reduce(
      nodes.index_range(),
      1,
      nvtd,
      [&](const IndexRange range, NearestVertexFakeNeighborData nvtd) {
        for (const int i : range) {
          do_fake_neighbor_search_task(
              ss, nearest_vertex_search_co, max_distance_sq, nodes[i], &nvtd);
        }
        return nvtd;
      },
      [](const NearestVertexFakeNeighborData &a, const NearestVertexFakeNeighborData &b) {
        NearestVertexFakeNeighborData joined = a;
        if (joined.nearest_vertex.i == PBVH_REF_NONE) {
          joined.nearest_vertex = b.nearest_vertex;
          joined.nearest_vertex_distance_sq = b.nearest_vertex_distance_sq;
        }
        else if (b.nearest_vertex_distance_sq < joined.nearest_vertex_distance_sq) {
          joined.nearest_vertex = b.nearest_vertex;
          joined.nearest_vertex_distance_sq = b.nearest_vertex_distance_sq;
        }
        return joined;
      });

  return nvtd.nearest_vertex;
}

struct SculptTopologyIDFloodFillData {
  int next_id;
};

}  // namespace blender::ed::sculpt_paint

namespace blender::ed::sculpt_paint::boundary {

void ensure_boundary_info(Object &object)
{
  SculptSession &ss = *object.sculpt;
  if (!ss.vertex_info.boundary.is_empty()) {
    return;
  }

  Mesh *base_mesh = BKE_mesh_from_object(&object);

  ss.vertex_info.boundary.resize(base_mesh->verts_num);
  Array<int> adjacent_faces_edge_count(base_mesh->edges_num, 0);
  array_utils::count_indices(base_mesh->corner_edges(), adjacent_faces_edge_count);

  const Span<int2> edges = base_mesh->edges();
  for (const int e : edges.index_range()) {
    if (adjacent_faces_edge_count[e] < 2) {
      const int2 &edge = edges[e];
      ss.vertex_info.boundary[edge[0]].set();
      ss.vertex_info.boundary[edge[1]].set();
    }
  }
}

}  // namespace blender::ed::sculpt_paint::boundary

void SCULPT_fake_neighbors_ensure(Object &ob, const float max_dist)
{
  using namespace blender::ed::sculpt_paint;
  SculptSession &ss = *ob.sculpt;
  const int totvert = SCULPT_vertex_count_get(ss);

  /* Fake neighbors were already initialized with the same distance, so no need to be
   * recalculated.
   */
  if (ss.fake_neighbors.fake_neighbor_index && ss.fake_neighbors.current_max_distance == max_dist)
  {
    return;
  }

  islands::ensure_cache(ob);
  fake_neighbor_init(ss, max_dist);

  for (int i = 0; i < totvert; i++) {
    const PBVHVertRef from_v = BKE_pbvh_index_to_vertex(*ss.pbvh, i);

    /* This vertex does not have a fake neighbor yet, search one for it. */
    if (ss.fake_neighbors.fake_neighbor_index[i] == FAKE_NEIGHBOR_NONE) {
      const PBVHVertRef to_v = fake_neighbor_search(ob, from_v, max_dist);
      if (to_v.i != PBVH_REF_NONE) {
        /* Add the fake neighbor if available. */
        fake_neighbor_add(ss, from_v, to_v);
      }
    }
  }
}

void SCULPT_fake_neighbors_enable(Object &ob)
{
  SculptSession &ss = *ob.sculpt;
  BLI_assert(ss.fake_neighbors.fake_neighbor_index != nullptr);
  ss.fake_neighbors.use_fake_neighbors = true;
}

void SCULPT_fake_neighbors_disable(Object &ob)
{
  SculptSession &ss = *ob.sculpt;
  BLI_assert(ss.fake_neighbors.fake_neighbor_index != nullptr);
  ss.fake_neighbors.use_fake_neighbors = false;
}

void SCULPT_fake_neighbors_free(Object &ob)
{
  using namespace blender::ed::sculpt_paint;
  SculptSession &ss = *ob.sculpt;
  sculpt_pose_fake_neighbors_free(ss);
}

bool SCULPT_vertex_is_occluded(SculptSession &ss, PBVHVertRef vertex, bool original)
{
  using namespace blender;
  float ray_start[3], ray_end[3], ray_normal[3], face_normal[3];
  float co[3];

  copy_v3_v3(co, SCULPT_vertex_co_get(ss, vertex));

  ViewContext *vc = ss.cache ? ss.cache->vc : &ss.filter_cache->vc;

  const blender::float2 mouse = ED_view3d_project_float_v2_m4(
      vc->region, co, ss.cache ? ss.cache->projection_mat : ss.filter_cache->viewmat);

  int depth = SCULPT_raycast_init(vc, mouse, ray_end, ray_start, ray_normal, original);

  negate_v3(ray_normal);

  copy_v3_v3(ray_start, SCULPT_vertex_co_get(ss, vertex));
  madd_v3_v3fl(ray_start, ray_normal, 0.002);

  SculptRaycastData srd = {nullptr};
  srd.original = original;
  srd.ss = &ss;
  srd.hit = false;
  srd.ray_start = ray_start;
  srd.ray_normal = ray_normal;
  srd.depth = depth;
  srd.face_normal = face_normal;
  srd.corner_verts = ss.corner_verts;
  if (ss.pbvh->type() == bke::pbvh::Type::Mesh) {
    srd.corner_tris = BKE_pbvh_get_mesh(*ss.pbvh)->corner_tris();
    srd.corner_tri_faces = BKE_pbvh_get_mesh(*ss.pbvh)->corner_tri_faces();
  }

  isect_ray_tri_watertight_v3_precalc(&srd.isect_precalc, ray_normal);
  bke::pbvh::raycast(
      *ss.pbvh,
      [&](bke::pbvh::Node &node, float *tmin) { sculpt_raycast_cb(node, srd, tmin); },
      ray_start,
      ray_normal,
      srd.original);

  return srd.hit;
}

void SCULPT_stroke_id_next(Object &ob)
{
  /* Manually wrap in int32 space to avoid tripping up undefined behavior
   * sanitizers.
   */
  ob.sculpt->stroke_id = uchar((int(ob.sculpt->stroke_id) + 1) & 255);
}

void SCULPT_stroke_id_ensure(Object &ob)
{
  using namespace blender;
  SculptSession &ss = *ob.sculpt;

  if (!ss.attrs.automasking_stroke_id) {
    SculptAttributeParams params = {0};
    ss.attrs.automasking_stroke_id = BKE_sculpt_attribute_ensure(
        &ob,
        bke::AttrDomain::Point,
        CD_PROP_INT8,
        SCULPT_ATTRIBUTE_NAME(automasking_stroke_id),
        &params);
  }
}

namespace blender::ed::sculpt_paint::islands {

int vert_id_get(const SculptSession &ss, PBVHVertRef vertex)
{
  if (ss.attrs.topology_island_key) {
    return *static_cast<uint8_t *>(SCULPT_vertex_attr_get(vertex, ss.attrs.topology_island_key));
  }

  return -1;
}

void invalidate(SculptSession &ss)
{
  ss.islands_valid = false;
}

void ensure_cache(Object &ob)
{
  SculptSession &ss = *ob.sculpt;

  if (ss.attrs.topology_island_key && ss.islands_valid &&
      ss.pbvh->type() != bke::pbvh::Type::BMesh)
  {
    return;
  }

  SculptAttributeParams params;
  params.permanent = params.stroke_only = params.simple_array = false;

  ss.attrs.topology_island_key = BKE_sculpt_attribute_ensure(
      &ob,
      bke::AttrDomain::Point,
      CD_PROP_INT8,
      SCULPT_ATTRIBUTE_NAME(topology_island_key),
      &params);
  SCULPT_vertex_random_access_ensure(ss);

  int totvert = SCULPT_vertex_count_get(ss);
  Set<PBVHVertRef> visit;
  Vector<PBVHVertRef> stack;
  uint8_t island_nr = 0;

  for (int i = 0; i < totvert; i++) {
    PBVHVertRef vertex = BKE_pbvh_index_to_vertex(*ss.pbvh, i);

    if (visit.contains(vertex)) {
      continue;
    }

    stack.clear();
    stack.append(vertex);
    visit.add(vertex);

    while (stack.size()) {
      PBVHVertRef vertex2 = stack.pop_last();
      SculptVertexNeighborIter ni;

      *static_cast<uint8_t *>(
          SCULPT_vertex_attr_get(vertex2, ss.attrs.topology_island_key)) = island_nr;

      SCULPT_VERTEX_DUPLICATES_AND_NEIGHBORS_ITER_BEGIN (ss, vertex2, ni) {
        if (visit.add(ni.vertex) && hide::vert_any_face_visible_get(ss, ni.vertex)) {
          stack.append(ni.vertex);
        }
      }
      SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);
    }

    island_nr++;
  }

  ss.islands_valid = true;
}

}  // namespace blender::ed::sculpt_paint::islands

void SCULPT_cube_tip_init(const Sculpt & /*sd*/,
                          const Object &ob,
                          const Brush &brush,
                          float mat[4][4])
{
  using namespace blender::ed::sculpt_paint;
  SculptSession &ss = *ob.sculpt;
  float scale[4][4];
  float tmat[4][4];
  float unused[4][4];

  zero_m4(mat);
  calc_brush_local_mat(0.0, ob, unused, mat);

  /* NOTE: we ignore the radius scaling done inside of calc_brush_local_mat to
   * duplicate prior behavior.
   *
   * TODO: try disabling this and check that all edge cases work properly.
   */
  normalize_m4(mat);

  scale_m4_fl(scale, ss.cache->radius);
  mul_m4_m4m4(tmat, mat, scale);
  mul_v3_fl(tmat[1], brush.tip_scale_x);
  invert_m4_m4(mat, tmat);
}
/** \} */

namespace blender::ed::sculpt_paint {

void gather_grids_positions(const CCGKey &key,
                            const Span<CCGElem *> elems,
                            const Span<int> grids,
                            const MutableSpan<float3> positions)
{
  BLI_assert(grids.size() * key.grid_area == positions.size());

  for (const int i : grids.index_range()) {
    CCGElem *elem = elems[grids[i]];
    const int start = i * key.grid_area;
    for (const int offset : IndexRange(key.grid_area)) {
      positions[start + offset] = CCG_elem_offset_co(key, elem, offset);
    }
  }
}

void gather_bmesh_positions(const Set<BMVert *, 0> &verts, const MutableSpan<float3> positions)
{
  BLI_assert(verts.size() == positions.size());

  int i = 0;
  for (const BMVert *vert : verts) {
    positions[i] = vert->co;
    i++;
  }
}

void gather_grids_normals(const SubdivCCG &subdiv_ccg,
                          const Span<int> grids,
                          const MutableSpan<float3> normals)
{
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const Span<CCGElem *> elems = subdiv_ccg.grids;
  BLI_assert(grids.size() * key.grid_area == normals.size());

  for (const int i : grids.index_range()) {
    CCGElem *elem = elems[grids[i]];
    const int start = i * key.grid_area;
    for (const int offset : IndexRange(key.grid_area)) {
      normals[start + offset] = CCG_elem_offset_no(key, elem, offset);
    }
  }
}

void gather_bmesh_normals(const Set<BMVert *, 0> &verts, const MutableSpan<float3> normals)
{
  int i = 0;
  for (const BMVert *vert : verts) {
    normals[i] = vert->no;
    i++;
  }
}

template<typename T>
void gather_data_mesh(const Span<T> src, const Span<int> indices, const MutableSpan<T> dst)
{
  BLI_assert(indices.size() == dst.size());

  for (const int i : indices.index_range()) {
    dst[i] = src[indices[i]];
  }
}

template<typename T>
void gather_data_grids(const SubdivCCG &subdiv_ccg,
                       const Span<T> src,
                       const Span<int> grids,
                       const MutableSpan<T> node_data)
{
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  BLI_assert(grids.size() * key.grid_area == node_data.size());

  for (const int i : grids.index_range()) {
    const int node_start = i * key.grid_area;
    const int grids_start = grids[i] * key.grid_area;
    node_data.slice(node_start, key.grid_area).copy_from(src.slice(grids_start, key.grid_area));
  }
}

template<typename T>
void gather_data_vert_bmesh(const Span<T> src,
                            const Set<BMVert *, 0> &verts,
                            const MutableSpan<T> node_data)
{
  BLI_assert(verts.size() == node_data.size());

  int i = 0;
  for (const BMVert *vert : verts) {
    node_data[i] = src[BM_elem_index_get(vert)];
    i++;
  }
}

template<typename T>
void scatter_data_mesh(const Span<T> src, const Span<int> indices, const MutableSpan<T> dst)
{
  BLI_assert(indices.size() == src.size());

  for (const int i : indices.index_range()) {
    dst[indices[i]] = src[i];
  }
}

template<typename T>
void scatter_data_grids(const SubdivCCG &subdiv_ccg,
                        const Span<T> node_data,
                        const Span<int> grids,
                        const MutableSpan<T> dst)
{
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  BLI_assert(grids.size() * key.grid_area == node_data.size());

  for (const int i : grids.index_range()) {
    const int node_start = i * key.grid_area;
    const int grids_start = grids[i] * key.grid_area;
    dst.slice(grids_start, key.grid_area).copy_from(node_data.slice(node_start, key.grid_area));
  }
}

template<typename T>
void scatter_data_vert_bmesh(const Span<T> node_data,
                             const Set<BMVert *, 0> &verts,
                             const MutableSpan<T> dst)
{
  BLI_assert(verts.size() == node_data.size());

  int i = 0;
  for (const BMVert *vert : verts) {
    dst[BM_elem_index_get(vert)] = node_data[i];
    i++;
  }
}

template void gather_data_mesh<float>(Span<float>, Span<int>, MutableSpan<float>);
template void gather_data_mesh<float3>(Span<float3>, Span<int>, MutableSpan<float3>);
template void gather_data_mesh<float4>(Span<float4>, Span<int>, MutableSpan<float4>);
template void gather_data_grids<float>(const SubdivCCG &,
                                       Span<float>,
                                       Span<int>,
                                       MutableSpan<float>);
template void gather_data_grids<float3>(const SubdivCCG &,
                                        Span<float3>,
                                        Span<int>,
                                        MutableSpan<float3>);
template void gather_data_vert_bmesh<float>(Span<float>,
                                            const Set<BMVert *, 0> &,
                                            MutableSpan<float>);
template void gather_data_vert_bmesh<float3>(Span<float3>,
                                             const Set<BMVert *, 0> &,
                                             MutableSpan<float3>);

template void scatter_data_mesh<float>(Span<float>, Span<int>, MutableSpan<float>);
template void scatter_data_mesh<float3>(Span<float3>, Span<int>, MutableSpan<float3>);
template void scatter_data_mesh<float4>(Span<float4>, Span<int>, MutableSpan<float4>);
template void scatter_data_grids<float>(const SubdivCCG &,
                                        Span<float>,
                                        Span<int>,
                                        MutableSpan<float>);
template void scatter_data_grids<float3>(const SubdivCCG &,
                                         Span<float3>,
                                         Span<int>,
                                         MutableSpan<float3>);
template void scatter_data_vert_bmesh<float>(Span<float>,
                                             const Set<BMVert *, 0> &,
                                             MutableSpan<float>);
template void scatter_data_vert_bmesh<float3>(Span<float3>,
                                              const Set<BMVert *, 0> &,
                                              MutableSpan<float3>);

void fill_factor_from_hide(const Mesh &mesh,
                           const Span<int> verts,
                           const MutableSpan<float> r_factors)
{
  BLI_assert(verts.size() == r_factors.size());

  /* TODO: Avoid overhead of accessing attributes for every bke::pbvh::Tree node. */
  const bke::AttributeAccessor attributes = mesh.attributes();
  if (const VArray hide_vert = *attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point)) {
    const VArraySpan span(hide_vert);
    for (const int i : verts.index_range()) {
      r_factors[i] = span[verts[i]] ? 0.0f : 1.0f;
    }
  }
  else {
    r_factors.fill(1.0f);
  }
}

void fill_factor_from_hide(const SubdivCCG &subdiv_ccg,
                           const Span<int> grids,
                           const MutableSpan<float> r_factors)
{
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  BLI_assert(grids.size() * key.grid_area == r_factors.size());

  const BitGroupVector<> &grid_hidden = subdiv_ccg.grid_hidden;
  if (grid_hidden.is_empty()) {
    r_factors.fill(1.0f);
    return;
  }
  for (const int i : grids.index_range()) {
    const BitSpan hidden = grid_hidden[grids[i]];
    const int start = i * key.grid_area;
    for (const int offset : IndexRange(key.grid_area)) {
      r_factors[start + offset] = hidden[offset] ? 0.0f : 1.0f;
    }
  }
}

void fill_factor_from_hide(const Set<BMVert *, 0> &verts, const MutableSpan<float> r_factors)
{
  BLI_assert(verts.size() == r_factors.size());

  int i = 0;
  for (const BMVert *vert : verts) {
    r_factors[i] = BM_elem_flag_test_bool(vert, BM_ELEM_HIDDEN) ? 0.0f : 1.0f;
    i++;
  }
}

void fill_factor_from_hide_and_mask(const Mesh &mesh,
                                    const Span<int> verts,
                                    const MutableSpan<float> r_factors)
{
  BLI_assert(verts.size() == r_factors.size());

  /* TODO: Avoid overhead of accessing attributes for every bke::pbvh::Tree node. */
  const bke::AttributeAccessor attributes = mesh.attributes();
  if (const VArray mask = *attributes.lookup<float>(".sculpt_mask", bke::AttrDomain::Point)) {
    const VArraySpan span(mask);
    for (const int i : verts.index_range()) {
      r_factors[i] = 1.0f - span[verts[i]];
    }
  }
  else {
    r_factors.fill(1.0f);
  }

  if (const VArray hide_vert = *attributes.lookup<bool>(".hide_vert", bke::AttrDomain::Point)) {
    const VArraySpan span(hide_vert);
    for (const int i : verts.index_range()) {
      if (span[verts[i]]) {
        r_factors[i] = 0.0f;
      }
    }
  }
}

void fill_factor_from_hide_and_mask(const BMesh &bm,
                                    const Set<BMVert *, 0> &verts,
                                    const MutableSpan<float> r_factors)
{
  BLI_assert(verts.size() == r_factors.size());

  /* TODO: Avoid overhead of accessing attributes for every bke::pbvh::Tree node. */
  const int mask_offset = CustomData_get_offset_named(&bm.vdata, CD_PROP_FLOAT, ".sculpt_mask");
  int i = 0;
  for (const BMVert *vert : verts) {
    r_factors[i] = (mask_offset == -1) ? 1.0f : 1.0f - BM_ELEM_CD_GET_FLOAT(vert, mask_offset);
    if (BM_elem_flag_test(vert, BM_ELEM_HIDDEN)) {
      r_factors[i] = 0.0f;
    }
    i++;
  }
}

void fill_factor_from_hide_and_mask(const SubdivCCG &subdiv_ccg,
                                    const Span<int> grids,
                                    const MutableSpan<float> r_factors)
{
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const Span<CCGElem *> elems = subdiv_ccg.grids;
  BLI_assert(grids.size() * key.grid_area == r_factors.size());

  if (key.has_mask) {
    for (const int i : grids.index_range()) {
      CCGElem *elem = elems[grids[i]];
      const int start = i * key.grid_area;
      for (const int offset : IndexRange(key.grid_area)) {
        r_factors[start + offset] = 1.0f - CCG_elem_offset_mask(key, elem, offset);
      }
    }
  }
  else {
    r_factors.fill(1.0f);
  }

  const BitGroupVector<> &grid_hidden = subdiv_ccg.grid_hidden;
  if (!grid_hidden.is_empty()) {
    for (const int i : grids.index_range()) {
      const BitSpan hidden = grid_hidden[grids[i]];
      const int start = i * key.grid_area;
      for (const int offset : IndexRange(key.grid_area)) {
        if (hidden[offset]) {
          r_factors[start + offset] = 0.0f;
        }
      }
    }
  }
}

void calc_front_face(const float3 &view_normal,
                     const Span<float3> vert_normals,
                     const Span<int> verts,
                     const MutableSpan<float> factors)
{
  BLI_assert(verts.size() == factors.size());

  for (const int i : verts.index_range()) {
    const float dot = math::dot(view_normal, vert_normals[verts[i]]);
    factors[i] *= std::max(dot, 0.0f);
  }
}

void calc_front_face(const float3 &view_normal,
                     const Span<float3> normals,
                     const MutableSpan<float> factors)
{
  BLI_assert(normals.size() == factors.size());

  for (const int i : normals.index_range()) {
    const float dot = math::dot(view_normal, normals[i]);
    factors[i] *= std::max(dot, 0.0f);
  }
}
void calc_front_face(const float3 &view_normal,
                     const SubdivCCG &subdiv_ccg,
                     const Span<int> grids,
                     const MutableSpan<float> factors)
{
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const Span<CCGElem *> elems = subdiv_ccg.grids;
  BLI_assert(grids.size() * key.grid_area == factors.size());

  for (const int i : grids.index_range()) {
    CCGElem *elem = elems[grids[i]];
    const int start = i * key.grid_area;
    for (const int offset : IndexRange(key.grid_area)) {
      const float dot = math::dot(view_normal, CCG_elem_offset_no(key, elem, offset));
      factors[start + offset] *= std::max(dot, 0.0f);
    }
  }
}

void calc_front_face(const float3 &view_normal,
                     const Set<BMVert *, 0> &verts,
                     const MutableSpan<float> factors)
{
  BLI_assert(verts.size() == factors.size());

  int i = 0;
  for (const BMVert *vert : verts) {
    const float dot = math::dot(view_normal, float3(vert->no));
    factors[i] *= std::max(dot, 0.0f);
    i++;
  }
}

void calc_front_face(const float3 &view_normal,
                     const Set<BMFace *, 0> &faces,
                     const MutableSpan<float> factors)
{
  BLI_assert(faces.size() == factors.size());

  int i = 0;
  for (const BMFace *face : faces) {
    const float dot = math::dot(view_normal, float3(face->no));
    factors[i] *= std::max(dot, 0.0f);
    i++;
  }
}

void filter_region_clip_factors(const SculptSession &ss,
                                const Span<float3> positions,
                                const Span<int> verts,
                                const MutableSpan<float> factors)
{
  BLI_assert(verts.size() == factors.size());

  const RegionView3D *rv3d = ss.cache ? ss.cache->vc->rv3d : ss.rv3d;
  const View3D *v3d = ss.cache ? ss.cache->vc->v3d : ss.v3d;
  if (!RV3D_CLIPPING_ENABLED(v3d, rv3d)) {
    return;
  }

  const ePaintSymmetryFlags mirror_symmetry_pass = ss.cache ? ss.cache->mirror_symmetry_pass :
                                                              ePaintSymmetryFlags(0);
  const int radial_symmetry_pass = ss.cache ? ss.cache->radial_symmetry_pass : 0;
  const float4x4 symm_rot_mat_inv = ss.cache ? ss.cache->symm_rot_mat_inv : float4x4::identity();
  for (const int i : verts.index_range()) {
    float3 symm_co = symmetry_flip(positions[verts[i]], mirror_symmetry_pass);
    if (radial_symmetry_pass) {
      symm_co = math::transform_point(symm_rot_mat_inv, symm_co);
    }
    if (ED_view3d_clipping_test(rv3d, symm_co, true)) {
      factors[i] = 0.0f;
    }
  }
}

void filter_region_clip_factors(const SculptSession &ss,
                                const Span<float3> positions,
                                const MutableSpan<float> factors)
{
  BLI_assert(positions.size() == factors.size());

  const RegionView3D *rv3d = ss.cache ? ss.cache->vc->rv3d : ss.rv3d;
  const View3D *v3d = ss.cache ? ss.cache->vc->v3d : ss.v3d;
  if (!RV3D_CLIPPING_ENABLED(v3d, rv3d)) {
    return;
  }

  const ePaintSymmetryFlags mirror_symmetry_pass = ss.cache ? ss.cache->mirror_symmetry_pass :
                                                              ePaintSymmetryFlags(0);
  const int radial_symmetry_pass = ss.cache ? ss.cache->radial_symmetry_pass : 0;
  const float4x4 symm_rot_mat_inv = ss.cache ? ss.cache->symm_rot_mat_inv : float4x4::identity();
  for (const int i : positions.index_range()) {
    float3 symm_co = symmetry_flip(positions[i], mirror_symmetry_pass);
    if (radial_symmetry_pass) {
      symm_co = math::transform_point(symm_rot_mat_inv, symm_co);
    }
    if (ED_view3d_clipping_test(rv3d, symm_co, true)) {
      factors[i] = 0.0f;
    }
  }
}

void calc_brush_distances(const SculptSession &ss,
                          const Span<float3> positions,
                          const Span<int> verts,
                          const eBrushFalloffShape falloff_shape,
                          const MutableSpan<float> r_distances)
{
  BLI_assert(verts.size() == r_distances.size());

  const float3 &test_location = ss.cache ? ss.cache->location : ss.cursor_location;
  if (falloff_shape == PAINT_FALLOFF_SHAPE_TUBE && (ss.cache || ss.filter_cache)) {
    /* The tube falloff shape requires the cached view normal. */
    const float3 &view_normal = ss.cache ? ss.cache->view_normal : ss.filter_cache->view_normal;
    float4 test_plane;
    plane_from_point_normal_v3(test_plane, test_location, view_normal);
    for (const int i : verts.index_range()) {
      float3 projected;
      closest_to_plane_normalized_v3(projected, test_plane, positions[verts[i]]);
      r_distances[i] = math::distance(projected, test_location);
    }
  }
  else {
    for (const int i : verts.index_range()) {
      r_distances[i] = math::distance(test_location, positions[verts[i]]);
    }
  }
}

void calc_brush_distances(const SculptSession &ss,
                          const Span<float3> positions,
                          const eBrushFalloffShape falloff_shape,
                          const MutableSpan<float> r_distances)
{
  BLI_assert(positions.size() == r_distances.size());

  const float3 &test_location = ss.cache ? ss.cache->location : ss.cursor_location;
  if (falloff_shape == PAINT_FALLOFF_SHAPE_TUBE && (ss.cache || ss.filter_cache)) {
    /* The tube falloff shape requires the cached view normal. */
    const float3 &view_normal = ss.cache ? ss.cache->view_normal : ss.filter_cache->view_normal;
    float4 test_plane;
    plane_from_point_normal_v3(test_plane, test_location, view_normal);
    for (const int i : positions.index_range()) {
      float3 projected;
      closest_to_plane_normalized_v3(projected, test_plane, positions[i]);
      r_distances[i] = math::distance(projected, test_location);
    }
  }
  else {
    for (const int i : positions.index_range()) {
      r_distances[i] = math::distance(test_location, positions[i]);
    }
  }
}

void filter_distances_with_radius(const float radius,
                                  const Span<float> distances,
                                  const MutableSpan<float> factors)
{
  for (const int i : distances.index_range()) {
    if (distances[i] > radius) {
      factors[i] = 0.0f;
    }
  }
}

void calc_brush_cube_distances(const SculptSession &ss,
                               const Brush &brush,
                               const float4x4 &mat,
                               const Span<float3> positions,
                               const Span<int> verts,
                               const MutableSpan<float> r_distances,
                               const MutableSpan<float> factors)
{
  BLI_assert(verts.size() == factors.size());
  BLI_assert(verts.size() == r_distances.size());

  SculptBrushTest test;
  SCULPT_brush_test_init(ss, test);
  const float tip_roundness = brush.tip_roundness;
  const float tip_scale_x = brush.tip_scale_x;
  for (const int i : verts.index_range()) {
    if (factors[i] == 0.0f) {
      r_distances[i] = FLT_MAX;
      continue;
    }
    /* TODO: Break up #SCULPT_brush_test_cube. */
    if (!SCULPT_brush_test_cube(test, positions[verts[i]], mat.ptr(), tip_roundness, tip_scale_x))
    {
      factors[i] = 0.0f;
      r_distances[i] = FLT_MAX;
    }
    r_distances[i] = test.dist;
  }
}

void calc_brush_cube_distances(const SculptSession &ss,
                               const Brush &brush,
                               const float4x4 &mat,
                               const Span<float3> positions,
                               const MutableSpan<float> r_distances,
                               const MutableSpan<float> factors)
{
  BLI_assert(positions.size() == factors.size());
  BLI_assert(positions.size() == r_distances.size());

  SculptBrushTest test;
  SCULPT_brush_test_init(ss, test);
  const float tip_roundness = brush.tip_roundness;
  const float tip_scale_x = brush.tip_scale_x;
  for (const int i : positions.index_range()) {
    if (factors[i] == 0.0f) {
      r_distances[i] = FLT_MAX;
      continue;
    }
    /* TODO: Break up #SCULPT_brush_test_cube. */
    if (!SCULPT_brush_test_cube(test, positions[i], mat.ptr(), tip_roundness, tip_scale_x)) {
      factors[i] = 0.0f;
      r_distances[i] = FLT_MAX;
    }
    r_distances[i] = test.dist;
  }
}

void apply_hardness_to_distances(const float radius,
                                 const float hardness,
                                 const MutableSpan<float> distances)
{
  if (hardness == 0.0f) {
    return;
  }
  if (hardness == 1.0f) {
    distances.fill(0.0f);
    return;
  }
  const float threshold = hardness * radius;
  const float radius_inv = math::rcp(radius);
  const float hardness_inv_rcp = math::rcp(1.0f - hardness);
  for (const int i : distances.index_range()) {
    if (distances[i] < threshold) {
      distances[i] = 0.0f;
    }
    else {
      const float radius_factor = (distances[i] * radius_inv - hardness) * hardness_inv_rcp;
      distances[i] = radius_factor * radius;
    }
  }
}

void calc_brush_strength_factors(const StrokeCache &cache,
                                 const Brush &brush,
                                 const Span<float> distances,
                                 const MutableSpan<float> factors)
{
  BKE_brush_calc_curve_factors(
      eBrushCurvePreset(brush.curve_preset), brush.curve, distances, cache.radius, factors);
}

void calc_brush_texture_factors(const SculptSession &ss,
                                const Brush &brush,
                                const Span<float3> vert_positions,
                                const Span<int> verts,
                                const MutableSpan<float> factors)
{
  BLI_assert(verts.size() == factors.size());

  const int thread_id = BLI_task_parallel_thread_id(nullptr);
  const MTex *mtex = BKE_brush_mask_texture_get(&brush, OB_MODE_SCULPT);
  if (!mtex->tex) {
    return;
  }

  for (const int i : verts.index_range()) {
    float texture_value;
    float4 texture_rgba;
    /* NOTE: This is not a thread-safe call. */
    sculpt_apply_texture(
        ss, brush, vert_positions[verts[i]], thread_id, &texture_value, texture_rgba);

    factors[i] *= texture_value;
  }
}

void calc_brush_texture_factors(const SculptSession &ss,
                                const Brush &brush,
                                const Span<float3> positions,
                                const MutableSpan<float> factors)
{
  BLI_assert(positions.size() == factors.size());

  const int thread_id = BLI_task_parallel_thread_id(nullptr);
  const MTex *mtex = BKE_brush_mask_texture_get(&brush, OB_MODE_SCULPT);
  if (!mtex->tex) {
    return;
  }

  for (const int i : positions.index_range()) {
    float texture_value;
    float4 texture_rgba;
    /* NOTE: This is not a thread-safe call. */
    sculpt_apply_texture(ss, brush, positions[i], thread_id, &texture_value, texture_rgba);

    factors[i] *= texture_value;
  }
}

void apply_translations(const Span<float3> translations,
                        const Span<int> verts,
                        const MutableSpan<float3> positions)
{
  BLI_assert(verts.size() == translations.size());

  for (const int i : verts.index_range()) {
    const int vert = verts[i];
    positions[vert] += translations[i];
  }
}

void apply_translations(const Span<float3> translations,
                        const Span<int> grids,
                        SubdivCCG &subdiv_ccg)
{
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const Span<CCGElem *> elems = subdiv_ccg.grids;
  BLI_assert(grids.size() * key.grid_area == translations.size());

  for (const int i : grids.index_range()) {
    CCGElem *elem = elems[grids[i]];
    const int start = i * key.grid_area;
    for (const int offset : IndexRange(key.grid_area)) {
      CCG_elem_offset_co(key, elem, offset) += translations[start + offset];
    }
  }
}

void apply_translations(const Span<float3> translations, const Set<BMVert *, 0> &verts)
{
  BLI_assert(verts.size() == translations.size());

  int i = 0;
  for (BMVert *vert : verts) {
    add_v3_v3(vert->co, translations[i]);
    i++;
  }
}

void project_translations(const MutableSpan<float3> translations, const float3 &plane)
{
  /* Equivalent to #project_plane_v3_v3v3. */
  const float len_sq = math::length_squared(plane);
  if (len_sq < std::numeric_limits<float>::epsilon()) {
    return;
  }
  const float dot_factor = -math::rcp(len_sq);
  for (const int i : translations.index_range()) {
    translations[i] += plane * math::dot(translations[i], plane) * dot_factor;
  }
}

void apply_crazyspace_to_translations(const Span<float3x3> deform_imats,
                                      const Span<int> verts,
                                      const MutableSpan<float3> translations)
{
  BLI_assert(verts.size() == translations.size());

  for (const int i : verts.index_range()) {
    translations[i] = math::transform_point(deform_imats[verts[i]], translations[i]);
  }
}

void clip_and_lock_translations(const Sculpt &sd,
                                const SculptSession &ss,
                                const Span<float3> positions,
                                const Span<int> verts,
                                const MutableSpan<float3> translations)
{
  BLI_assert(verts.size() == translations.size());

  const StrokeCache *cache = ss.cache;
  if (!cache) {
    return;
  }
  for (const int axis : IndexRange(3)) {
    if (sd.flags & (SCULPT_LOCK_X << axis)) {
      for (float3 &translation : translations) {
        translation[axis] = 0.0f;
      }
      continue;
    }

    if (!(cache->flag & (CLIP_X << axis))) {
      continue;
    }

    const float4x4 mirror(cache->clip_mirror_mtx);
    const float4x4 mirror_inverse = math::invert(mirror);
    for (const int i : verts.index_range()) {
      const int vert = verts[i];

      /* Transform into the space of the mirror plane, check translations, then transform back. */
      float3 co_mirror = math::transform_point(mirror, positions[vert]);
      if (math::abs(co_mirror[axis]) > cache->clip_tolerance[axis]) {
        continue;
      }
      /* Clear the translation in the local space of the mirror object. */
      co_mirror[axis] = 0.0f;
      const float3 co_local = math::transform_point(mirror_inverse, co_mirror);
      translations[i][axis] = co_local[axis] - positions[vert][axis];
    }
  }
}

void clip_and_lock_translations(const Sculpt &sd,
                                const SculptSession &ss,
                                const Span<float3> positions,
                                const MutableSpan<float3> translations)
{
  BLI_assert(positions.size() == translations.size());

  const StrokeCache *cache = ss.cache;
  if (!cache) {
    return;
  }
  for (const int axis : IndexRange(3)) {
    if (sd.flags & (SCULPT_LOCK_X << axis)) {
      for (float3 &translation : translations) {
        translation[axis] = 0.0f;
      }
      continue;
    }

    if (!(cache->flag & (CLIP_X << axis))) {
      continue;
    }

    const float4x4 mirror(cache->clip_mirror_mtx);
    const float4x4 mirror_inverse = math::invert(mirror);
    for (const int i : positions.index_range()) {
      /* Transform into the space of the mirror plane, check translations, then transform back. */
      float3 co_mirror = math::transform_point(mirror, positions[i]);
      if (math::abs(co_mirror[axis]) > cache->clip_tolerance[axis]) {
        continue;
      }
      /* Clear the translation in the local space of the mirror object. */
      co_mirror[axis] = 0.0f;
      const float3 co_local = math::transform_point(mirror_inverse, co_mirror);
      translations[i][axis] = co_local[axis] - positions[i][axis];
    }
  }
}

void apply_translations_to_shape_keys(Object &object,
                                      const Span<int> verts,
                                      const Span<float3> translations,
                                      const MutableSpan<float3> positions_orig)
{
  Mesh &mesh = *static_cast<Mesh *>(object.data);
  KeyBlock *active_key = BKE_keyblock_from_object(&object);
  if (!active_key) {
    return;
  }

  MutableSpan active_key_data(static_cast<float3 *>(active_key->data), active_key->totelem);
  if (active_key == mesh.key->refkey) {
    for (const int vert : verts) {
      active_key_data[vert] = positions_orig[vert];
    }
  }
  else {
    apply_translations(translations, verts, active_key_data);
  }

  /* For relative keys editing of base should update other keys. */
  if (bool *dependent = BKE_keyblock_get_dependent_keys(mesh.key, object.shapenr - 1)) {
    int i;
    LISTBASE_FOREACH_INDEX (KeyBlock *, other_key, &mesh.key->block, i) {
      if ((other_key != active_key) && dependent[i]) {
        MutableSpan<float3> data(static_cast<float3 *>(other_key->data), other_key->totelem);
        apply_translations(translations, verts, data);
      }
    }
    MEM_freeN(dependent);
  }
}

void apply_translations_to_pbvh(bke::pbvh::Tree &pbvh,
                                Span<int> verts,
                                const Span<float3> translations)
{
  if (!BKE_pbvh_is_deformed(pbvh)) {
    return;
  }
  MutableSpan<float3> pbvh_positions = BKE_pbvh_get_vert_positions(pbvh);
  for (const int i : verts.index_range()) {
    const int vert = verts[i];
    pbvh_positions[vert] += translations[i];
  }
}

void write_translations(const Sculpt &sd,
                        Object &object,
                        const Span<float3> positions_eval,
                        const Span<int> verts,
                        const MutableSpan<float3> translations,
                        const MutableSpan<float3> positions_orig)
{
  SculptSession &ss = *object.sculpt;

  clip_and_lock_translations(sd, ss, positions_eval, verts, translations);

  apply_translations_to_pbvh(*ss.pbvh, verts, translations);

  if (!ss.deform_imats.is_empty()) {
    apply_crazyspace_to_translations(ss.deform_imats, verts, translations);
  }

  apply_translations(translations, verts, positions_orig);
  apply_translations_to_shape_keys(object, verts, translations, positions_orig);
}

void scale_translations(const MutableSpan<float3> translations, const Span<float> factors)
{
  for (const int i : translations.index_range()) {
    translations[i] *= factors[i];
  }
}

void scale_translations(const MutableSpan<float3> translations, const float factor)
{
  if (factor == 1.0f) {
    return;
  }
  for (const int i : translations.index_range()) {
    translations[i] *= factor;
  }
}

void scale_factors(const MutableSpan<float> factors, const float strength)
{
  if (strength == 1.0f) {
    return;
  }
  for (float &factor : factors) {
    factor *= strength;
  }
}

void scale_factors(const MutableSpan<float> factors, const Span<float> strengths)
{
  BLI_assert(factors.size() == strengths.size());

  for (const int i : factors.index_range()) {
    factors[i] *= strengths[i];
  }
}

void translations_from_offset_and_factors(const float3 &offset,
                                          const Span<float> factors,
                                          const MutableSpan<float3> r_translations)
{
  BLI_assert(r_translations.size() == factors.size());

  for (const int i : factors.index_range()) {
    r_translations[i] = offset * factors[i];
  }
}

void translations_from_new_positions(const Span<float3> new_positions,
                                     const Span<int> verts,
                                     const Span<float3> old_positions,
                                     const MutableSpan<float3> translations)
{
  BLI_assert(new_positions.size() == verts.size());
  for (const int i : verts.index_range()) {
    translations[i] = new_positions[i] - old_positions[verts[i]];
  }
}

void translations_from_new_positions(const Span<float3> new_positions,
                                     const Span<float3> old_positions,
                                     const MutableSpan<float3> translations)
{
  BLI_assert(new_positions.size() == old_positions.size());
  for (const int i : new_positions.index_range()) {
    translations[i] = new_positions[i] - old_positions[i];
  }
}

void transform_positions(const Span<float3> src,
                         const float4x4 &transform,
                         const MutableSpan<float3> dst)
{
  BLI_assert(src.size() == dst.size());

  for (const int i : src.index_range()) {
    dst[i] = math::transform_point(transform, src[i]);
  }
}

void transform_positions(const float4x4 &transform, const MutableSpan<float3> positions)
{
  for (const int i : positions.index_range()) {
    positions[i] = math::transform_point(transform, positions[i]);
  }
}

OffsetIndices<int> create_node_vert_offsets(Span<bke::pbvh::Node *> nodes, Array<int> &node_data)
{
  node_data.reinitialize(nodes.size() + 1);
  for (const int i : nodes.index_range()) {
    node_data[i] = bke::pbvh::node_unique_verts(*nodes[i]).size();
  }
  return offset_indices::accumulate_counts_to_offsets(node_data);
}

OffsetIndices<int> create_node_vert_offsets(Span<bke::pbvh::Node *> nodes,
                                            const CCGKey &key,
                                            Array<int> &node_data)
{
  node_data.reinitialize(nodes.size() + 1);
  for (const int i : nodes.index_range()) {
    node_data[i] = bke::pbvh::node_grid_indices(*nodes[i]).size() * key.grid_area;
  }
  return offset_indices::accumulate_counts_to_offsets(node_data);
}

OffsetIndices<int> create_node_vert_offsets_bmesh(Span<bke::pbvh::Node *> nodes,
                                                  Array<int> &node_data)
{
  node_data.reinitialize(nodes.size() + 1);
  for (const int i : nodes.index_range()) {
    node_data[i] = BKE_pbvh_bmesh_node_unique_verts(nodes[i]).size();
  }
  return offset_indices::accumulate_counts_to_offsets(node_data);
}

void calc_vert_neighbors(const OffsetIndices<int> faces,
                         const Span<int> corner_verts,
                         const GroupedSpan<int> vert_to_face,
                         const Span<bool> hide_poly,
                         const Span<int> verts,
                         const MutableSpan<Vector<int>> result)
{
  BLI_assert(result.size() == verts.size());
  BLI_assert(corner_verts.size() == faces.total_size());
  for (const int i : verts.index_range()) {
    vert_neighbors_get_mesh(verts[i], faces, corner_verts, vert_to_face, hide_poly, result[i]);
  }
}

void calc_vert_neighbors_interior(const OffsetIndices<int> faces,
                                  const Span<int> corner_verts,
                                  const GroupedSpan<int> vert_to_face,
                                  const BitSpan boundary_verts,
                                  const Span<bool> hide_poly,
                                  const Span<int> verts,
                                  const MutableSpan<Vector<int>> result)
{
  BLI_assert(result.size() == verts.size());
  BLI_assert(corner_verts.size() == faces.total_size());

  for (const int i : verts.index_range()) {
    const int vert = verts[i];
    Vector<int> &neighbors = result[i];
    vert_neighbors_get_mesh(verts[i], faces, corner_verts, vert_to_face, hide_poly, neighbors);

    if (boundary_verts[vert]) {
      if (neighbors.size() == 2) {
        /* Do not include neighbors of corner vertices. */
        neighbors.clear();
      }
      else {
        /* Only include other boundary vertices as neighbors of boundary vertices. */
        neighbors.remove_if([&](const int vert) { return !boundary_verts[vert]; });
      }
    }
  }
}

void calc_vert_neighbors_interior(const OffsetIndices<int> faces,
                                  const Span<int> corner_verts,
                                  const BitSpan boundary_verts,
                                  const SubdivCCG &subdiv_ccg,
                                  const Span<int> grids,
                                  const MutableSpan<Vector<SubdivCCGCoord>> result)
{
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);

  BLI_assert(grids.size() * key.grid_area == result.size());

  for (const int i : grids.index_range()) {
    const int grid = grids[i];
    const int node_verts_start = i * key.grid_area;

    /* TODO: This loop could be optimized in the future by skipping unnecessary logic for
     * non-boundary grid vertices. */
    for (const int y : IndexRange(key.grid_size)) {
      for (const int x : IndexRange(key.grid_size)) {
        const int offset = CCG_grid_xy_to_index(key.grid_size, x, y);
        const int node_vert_index = node_verts_start + offset;

        SubdivCCGCoord coord{};
        coord.grid_index = grid;
        coord.x = x;
        coord.y = y;

        SubdivCCGNeighbors neighbors;
        BKE_subdiv_ccg_neighbor_coords_get(subdiv_ccg, coord, false, neighbors);

        if (BKE_subdiv_ccg_coord_is_mesh_boundary(
                faces, corner_verts, boundary_verts, subdiv_ccg, coord))
        {
          if (neighbors.coords.size() == 2) {
            /* Do not include neighbors of corner vertices. */
            neighbors.coords.clear();
          }
          else {
            /* Only include other boundary vertices as neighbors of boundary vertices. */
            neighbors.coords.remove_if([&](const SubdivCCGCoord coord) {
              return !BKE_subdiv_ccg_coord_is_mesh_boundary(
                  faces, corner_verts, boundary_verts, subdiv_ccg, coord);
            });
          }
        }
        result[node_vert_index] = neighbors.coords;
      }
    }
  }
}

void calc_vert_neighbors_interior(const Set<BMVert *, 0> &verts,
                                  MutableSpan<Vector<BMVert *>> result)
{
  BLI_assert(verts.size() == result.size());
  Vector<BMVert *, 64> neighbor_data;

  int i = 0;
  for (BMVert *vert : verts) {
    neighbor_data.clear();
    vert_neighbors_get_interior_bmesh(*vert, neighbor_data);
    result[i] = neighbor_data;
    i++;
  }
}

void calc_translations_to_plane(const Span<float3> vert_positions,
                                const Span<int> verts,
                                const float4 &plane,
                                const MutableSpan<float3> translations)
{
  for (const int i : verts.index_range()) {
    const float3 &position = vert_positions[verts[i]];
    float3 closest;
    closest_to_plane_normalized_v3(closest, plane, position);
    translations[i] = closest - position;
  }
}

void calc_translations_to_plane(const Span<float3> positions,
                                const float4 &plane,
                                const MutableSpan<float3> translations)
{
  for (const int i : positions.index_range()) {
    const float3 &position = positions[i];
    float3 closest;
    closest_to_plane_normalized_v3(closest, plane, position);
    translations[i] = closest - position;
  }
}

void filter_plane_trim_limit_factors(const Brush &brush,
                                     const StrokeCache &cache,
                                     const Span<float3> translations,
                                     const MutableSpan<float> factors)
{
  if (!(brush.flag & BRUSH_PLANE_TRIM)) {
    return;
  }
  const float threshold = cache.radius_squared * cache.plane_trim_squared;
  for (const int i : translations.index_range()) {
    if (math::length_squared(translations[i]) <= threshold) {
      factors[i] = 0.0f;
    }
  }
}

void filter_below_plane_factors(const Span<float3> vert_positions,
                                const Span<int> verts,
                                const float4 &plane,
                                const MutableSpan<float> factors)
{
  for (const int i : verts.index_range()) {
    if (plane_point_side_v3(plane, vert_positions[verts[i]]) <= 0.0f) {
      factors[i] = 0.0f;
    }
  }
}

void filter_below_plane_factors(const Span<float3> positions,
                                const float4 &plane,
                                const MutableSpan<float> factors)
{
  for (const int i : positions.index_range()) {
    if (plane_point_side_v3(plane, positions[i]) <= 0.0f) {
      factors[i] = 0.0f;
    }
  }
}

void filter_above_plane_factors(const Span<float3> vert_positions,
                                const Span<int> verts,
                                const float4 &plane,
                                const MutableSpan<float> factors)
{
  for (const int i : verts.index_range()) {
    if (plane_point_side_v3(plane, vert_positions[verts[i]]) > 0.0f) {
      factors[i] = 0.0f;
    }
  }
}

void filter_above_plane_factors(const Span<float3> positions,
                                const float4 &plane,
                                const MutableSpan<float> factors)
{
  for (const int i : positions.index_range()) {
    if (plane_point_side_v3(plane, positions[i]) > 0.0f) {
      factors[i] = 0.0f;
    }
  }
}

}  // namespace blender::ed::sculpt_paint
