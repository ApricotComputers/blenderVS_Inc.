/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include <fmt/format.h>

#include "DNA_windowmanager_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_enumerable_thread_specific.hh"
#include "BLI_hash.h"
#include "BLI_index_range.hh"
#include "BLI_math_base.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_task.h"

#include "BLT_translation.hh"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_layer.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_pbvh_api.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_screen.hh"
#include "ED_sculpt.hh"
#include "ED_view3d.hh"

#include "mesh_brush_common.hh"
#include "paint_intern.hh"
#include "sculpt_intern.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "UI_interface.hh"
#include "UI_resources.hh"

#include "bmesh.hh"

#include <cmath>
#include <cstdlib>

namespace blender::ed::sculpt_paint::filter {

float3 to_orientation_space(const filter::Cache &filter_cache, const float3 &vector)
{
  switch (filter_cache.orientation) {
    case FilterOrientation::Local:
      /* Do nothing, Sculpt Mode already works in object space. */
      return vector;
    case FilterOrientation::World:
      return math::transform_point(float3x3(filter_cache.obmat), vector);
      break;
    case FilterOrientation::View: {
      const float3 world_space = math::transform_point(float3x3(filter_cache.obmat), vector);
      return math::transform_point(float3x3(filter_cache.viewmat), world_space);
    }
  }
  BLI_assert_unreachable();
  return {};
}

float3 to_object_space(const filter::Cache &filter_cache, const float3 &vector)
{
  switch (filter_cache.orientation) {
    case FilterOrientation::Local:
      /* Do nothing, Sculpt Mode already works in object space. */
      return vector;
    case FilterOrientation::World:
      return math::transform_point(float3x3(filter_cache.obmat_inv), vector);
    case FilterOrientation::View: {
      const float3 world_space = math::transform_point(float3x3(filter_cache.viewmat_inv), vector);
      return math::transform_point(float3x3(filter_cache.obmat_inv), world_space);
    }
  }
  BLI_assert_unreachable();
  return {};
}

float3 zero_disabled_axis_components(const filter::Cache &filter_cache, const float3 &vector)
{
  float3 v = to_orientation_space(filter_cache, vector);
  for (int axis = 0; axis < 3; axis++) {
    if (!filter_cache.enabled_force_axis[axis]) {
      v[axis] = 0.0f;
    }
  }
  return to_object_space(filter_cache, v);
}

void cache_init(bContext *C,
                Object &ob,
                const Sculpt &sd,
                const undo::Type undo_type,
                const float mval_fl[2],
                float area_normal_radius,
                float start_strength)
{
  SculptSession &ss = *ob.sculpt;
  bke::pbvh::Tree &pbvh = *ss.pbvh;

  ss.filter_cache = MEM_new<filter::Cache>(__func__);
  ss.filter_cache->start_filter_strength = start_strength;
  ss.filter_cache->random_seed = rand();

  if (undo_type == undo::Type::Color) {
    const Mesh &mesh = *static_cast<const Mesh *>(ob.data);
    BKE_pbvh_ensure_node_loops(pbvh, mesh.corner_tris());
  }

  ss.filter_cache->nodes = bke::pbvh::search_gather(
      pbvh, [&](bke::pbvh::Node &node) { return !node_fully_masked_or_hidden(node); });

  undo::push_nodes(ob, ss.filter_cache->nodes, undo_type);

  /* Setup orientation matrices. */
  copy_m4_m4(ss.filter_cache->obmat.ptr(), ob.object_to_world().ptr());
  invert_m4_m4(ss.filter_cache->obmat_inv.ptr(), ob.object_to_world().ptr());

  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(C);
  ViewContext vc = ED_view3d_viewcontext_init(C, depsgraph);

  ss.filter_cache->vc = vc;
  if (vc.rv3d) {
    copy_m4_m4(ss.filter_cache->viewmat.ptr(), vc.rv3d->viewmat);
    copy_m4_m4(ss.filter_cache->viewmat_inv.ptr(), vc.rv3d->viewinv);
  }

  Scene *scene = CTX_data_scene(C);
  UnifiedPaintSettings *ups = &scene->toolsettings->unified_paint_settings;

  float3 co;

  if (vc.rv3d && SCULPT_stroke_get_location(C, co, mval_fl, false)) {
    Vector<bke::pbvh::Node *> nodes;

    /* Get radius from brush. */
    const Brush *brush = BKE_paint_brush_for_read(&sd.paint);
    float radius;

    if (brush) {
      if (BKE_brush_use_locked_size(scene, brush)) {
        radius = paint_calc_object_space_radius(
            vc, co, float(BKE_brush_size_get(scene, brush) * area_normal_radius));
      }
      else {
        radius = BKE_brush_unprojected_radius_get(scene, brush) * area_normal_radius;
      }
    }
    else {
      radius = paint_calc_object_space_radius(vc, co, float(ups->size) * area_normal_radius);
    }

    const float radius_sq = math::square(radius);
    nodes = bke::pbvh::search_gather(pbvh, [&](bke::pbvh::Node &node) {
      return !node_fully_masked_or_hidden(node) && node_in_sphere(node, co, radius_sq, true);
    });

    const std::optional<float3> area_normal = calc_area_normal(*brush, ob, nodes);
    if (BKE_paint_brush_for_read(&sd.paint) && area_normal) {
      ss.filter_cache->initial_normal = *area_normal;
      ss.last_normal = ss.filter_cache->initial_normal;
    }
    else {
      ss.filter_cache->initial_normal = ss.last_normal;
    }

    /* Update last stroke location */

    mul_m4_v3(ob.object_to_world().ptr(), co);

    add_v3_v3(ups->average_stroke_accum, co);
    ups->average_stroke_counter++;
    ups->last_stroke_valid = true;
  }
  else {
    /* Use last normal. */
    copy_v3_v3(ss.filter_cache->initial_normal, ss.last_normal);
  }

  /* Update view normal */
  float3x3 mat;
  float3 viewDir{0.0f, 0.0f, 1.0f};
  if (vc.rv3d) {
    invert_m4_m4(ob.runtime->world_to_object.ptr(), ob.object_to_world().ptr());
    copy_m3_m4(mat.ptr(), vc.rv3d->viewinv);
    mul_m3_v3(mat.ptr(), viewDir);
    copy_m3_m4(mat.ptr(), ob.world_to_object().ptr());
    mul_m3_v3(mat.ptr(), viewDir);
    normalize_v3_v3(ss.filter_cache->view_normal, viewDir);
  }
}

enum class MeshFilterType {
  Smooth = 0,
  Scale = 1,
  Inflate = 2,
  Sphere = 3,
  Random = 4,
  Relax = 5,
  RelaxFaceSets = 6,
  SurfaceSmooth = 7,
  Sharpen = 8,
  EnhanceDetails = 9,
  EraseDispacement = 10,
};

static EnumPropertyItem prop_mesh_filter_types[] = {
    {int(MeshFilterType::Smooth), "SMOOTH", 0, "Smooth", "Smooth mesh"},
    {int(MeshFilterType::Scale), "SCALE", 0, "Scale", "Scale mesh"},
    {int(MeshFilterType::Inflate), "INFLATE", 0, "Inflate", "Inflate mesh"},
    {int(MeshFilterType::Sphere), "SPHERE", 0, "Sphere", "Morph into sphere"},
    {int(MeshFilterType::Random), "RANDOM", 0, "Random", "Randomize vertex positions"},
    {int(MeshFilterType::Relax), "RELAX", 0, "Relax", "Relax mesh"},
    {int(MeshFilterType::RelaxFaceSets),
     "RELAX_FACE_SETS",
     0,
     "Relax Face Sets",
     "Smooth the edges of all the Face Sets"},
    {int(MeshFilterType::SurfaceSmooth),
     "SURFACE_SMOOTH",
     0,
     "Surface Smooth",
     "Smooth the surface of the mesh, preserving the volume"},
    {int(MeshFilterType::Sharpen), "SHARPEN", 0, "Sharpen", "Sharpen the cavities of the mesh"},
    {int(MeshFilterType::EnhanceDetails),
     "ENHANCE_DETAILS",
     0,
     "Enhance Details",
     "Enhance the high frequency surface detail"},
    {int(MeshFilterType::EraseDispacement),
     "ERASE_DISCPLACEMENT",
     0,
     "Erase Displacement",
     "Deletes the displacement of the Multires Modifier"},
    {0, nullptr, 0, nullptr, nullptr},
};

enum eMeshFilterDeformAxis {
  MESH_FILTER_DEFORM_X = 1 << 0,
  MESH_FILTER_DEFORM_Y = 1 << 1,
  MESH_FILTER_DEFORM_Z = 1 << 2,
};

static EnumPropertyItem prop_mesh_filter_deform_axis_items[] = {
    {MESH_FILTER_DEFORM_X, "X", 0, "X", "Deform in the X axis"},
    {MESH_FILTER_DEFORM_Y, "Y", 0, "Y", "Deform in the Y axis"},
    {MESH_FILTER_DEFORM_Z, "Z", 0, "Z", "Deform in the Z axis"},
    {0, nullptr, 0, nullptr, nullptr},
};

static EnumPropertyItem prop_mesh_filter_orientation_items[] = {
    {int(FilterOrientation::Local),
     "LOCAL",
     0,
     "Local",
     "Use the local axis to limit the displacement"},
    {int(FilterOrientation::World),
     "WORLD",
     0,
     "World",
     "Use the global axis to limit the displacement"},
    {int(FilterOrientation::View),
     "VIEW",
     0,
     "View",
     "Use the view axis to limit the displacement"},
    {0, nullptr, 0, nullptr, nullptr},
};

static bool sculpt_mesh_filter_needs_pmap(MeshFilterType filter_type)
{
  return ELEM(filter_type,
              MeshFilterType::Smooth,
              MeshFilterType::Relax,
              MeshFilterType::RelaxFaceSets,
              MeshFilterType::SurfaceSmooth,
              MeshFilterType::EnhanceDetails,
              MeshFilterType::Sharpen);
}

static bool sculpt_mesh_filter_is_continuous(MeshFilterType type)
{
  return ELEM(type,
              MeshFilterType::Sharpen,
              MeshFilterType::Smooth,
              MeshFilterType::Relax,
              MeshFilterType::RelaxFaceSets);
}

static void mesh_filter_task(Object &ob,
                             const MeshFilterType filter_type,
                             const float filter_strength,
                             bke::pbvh::Node *node)
{
  SculptSession &ss = *ob.sculpt;

  SculptOrigVertData orig_data = SCULPT_orig_vert_data_init(ob, *node, undo::Type::Position);

  /* When using the relax face sets meshes filter,
   * each 3 iterations, do a whole mesh relax to smooth the contents of the Face Set. */
  /* This produces better results as the relax operation is no completely focused on the
   * boundaries. */
  const bool relax_face_sets = !(ss.filter_cache->iteration_count % 3 == 0);
  auto_mask::NodeData automask_data = auto_mask::node_begin(
      ob, ss.filter_cache->automasking.get(), *node);

  PBVHVertexIter vd;
  BKE_pbvh_vertex_iter_begin (*ss.pbvh, node, vd, PBVH_ITER_UNIQUE) {
    SCULPT_orig_vert_data_update(orig_data, vd);
    auto_mask::node_update(automask_data, vd);

    float3 orig_co, val, disp, disp2, final_pos;
    float3x3 transform;
    float fade = vd.mask;
    fade = 1.0f - fade;
    fade *= filter_strength;
    fade *= auto_mask::factor_get(
        ss.filter_cache->automasking.get(), ss, vd.vertex, &automask_data);

    if (fade == 0.0f && filter_type != MeshFilterType::SurfaceSmooth) {
      /* Surface Smooth can't skip the loop for this vertex as it needs to calculate its
       * laplacian_disp. This value is accessed from the vertex neighbors when deforming the
       * vertices, so it is needed for all vertices even if they are not going to be displaced.
       */
      continue;
    }

    if (ELEM(filter_type, MeshFilterType::Relax, MeshFilterType::RelaxFaceSets) ||
        ss.filter_cache->no_orig_co)
    {
      orig_co = vd.co;
    }
    else {
      orig_co = orig_data.co;
    }

    if (filter_type == MeshFilterType::RelaxFaceSets) {
      if (relax_face_sets == face_set::vert_has_unique_face_set(ss, vd.vertex)) {
        continue;
      }
    }

    switch (filter_type) {
      case MeshFilterType::Smooth: {
        fade = clamp_f(fade, -1.0f, 1.0f);
        const float3 avg = smooth::neighbor_coords_average_interior(ss, vd.vertex);
        val = avg - orig_co;
        val = orig_co + val * fade;
        disp = val - orig_co;
        break;
      }
      case MeshFilterType::Inflate:
        disp = float3(orig_data.no) * fade;
        break;
      case MeshFilterType::Scale:
        unit_m3(transform.ptr());
        scale_m3_fl(transform.ptr(), 1.0f + fade);
        val = orig_co;
        val = transform * val;
        disp = val - orig_co;
        break;
      case MeshFilterType::Sphere:
        disp = math::normalize(orig_co);
        disp *= math::abs(fade);

        unit_m3(transform.ptr());
        if (fade > 0.0f) {
          scale_m3_fl(transform.ptr(), 1.0f - fade);
        }
        else {
          scale_m3_fl(transform.ptr(), 1.0f + fade);
        }
        val = orig_co;
        val = transform * val;
        disp2 = val - orig_co;

        disp = math::midpoint(disp, disp2);
        break;
      case MeshFilterType::Random: {
        float3 normal;
        copy_v3_v3(normal, orig_data.no);
        /* Index is not unique for multi-resolution, so hash by vertex coordinates. */
        const uint *hash_co = (const uint *)&orig_co;
        const uint hash = BLI_hash_int_2d(hash_co[0], hash_co[1]) ^
                          BLI_hash_int_2d(hash_co[2], ss.filter_cache->random_seed);
        normal *= (hash * (1.0f / float(0xFFFFFFFF)) - 0.5f);
        disp = normal * fade;
        break;
      }
      case MeshFilterType::Relax: {
        smooth::relax_vertex(ss, &vd, clamp_f(fade, 0.0f, 1.0f), false, val);
        disp = val - float3(vd.co);
        break;
      }
      case MeshFilterType::RelaxFaceSets: {
        smooth::relax_vertex(ss, &vd, clamp_f(fade, 0.0f, 1.0f), relax_face_sets, val);
        disp = val - float3(vd.co);
        break;
      }
      case MeshFilterType::SurfaceSmooth: {
        smooth::surface_smooth_laplacian_step(ss,
                                              disp,
                                              vd.co,
                                              ss.filter_cache->surface_smooth_laplacian_disp,
                                              vd.vertex,
                                              orig_data.co,
                                              ss.filter_cache->surface_smooth_shape_preservation);
        break;
      }
      case MeshFilterType::Sharpen: {
        const float smooth_ratio = ss.filter_cache->sharpen_smooth_ratio;

        /* This filter can't work at full strength as it needs multiple iterations to reach a
         * stable state. */
        fade = clamp_f(fade, 0.0f, 0.5f);
        float3 disp_sharpen(0.0f);

        SculptVertexNeighborIter ni;
        SCULPT_VERTEX_NEIGHBORS_ITER_BEGIN (ss, vd.vertex, ni) {
          float3 disp_n = float3(SCULPT_vertex_co_get(ss, ni.vertex)) -
                          float3(SCULPT_vertex_co_get(ss, vd.vertex));
          disp_n *= ss.filter_cache->sharpen_factor[ni.index];
          disp_sharpen += disp_n;
        }
        SCULPT_VERTEX_NEIGHBORS_ITER_END(ni);

        disp_sharpen *= (1.0f - ss.filter_cache->sharpen_factor[vd.index]);

        const float3 avg_co = smooth::neighbor_coords_average(ss, vd.vertex);
        float3 disp_avg = avg_co - float3(vd.co);
        disp_avg = disp_avg * smooth_ratio * pow2f(ss.filter_cache->sharpen_factor[vd.index]);
        disp = disp_avg + disp_sharpen;
        /* Intensify details. */
        if (ss.filter_cache->sharpen_intensify_detail_strength > 0.0f) {
          float3 detail_strength = ss.filter_cache->detail_directions[vd.index];
          disp += detail_strength * -ss.filter_cache->sharpen_intensify_detail_strength *
                  ss.filter_cache->sharpen_factor[vd.index];
        }
        break;
      }

      case MeshFilterType::EnhanceDetails: {
        disp = ss.filter_cache->detail_directions[vd.index] * -fabsf(fade);
        break;
      }
      case MeshFilterType::EraseDispacement: {
        fade = clamp_f(fade, -1.0f, 1.0f);
        disp = ss.filter_cache->limit_surface_co[vd.index] - orig_co;
        disp *= fade;
        break;
      }
    }

    disp = to_orientation_space(*ss.filter_cache, disp);
    for (int it = 0; it < 3; it++) {
      if (!ss.filter_cache->enabled_axis[it]) {
        disp[it] = 0.0f;
      }
    }
    disp = to_object_space(*ss.filter_cache, disp);

    if (ELEM(filter_type, MeshFilterType::SurfaceSmooth, MeshFilterType::Sharpen)) {
      final_pos = float3(vd.co) + disp * clamp_f(fade, 0.0f, 1.0f);
    }
    else {
      final_pos = orig_co + disp;
    }
    copy_v3_v3(vd.co, final_pos);
  }
  BKE_pbvh_vertex_iter_end;

  BKE_pbvh_node_mark_positions_update(node);
}

static void mesh_filter_surface_smooth_init(SculptSession &ss,
                                            const float shape_preservation,
                                            const float current_vertex_displacement)
{
  const int totvert = SCULPT_vertex_count_get(ss);
  filter::Cache *filter_cache = ss.filter_cache;

  filter_cache->surface_smooth_laplacian_disp.reinitialize(totvert);
  filter_cache->surface_smooth_shape_preservation = shape_preservation;
  filter_cache->surface_smooth_current_vertex = current_vertex_displacement;
}

static void mesh_filter_init_limit_surface_co(SculptSession &ss)
{
  const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const Span<CCGElem *> elems = subdiv_ccg.grids;

  ss.filter_cache->limit_surface_co = Array<float3>(elems.size() * key.grid_area);
  MutableSpan<float3> limit_positions = ss.filter_cache->limit_surface_co;
  threading::parallel_for(elems.index_range(), 512, [&](const IndexRange range) {
    for (const int grid : range) {
      const int start = grid * key.grid_area;
      BKE_subdiv_ccg_eval_limit_positions(
          subdiv_ccg, key, grid, limit_positions.slice(start, key.grid_area));
    }
  });
}

static void mesh_filter_sharpen_init(const Object &object,
                                     const float smooth_ratio,
                                     const float intensify_detail_strength,
                                     const int curvature_smooth_iterations,
                                     filter::Cache &filter_cache)
{
  const SculptSession &ss = *object.sculpt;
  const bke::pbvh::Tree &pbvh = *ss.pbvh;
  const Span<bke::pbvh::Node *> nodes = filter_cache.nodes;
  const int totvert = SCULPT_vertex_count_get(ss);

  filter_cache.sharpen_smooth_ratio = smooth_ratio;
  filter_cache.sharpen_intensify_detail_strength = intensify_detail_strength;
  filter_cache.sharpen_curvature_smooth_iterations = curvature_smooth_iterations;
  filter_cache.sharpen_factor.reinitialize(totvert);
  filter_cache.detail_directions.reinitialize(totvert);
  MutableSpan<float3> detail_directions = filter_cache.detail_directions;
  MutableSpan<float> sharpen_factors = filter_cache.sharpen_factor;

  calc_smooth_translations(object, filter_cache.nodes, filter_cache.detail_directions);

  for (int i = 0; i < totvert; i++) {
    sharpen_factors[i] = math::length(detail_directions[i]);
  }

  float max_factor = 0.0f;
  for (int i = 0; i < totvert; i++) {
    if (sharpen_factors[i] > max_factor) {
      max_factor = sharpen_factors[i];
    }
  }

  max_factor = 1.0f / max_factor;
  for (int i = 0; i < totvert; i++) {
    sharpen_factors[i] *= max_factor;
    sharpen_factors[i] = 1.0f - pow2f(1.0f - sharpen_factors[i]);
  }

  /* Smooth the calculated factors and directions to remove high frequency detail. */
  struct LocalData {
    Vector<Vector<int>> vert_neighbors;
    Vector<float3> smooth_directions;
    Vector<float> smooth_factors;
  };
  threading::EnumerableThreadSpecific<LocalData> all_tls;
  for ([[maybe_unused]] const int _ : IndexRange(filter_cache.sharpen_curvature_smooth_iterations))
  {
    switch (pbvh.type()) {
      case bke::pbvh::Type::Mesh: {
        Mesh &mesh = *static_cast<Mesh *>(object.data);
        const OffsetIndices faces = mesh.faces();
        const Span<int> corner_verts = mesh.corner_verts();
        threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
          LocalData &tls = all_tls.local();
          for (const int i : range) {
            const Span<int> verts = bke::pbvh::node_unique_verts(*nodes[i]);

            tls.vert_neighbors.resize(verts.size());
            const MutableSpan<Vector<int>> neighbors = tls.vert_neighbors;
            calc_vert_neighbors(faces, corner_verts, ss.vert_to_face_map, {}, verts, neighbors);

            tls.smooth_directions.resize(verts.size());
            smooth::neighbor_data_average_mesh(
                detail_directions.as_span(), neighbors, tls.smooth_directions.as_mutable_span());
            scatter_data_mesh(tls.smooth_directions.as_span(), verts, detail_directions);

            tls.smooth_factors.resize(verts.size());
            smooth::neighbor_data_average_mesh(
                sharpen_factors.as_span(), neighbors, tls.smooth_factors.as_mutable_span());
            scatter_data_mesh(tls.smooth_factors.as_span(), verts, sharpen_factors);
          }
        });
        break;
      }
      case bke::pbvh::Type::Grids: {
        SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
        const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
        threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
          LocalData &tls = all_tls.local();
          for (const int i : range) {
            const Span<int> grids = bke::pbvh::node_grid_indices(*nodes[i]);
            const int grid_verts_num = grids.size() * key.grid_area;

            tls.smooth_directions.resize(grid_verts_num);
            smooth::average_data_grids(subdiv_ccg,
                                       detail_directions.as_span(),
                                       grids,
                                       tls.smooth_directions.as_mutable_span());
            scatter_data_grids(
                subdiv_ccg, tls.smooth_directions.as_span(), grids, detail_directions);

            tls.smooth_factors.resize(grid_verts_num);
            smooth::average_data_grids(subdiv_ccg,
                                       sharpen_factors.as_span(),
                                       grids,
                                       tls.smooth_factors.as_mutable_span());
            scatter_data_grids(subdiv_ccg, tls.smooth_factors.as_span(), grids, sharpen_factors);
          }
        });
        break;
      }
      case bke::pbvh::Type::BMesh:
        threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
          LocalData &tls = all_tls.local();
          for (const int i : range) {
            const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(nodes[i]);

            tls.smooth_directions.resize(verts.size());
            smooth::average_data_bmesh(
                detail_directions.as_span(), verts, tls.smooth_directions.as_mutable_span());
            scatter_data_vert_bmesh(tls.smooth_directions.as_span(), verts, detail_directions);

            tls.smooth_factors.resize(verts.size());
            smooth::average_data_bmesh(
                sharpen_factors.as_span(), verts, tls.smooth_factors.as_mutable_span());
            scatter_data_vert_bmesh(tls.smooth_factors.as_span(), verts, sharpen_factors);
          }
        });
        break;
    }
  }
}

