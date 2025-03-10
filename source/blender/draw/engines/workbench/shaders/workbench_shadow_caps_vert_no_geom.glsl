/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma BLENDER_REQUIRE(common_view_lib.glsl)
#pragma BLENDER_REQUIRE(gpu_shader_attribute_load_lib.glsl)
#pragma BLENDER_REQUIRE(gpu_shader_index_load_lib.glsl)

#ifdef DOUBLE_MANIFOLD
#  define vert_len 6 /* Triangle Strip with 6 verts = 4 triangles = 12 verts. */
#else
#  define vert_len 6 /* Triangle Strip with 6 verts = 4 triangles = 12 verts. */
#endif

struct VertexData {
  vec3 lP;            /* local position */
  vec4 frontPosition; /* final ndc position */
  vec4 backPosition;
};

/* Input geometry triangle list. */
VertexData vData[3];

#define DISCARD_VERTEX \
  gl_Position = vec4(0.0); \
  return;

vec4 get_pos(int v, bool backface)
{
  return (backface) ? vData[v].backPosition : vData[v].frontPosition;
}

vec3 extrude_offset(vec3 ls_P)
{
  vec3 ws_P = point_object_to_world(ls_P);
  float extrude_distance = 1e5f;
  float L_dot_FP = dot(pass_data.light_direction_ws, pass_data.far_plane.xyz);
  if (L_dot_FP > 0.0) {
    float signed_distance = dot(pass_data.far_plane.xyz, ws_P) - pass_data.far_plane.w;
    extrude_distance = -signed_distance / L_dot_FP;
    /* Ensure we don't overlap the far plane. */
    extrude_distance -= 1e-3f;
  }
  return pass_data.light_direction_ws * extrude_distance;
}

void emit_cap(const bool front, bool reversed, int triangle_vertex_id)
{
  /* Inverse. */
  ivec2 idx = (reversed) ? ivec2(2, 1) : ivec2(1, 2);

  /* Output position depending on vertex ID. */
  switch (triangle_vertex_id) {
    case 0: {
      gl_Position = (front) ? vData[0].frontPosition : vData[0].backPosition;
      break;
    }
    case 1: {
      gl_Position = (front) ? vData[idx.x].frontPosition : vData[idx.y].backPosition;
      break;
    }
    case 2: {
      gl_Position = (front) ? vData[idx.y].frontPosition : vData[idx.x].backPosition;
      break;
    }
  }

  /* Apply depth bias. Prevents Z-fighting artifacts when fast-math is enabled. */
  gl_Position.z += 0.00005;
}

void main()
{
  /* Output Data indexing. */
  int input_prim_index = int(gl_VertexID / 6);
  int output_vertex_id = gl_VertexID % 6;
  int output_triangle_id = output_vertex_id / 3;

  /* Source primitive data location derived from output primitive. */
  int input_base_index = input_prim_index * 3;

  /* In data is triangles - Should be guaranteed. */
  uint v0 = gpu_index_load(input_base_index + 0);
  uint v1 = gpu_index_load(input_base_index + 1);
  uint v2 = gpu_index_load(input_base_index + 2);

  /* Read input position data. */
  vData[0].lP = gpu_attr_load_float3(pos, gpu_attr_3, v0);
  vData[1].lP = gpu_attr_load_float3(pos, gpu_attr_3, v1);
  vData[2].lP = gpu_attr_load_float3(pos, gpu_attr_3, v2);

  /* Calculate front/back Positions. */
  vData[0].frontPosition = point_object_to_ndc(vData[0].lP);
  vData[0].backPosition = point_world_to_ndc(point_object_to_world(vData[0].lP) +
                                             extrude_offset(vData[0].lP));

  vData[1].frontPosition = point_object_to_ndc(vData[1].lP);
  vData[1].backPosition = point_world_to_ndc(point_object_to_world(vData[1].lP) +
                                             extrude_offset(vData[1].lP));

  vData[2].frontPosition = point_object_to_ndc(vData[2].lP);
  vData[2].backPosition = point_world_to_ndc(point_object_to_world(vData[2].lP) +
                                             extrude_offset(vData[2].lP));

  /* Geometry shader equivalent calc. */
  vec3 v10 = vData[0].lP - vData[1].lP;
  vec3 v12 = vData[2].lP - vData[1].lP;

  vec3 lightDirection = normal_world_to_object(vec3(pass_data.light_direction_ws));

  vec3 n = cross(v12, v10);
  float facing = dot(n, lightDirection);

  bool backface = facing > 0.0;

#ifdef DOUBLE_MANIFOLD
  /* In case of non manifold geom, we only increase/decrease
   * the stencil buffer by one but do every faces as they were facing the light. */
  bool invert = backface;
  const bool is_manifold = false;
#else
  const bool invert = false;
  const bool is_manifold = true;
#endif

  if (!is_manifold || !backface) {
    bool do_front = (output_triangle_id == 0) ? true : false;
    emit_cap(do_front, invert, output_vertex_id % 3);
  }
  else {
    DISCARD_VERTEX
  }
}
