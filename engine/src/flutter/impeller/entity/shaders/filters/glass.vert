// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <impeller/conversions.glsl>
#include <impeller/types.glsl>

// Only borrowed native framebuffer textures retain the embedder origin.
vec2 DenialRemapTextureCoords(vec2 coords, float y_scale) {
  return vec2(coords.x, y_scale < 0.0 ? 1.0 - coords.y : coords.y);
}

uniform FrameInfo {
  mat4 mvp;
  float blurred_sampler_y_coord_scale;
  float scene_sampler_y_coord_scale;
}
frame_info;

in vec2 position;
in vec2 blurred_texture_coords;
in vec2 scene_texture_coords;
in vec2 material_position;

out highp vec2 v_blurred_texture_coords;
out highp vec2 v_scene_texture_coords;
out highp vec2 v_material_position;

void main() {
  gl_Position = frame_info.mvp * vec4(position, 0.0, 1.0);
  v_blurred_texture_coords = DenialRemapTextureCoords(
      blurred_texture_coords, frame_info.blurred_sampler_y_coord_scale);
  v_scene_texture_coords = DenialRemapTextureCoords(
      scene_texture_coords, frame_info.scene_sampler_y_coord_scale);
  v_material_position = material_position;
}