static void mesh_filter_surface_smooth_displace_task(Object &ob,
                                                     const float filter_strength,
                                                     bke::pbvh::Node *node)
{
  SculptSession &ss = *ob.sculpt;
  PBVHVertexIter vd;

  auto_mask::NodeData automask_data = auto_mask::node_begin(
      ob, ss.filter_cache->automasking.get(), *node);

  BKE_pbvh_vertex_iter_begin (*ss.pbvh, node, vd, PBVH_ITER_UNIQUE) {
    auto_mask::node_update(automask_data, vd);

    float fade = vd.mask;
    fade = 1.0f - fade;
    fade *= filter_strength;
    fade *= auto_mask::factor_get(
        ss.filter_cache->automasking.get(), ss, vd.vertex, &automask_data);
    if (fade == 0.0f) {
      continue;
    }

    smooth::surface_smooth_displace_step(ss,
                                         vd.co,
                                         ss.filter_cache->surface_smooth_laplacian_disp,
                                         vd.vertex,
                                         ss.filter_cache->surface_smooth_current_vertex,
                                         clamp_f(fade, 0.0f, 1.0f));
  }
  BKE_pbvh_vertex_iter_end;
}

enum {
  FILTER_MESH_MODAL_CANCEL = 1,
  FILTER_MESH_MODAL_CONFIRM,
};

