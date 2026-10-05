// Copyright 2026 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <flutter/runtime_effect.glsl>

uniform vec2 u_size;
uniform sampler2D u_texture;
uniform vec2 u_displacement;
uniform vec4 u_shape;

out vec4 frag_color;

void main() {
  vec2 position = FlutterFragCoord().xy;
  if (any(lessThan(position, u_shape.xy)) ||
      any(greaterThanEqual(position, u_shape.xy + u_shape.zw))) {
    frag_color = vec4(0.0);
    return;
  }
  frag_color = texture(u_texture, (position + u_displacement) / u_size);
}