wmKeyMap *modal_keymap(wmKeyConfig *keyconf)
{
  static const EnumPropertyItem modal_items[] = {
      {FILTER_MESH_MODAL_CANCEL, "CANCEL", 0, "Cancel", ""},
      {FILTER_MESH_MODAL_CONFIRM, "CONFIRM", 0, "Confirm", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  wmKeyMap *keymap = WM_modalkeymap_find(keyconf, "Mesh Filter Modal Map");

  /* This function is called for each space-type, only needs to add map once. */
  if (keymap && keymap->modal_items) {
    return nullptr;
  }

  keymap = WM_modalkeymap_ensure(keyconf, "Mesh Filter Modal Map", modal_items);

  WM_modalkeymap_assign(keymap, "SCULPT_OT_mesh_filter");

  return keymap;
}

static void sculpt_mesh_update_status_bar(bContext *C, wmOperator * /*op*/)
{
  WorkspaceStatus status(C);
  status.item(IFACE_("Confirm"), ICON_EVENT_RETURN);
  status.item(IFACE_("Cancel"), ICON_EVENT_ESC, ICON_MOUSE_RMB);
}

static void sculpt_mesh_filter_apply(bContext *C, wmOperator *op)
{
  Object &ob = *CTX_data_active_object(C);
  SculptSession &ss = *ob.sculpt;
  const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;
  MeshFilterType filter_type = MeshFilterType(RNA_enum_get(op->ptr, "type"));
  float filter_strength = RNA_float_get(op->ptr, "strength");

  SCULPT_vertex_random_access_ensure(ss);

  const Span<bke::pbvh::Node *> nodes = ss.filter_cache->nodes;

  /* The relax mesh filter needs updated normals. */
  if (ELEM(filter_type, MeshFilterType::Relax, MeshFilterType::RelaxFaceSets)) {
    bke::pbvh::update_normals(*ss.pbvh, ss.subdiv_ccg);
  }

  threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
    for (const int i : range) {
      mesh_filter_task(ob, MeshFilterType(filter_type), filter_strength, nodes[i]);
      BKE_pbvh_node_mark_positions_update(nodes[i]);
    }
  });

  if (filter_type == MeshFilterType::SurfaceSmooth) {
    threading::parallel_for(nodes.index_range(), 1, [&](const IndexRange range) {
      for (const int i : range) {
        mesh_filter_surface_smooth_displace_task(ob, filter_strength, nodes[i]);
        BKE_pbvh_node_mark_positions_update(nodes[i]);
      }
    });
  }

  ss.filter_cache->iteration_count++;

  if (ss.deform_modifiers_active || ss.shapekey_active) {
    SCULPT_flush_stroke_deform(sd, ob, true);
  }

  flush_update_step(C, UpdateType::Position);
}

static void sculpt_mesh_update_strength(wmOperator *op,
                                        SculptSession &ss,
                                        float2 prev_press_mouse,
                                        float2 mouse)
{
  const float len = prev_press_mouse[0] - mouse[0];

  float filter_strength = ss.filter_cache->start_filter_strength * -len * 0.001f * UI_SCALE_FAC;
  RNA_float_set(op->ptr, "strength", filter_strength);
}
static void sculpt_mesh_filter_apply_with_history(bContext *C, wmOperator *op)
{
  /* Event history is only stored for smooth and relax filters. */
  if (!RNA_collection_length(op->ptr, "event_history")) {
    sculpt_mesh_filter_apply(C, op);
    return;
  }

  Object &ob = *CTX_data_active_object(C);
  SculptSession &ss = *ob.sculpt;
  float2 start_mouse;
  bool first = true;
  float initial_strength = ss.filter_cache->start_filter_strength;

  RNA_BEGIN (op->ptr, item, "event_history") {
    float2 mouse;
    RNA_float_get_array(&item, "mouse_event", mouse);

    if (first) {
      first = false;
      start_mouse = mouse;
      continue;
    }

    sculpt_mesh_update_strength(op, ss, start_mouse, mouse);
    sculpt_mesh_filter_apply(C, op);
  }
  RNA_END;

  RNA_float_set(op->ptr, "strength", initial_strength);
}

static void sculpt_mesh_filter_end(bContext *C)
{
  Object &ob = *CTX_data_active_object(C);
  SculptSession &ss = *ob.sculpt;

  MEM_delete(ss.filter_cache);
  ss.filter_cache = nullptr;
  flush_update_done(C, ob, UpdateType::Position);
}

static int sculpt_mesh_filter_confirm(SculptSession &ss,
                                      wmOperator *op,
                                      const MeshFilterType filter_type)
{

  float initial_strength = ss.filter_cache->start_filter_strength;
  /* Don't update strength property if we're storing an event history. */
  if (sculpt_mesh_filter_is_continuous(filter_type)) {
    RNA_float_set(op->ptr, "strength", initial_strength);
  }

  return OPERATOR_FINISHED;
}

static void sculpt_mesh_filter_cancel(bContext *C, wmOperator * /*op*/)
{
  Object &ob = *CTX_data_active_object(C);
  SculptSession *ss = ob.sculpt;

  if (!ss || !ss->pbvh) {
    return;
  }

  undo::restore_position_from_undo_step(ob);

  bke::pbvh::update_bounds(*ss->pbvh);
}

static int sculpt_mesh_filter_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  Object &ob = *CTX_data_active_object(C);
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  SculptSession &ss = *ob.sculpt;
  const MeshFilterType filter_type = MeshFilterType(RNA_enum_get(op->ptr, "type"));

  WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_EW_SCROLL);
  sculpt_mesh_update_status_bar(C, op);

  if (event->type == EVT_MODAL_MAP) {
    int ret = OPERATOR_FINISHED;
    switch (event->val) {
      case FILTER_MESH_MODAL_CANCEL:
        sculpt_mesh_filter_cancel(C, op);
        undo::push_end_ex(ob, true);
        ret = OPERATOR_CANCELLED;
        break;

      case FILTER_MESH_MODAL_CONFIRM:
        ret = sculpt_mesh_filter_confirm(ss, op, filter_type);
        undo::push_end_ex(ob, false);
        break;
    }

    sculpt_mesh_filter_end(C);
    ED_workspace_status_text(C, nullptr); /* Clear status bar */
    WM_cursor_modal_restore(CTX_wm_window(C));

    return ret;
  }

  if (event->type != MOUSEMOVE) {
    return OPERATOR_RUNNING_MODAL;
  }

  /* NOTE: some filter types are continuous, for these we store an
   * event history in RNA for continuous.
   * This way the user can tweak the last operator properties
   * or repeat the op and get expected results. */
  if (sculpt_mesh_filter_is_continuous(filter_type)) {
    if (RNA_collection_length(op->ptr, "event_history") == 0) {
      /* First entry is the start mouse position, event->prev_press_xy. */
      PointerRNA startptr;
      RNA_collection_add(op->ptr, "event_history", &startptr);

      float2 mouse_start(float(event->prev_press_xy[0]), float(event->prev_press_xy[1]));
      RNA_float_set_array(&startptr, "mouse_event", mouse_start);
    }

    PointerRNA itemptr;
    RNA_collection_add(op->ptr, "event_history", &itemptr);

    float2 mouse(float(event->xy[0]), float(event->xy[1]));
    RNA_float_set_array(&itemptr, "mouse_event", mouse);
    RNA_float_set(&itemptr, "pressure", WM_event_tablet_data(event, nullptr, nullptr));
  }

  float2 prev_mval(float(event->prev_press_xy[0]), float(event->prev_press_xy[1]));
  float2 mval(float(event->xy[0]), float(event->xy[1]));

  sculpt_mesh_update_strength(op, ss, prev_mval, mval);

  BKE_sculpt_update_object_for_edit(depsgraph, &ob, false);

  sculpt_mesh_filter_apply(C, op);

  return OPERATOR_RUNNING_MODAL;
}

static void sculpt_filter_specific_init(const MeshFilterType filter_type,
                                        wmOperator *op,
                                        Object &object)
{
  SculptSession &ss = *object.sculpt;
  switch (filter_type) {
    case MeshFilterType::SurfaceSmooth: {
      const float shape_preservation = RNA_float_get(op->ptr, "surface_smooth_shape_preservation");
      const float current_vertex_displacement = RNA_float_get(op->ptr,
                                                              "surface_smooth_current_vertex");
      mesh_filter_surface_smooth_init(ss, shape_preservation, current_vertex_displacement);
      break;
    }
    case MeshFilterType::Sharpen: {
      const float smooth_ratio = RNA_float_get(op->ptr, "sharpen_smooth_ratio");
      const float intensify_detail_strength = RNA_float_get(op->ptr,
                                                            "sharpen_intensify_detail_strength");
      const int curvature_smooth_iterations = RNA_int_get(op->ptr,
                                                          "sharpen_curvature_smooth_iterations");
      mesh_filter_sharpen_init(object,
                               smooth_ratio,
                               intensify_detail_strength,
                               curvature_smooth_iterations,
                               *ss.filter_cache);
      break;
    }
    case MeshFilterType::EnhanceDetails: {
      ss.filter_cache->detail_directions.reinitialize(SCULPT_vertex_count_get(ss));
      calc_smooth_translations(object, ss.filter_cache->nodes, ss.filter_cache->detail_directions);
      break;
    }
    case MeshFilterType::EraseDispacement: {
      mesh_filter_init_limit_surface_co(ss);
      break;
    }
    default:
      break;
  }
}

/* Returns OPERATOR_PASS_THROUGH on success. */
static int sculpt_mesh_filter_start(bContext *C, wmOperator *op)
{
  Object &ob = *CTX_data_active_object(C);
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  const Sculpt &sd = *CTX_data_tool_settings(C)->sculpt;

  const View3D *v3d = CTX_wm_view3d(C);
  const Base *base = CTX_data_active_base(C);
  if (!BKE_base_is_visible(v3d, base)) {
    return OPERATOR_CANCELLED;
  }

  int mval[2];
  RNA_int_get_array(op->ptr, "start_mouse", mval);

  const MeshFilterType filter_type = MeshFilterType(RNA_enum_get(op->ptr, "type"));
  const bool use_automasking = auto_mask::is_enabled(sd, nullptr, nullptr);
  const bool needs_topology_info = sculpt_mesh_filter_needs_pmap(filter_type) || use_automasking;

  BKE_sculpt_update_object_for_edit(depsgraph, &ob, false);

  if (ED_sculpt_report_if_shape_key_is_locked(ob, op->reports)) {
    return OPERATOR_CANCELLED;
  }

  SculptSession &ss = *ob.sculpt;

  const eMeshFilterDeformAxis deform_axis = eMeshFilterDeformAxis(
      RNA_enum_get(op->ptr, "deform_axis"));

  if (deform_axis == 0) {
    /* All axis are disabled, so the filter is not going to produce any deformation. */
    return OPERATOR_CANCELLED;
  }

  float2 mval_fl{float(mval[0]), float(mval[1])};
  if (use_automasking) {
    /* Increment stroke id for automasking system. */
    SCULPT_stroke_id_next(ob);

    /* Update the active face set manually as the paint cursor is not enabled when using the Mesh
     * Filter Tool. */
    SculptCursorGeometryInfo sgi;
    SCULPT_cursor_geometry_info_update(C, &sgi, mval_fl, false);
  }

  SCULPT_vertex_random_access_ensure(ss);
  if (needs_topology_info) {
    boundary::ensure_boundary_info(ob);
  }

  undo::push_begin(ob, op);

  cache_init(C,
             ob,
             sd,
             undo::Type::Position,
             mval_fl,
             RNA_float_get(op->ptr, "area_normal_radius"),
             RNA_float_get(op->ptr, "strength"));

  filter::Cache *filter_cache = ss.filter_cache;
  filter_cache->active_face_set = SCULPT_FACE_SET_NONE;
  filter_cache->automasking = auto_mask::cache_init(sd, ob);

  sculpt_filter_specific_init(filter_type, op, ob);

  ss.filter_cache->enabled_axis[0] = deform_axis & MESH_FILTER_DEFORM_X;
  ss.filter_cache->enabled_axis[1] = deform_axis & MESH_FILTER_DEFORM_Y;
  ss.filter_cache->enabled_axis[2] = deform_axis & MESH_FILTER_DEFORM_Z;

  FilterOrientation orientation = FilterOrientation(RNA_enum_get(op->ptr, "orientation"));
  ss.filter_cache->orientation = orientation;

  return OPERATOR_PASS_THROUGH;
}

static int sculpt_mesh_filter_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  RNA_int_set_array(op->ptr, "start_mouse", event->mval);
  int ret = sculpt_mesh_filter_start(C, op);

  if (ret == OPERATOR_PASS_THROUGH) {
    WM_event_add_modal_handler(C, op);
    return OPERATOR_RUNNING_MODAL;
  }

  return ret;
}

static int sculpt_mesh_filter_exec(bContext *C, wmOperator *op)
{
  int ret = sculpt_mesh_filter_start(C, op);

  if (ret == OPERATOR_PASS_THROUGH) {
    Object &ob = *CTX_data_active_object(C);
    SculptSession &ss = *ob.sculpt;

    int iterations = RNA_int_get(op->ptr, "iteration_count");
    bool has_history = RNA_collection_length(op->ptr, "event_history") > 0;

    if (!has_history) {
      ss.filter_cache->no_orig_co = true;
    }

    for (int i = 0; i < iterations; i++) {
      sculpt_mesh_filter_apply_with_history(C, op);

      ss.filter_cache->no_orig_co = true;
    }

    sculpt_mesh_filter_end(C);

    return OPERATOR_FINISHED;
  }

  return ret;
}

void register_operator_props(wmOperatorType *ot)
{
  RNA_def_int_array(
      ot->srna, "start_mouse", 2, nullptr, 0, 1 << 14, "Starting Mouse", "", 0, 1 << 14);

  RNA_def_float(
      ot->srna,
      "area_normal_radius",
      0.25,
      0.001,
      5.0,
      "Normal Radius",
      "Radius used for calculating area normal on initial click,\nin percentage of brush radius",
      0.01,
      1.0);
  RNA_def_float(
      ot->srna, "strength", 1.0f, -10.0f, 10.0f, "Strength", "Filter strength", -10.0f, 10.0f);
  RNA_def_int(ot->srna,
              "iteration_count",
              1,
              1,
              10000,
              "Repeat",
              "How many times to repeat the filter",
              1,
              100);

  /* Smooth filter requires entire event history. */
  PropertyRNA *prop = RNA_def_collection_runtime(
      ot->srna, "event_history", &RNA_OperatorStrokeElement, "", "");
  RNA_def_property_flag(prop, PropertyFlag(int(PROP_HIDDEN) | int(PROP_SKIP_SAVE)));
}

static void sculpt_mesh_ui_exec(bContext * /*C*/, wmOperator *op)
{
  uiLayout *layout = op->layout;

  uiItemR(layout, op->ptr, "strength", UI_ITEM_NONE, nullptr, ICON_NONE);
  uiItemR(layout, op->ptr, "iteration_count", UI_ITEM_NONE, nullptr, ICON_NONE);
  uiItemR(layout, op->ptr, "orientation", UI_ITEM_NONE, nullptr, ICON_NONE);
  layout = uiLayoutRow(layout, true);
  uiItemR(layout, op->ptr, "deform_axis", UI_ITEM_R_EXPAND, nullptr, ICON_NONE);
}

void SCULPT_OT_mesh_filter(wmOperatorType *ot)
{
  ot->name = "Filter Mesh";
  ot->idname = "SCULPT_OT_mesh_filter";
  ot->description = "Applies a filter to modify the current mesh";

  ot->invoke = sculpt_mesh_filter_invoke;
  ot->modal = sculpt_mesh_filter_modal;
  ot->poll = SCULPT_mode_poll;
  ot->exec = sculpt_mesh_filter_exec;
  ot->ui = sculpt_mesh_ui_exec;

  /* Doesn't seem to actually be called?
   * Check `sculpt_mesh_filter_modal` to see where it's really called. */
  ot->cancel = sculpt_mesh_filter_cancel;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_GRAB_CURSOR_X | OPTYPE_BLOCKING |
             OPTYPE_DEPENDS_ON_CURSOR;

  register_operator_props(ot);

  ot->prop = RNA_def_enum(ot->srna,
                          "type",
                          prop_mesh_filter_types,
                          int(MeshFilterType::Inflate),
                          "Filter Type",
                          "Operation that is going to be applied to the mesh");
  RNA_def_property_translation_context(ot->prop, BLT_I18NCONTEXT_OPERATOR_DEFAULT);
  RNA_def_enum_flag(ot->srna,
                    "deform_axis",
                    prop_mesh_filter_deform_axis_items,
                    MESH_FILTER_DEFORM_X | MESH_FILTER_DEFORM_Y | MESH_FILTER_DEFORM_Z,
                    "Deform Axis",
                    "Apply the deformation in the selected axis");
  RNA_def_enum(ot->srna,
               "orientation",
               prop_mesh_filter_orientation_items,
               int(FilterOrientation::Local),
               "Orientation",
               "Orientation of the axis to limit the filter displacement");

  /* Surface Smooth Mesh Filter properties. */
  RNA_def_float(ot->srna,
                "surface_smooth_shape_preservation",
                0.5f,
                0.0f,
                1.0f,
                "Shape Preservation",
                "How much of the original shape is preserved when smoothing",
                0.0f,
                1.0f);
  RNA_def_float(ot->srna,
                "surface_smooth_current_vertex",
                0.5f,
                0.0f,
                1.0f,
                "Per Vertex Displacement",
                "How much the position of each individual vertex influences the final result",
                0.0f,
                1.0f);
  RNA_def_float(ot->srna,
                "sharpen_smooth_ratio",
                0.35f,
                0.0f,
                1.0f,
                "Smooth Ratio",
                "How much smoothing is applied to polished surfaces",
                0.0f,
                1.0f);

  RNA_def_float(ot->srna,
                "sharpen_intensify_detail_strength",
                0.0f,
                0.0f,
                10.0f,
                "Intensify Details",
                "How much creases and valleys are intensified",
                0.0f,
                1.0f);

  RNA_def_int(ot->srna,
              "sharpen_curvature_smooth_iterations",
              0,
              0,
              10,
              "Curvature Smooth Iterations",
              "How much smooth the resulting shape is, ignoring high frequency details",
              0,
              10);
}

}  // namespace blender::ed::sculpt_paint::filter
