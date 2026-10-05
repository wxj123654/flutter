// Copyright 2026 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <dlfcn.h>

#include <array>
#include <cstring>

#include "EGL/egl.h"
#include "EGL/eglext.h"
#include "flutter/testing/testing.h"
#include "gtest/gtest.h"
#include "impeller/entity/contents/filters/filter_contents.h"
#include "impeller/entity/gles/entity_shaders_gles.h"
#include "impeller/entity/gles/framebuffer_blend_shaders_gles.h"
#include "impeller/entity/gles/modern_shaders_gles.h"
#include "impeller/renderer/backend/gles/context_gles.h"
#include "impeller/renderer/backend/gles/texture_gles.h"

namespace impeller {
namespace testing {
namespace {

class RuntimeFilterWorker : public ReactorGLES::Worker {
 public:
  bool CanReactorReactOnCurrentThreadNow(const ReactorGLES&) const override {
    return true;
  }
};

// No window or playground. Dynamically load EGL so the mock-only tests remain
// usable without a system EGL dependency. Failure to create EGL is not a pass.
class RuntimeFilterGLES : public ::testing::Test {
 public:
  void SetUp() override {
    egl_ = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    ASSERT_NE(egl_, nullptr);
#define LOAD_EGL(name, type)                               \
  name = reinterpret_cast<type>(dlsym(egl_, "egl" #name)); \
  ASSERT_NE(name, nullptr)
    LOAD_EGL(GetProcAddress, PFNEGLGETPROCADDRESSPROC);
    LOAD_EGL(Initialize, PFNEGLINITIALIZEPROC);
    LOAD_EGL(BindAPI, PFNEGLBINDAPIPROC);
    LOAD_EGL(ChooseConfig, PFNEGLCHOOSECONFIGPROC);
    LOAD_EGL(CreateContext, PFNEGLCREATECONTEXTPROC);
    LOAD_EGL(MakeCurrent, PFNEGLMAKECURRENTPROC);
    LOAD_EGL(DestroyContext, PFNEGLDESTROYCONTEXTPROC);
    LOAD_EGL(Terminate, PFNEGLTERMINATEPROC);
#undef LOAD_EGL
    auto get_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        GetProcAddress("eglGetPlatformDisplayEXT"));
    ASSERT_NE(get_display, nullptr);
    display_ = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY,
                           nullptr);
    ASSERT_NE(display_, EGL_NO_DISPLAY);
    ASSERT_TRUE(Initialize(display_, nullptr, nullptr));
    ASSERT_TRUE(BindAPI(EGL_OPENGL_ES_API));
    const EGLint config_attributes[] = {EGL_SURFACE_TYPE,
                                        EGL_PBUFFER_BIT,
                                        EGL_RENDERABLE_TYPE,
                                        EGL_OPENGL_ES2_BIT,
                                        EGL_RED_SIZE,
                                        8,
                                        EGL_GREEN_SIZE,
                                        8,
                                        EGL_BLUE_SIZE,
                                        8,
                                        EGL_ALPHA_SIZE,
                                        8,
                                        EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    ASSERT_TRUE(ChooseConfig(display_, config_attributes, &config, 1, &count));
    ASSERT_EQ(count, 1);
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3,
                                         EGL_NONE};
    egl_context_ =
        CreateContext(display_, config, EGL_NO_CONTEXT, context_attributes);
    ASSERT_NE(egl_context_, EGL_NO_CONTEXT);
    ASSERT_TRUE(
        MakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_context_));
    context_ = ContextGLES::Create(
        Flags{}, std::make_unique<ProcTableGLES>([this](const char* name) {
          return reinterpret_cast<void*>(GetProcAddress(name));
        }),
        {std::make_shared<fml::NonOwnedMapping>(
             impeller_entity_shaders_gles_data,
             impeller_entity_shaders_gles_length),
         std::make_shared<fml::NonOwnedMapping>(
             impeller_modern_shaders_gles_data,
             impeller_modern_shaders_gles_length),
         std::make_shared<fml::NonOwnedMapping>(
             impeller_framebuffer_blend_shaders_gles_data,
             impeller_framebuffer_blend_shaders_gles_length)},
        false);
    ASSERT_TRUE(context_);
    worker_ = std::make_shared<RuntimeFilterWorker>();
    ASSERT_TRUE(context_->AddReactorWorker(worker_).has_value());
    renderer_ = std::make_unique<ContentContext>(context_, nullptr);
    ASSERT_TRUE(renderer_->IsValid());
    auto mapping = fml::FileMapping::CreateReadOnly(
        std::string(flutter::testing::GetFixturesPath()) +
        "/runtime_stage_filter_displacement.frag.iplr");
    ASSERT_TRUE(mapping);
    auto stages = RuntimeStage::DecodeRuntimeStages(std::move(mapping));
    ASSERT_TRUE(stages.ok()) << stages.status();
    stage_ = stages.value().at(RuntimeStageBackend::kOpenGLES);
    ASSERT_TRUE(stage_);
  }

  void TearDown() override {
    stage_.reset();
    renderer_.reset();
    context_.reset();
    worker_.reset();
    if (egl_context_ != EGL_NO_CONTEXT) {
      MakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      DestroyContext(display_, egl_context_);
    }
    if (display_ != EGL_NO_DISPLAY && Terminate) {
      Terminate(display_);
    }
    if (egl_) {
      dlclose(egl_);
    }
  }

 protected:
  const ProcTableGLES& GL() { return context_->GetReactor()->GetProcTable(); }

  void CheckDisplacement(Point translation,
                         Vector2 scale,
                         Vector2 displacement,
                         bool borrowed,
                         Point input_offset = {}) {
    const auto& gl = GL();
    // A spatially varying, opaque input catches both a missing copy and wrong
    // origin/scale/Y direction. Borrowed FBO row zero is the bottom screen row;
    // owned textures use the opposite convention.
    constexpr ISize kSize(100, 80);
    std::vector<uint8_t> pixels(kSize.width * kSize.height * 4);
    for (int y = 0; y < kSize.height; y++) {
      for (int x = 0; x < kSize.width; x++) {
        const auto offset = (y * kSize.width + x) * 4;
        pixels[offset] = 20 + x * 2;
        pixels[offset + 1] = 20 + (borrowed ? 79 - y : y) * 2;
        pixels[offset + 2] = 60;
        pixels[offset + 3] = 255;
      }
    }
    GLuint texture = 0;
    GLuint framebuffer = 0;
    gl.GenTextures(1, &texture);
    gl.BindTexture(GL_TEXTURE_2D, texture);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kSize.width, kSize.height, 0,
                  GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    gl.GenFramebuffers(1, &framebuffer);
    gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                            texture, 0);
    ASSERT_EQ(gl.CheckFramebufferStatus(GL_FRAMEBUFFER),
              static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    TextureDescriptor descriptor;
    descriptor.format = PixelFormat::kR8G8B8A8UNormInt;
    descriptor.size = kSize;
    descriptor.storage_mode = StorageMode::kDevicePrivate;
    descriptor.usage = TextureUsage::kRenderTarget | TextureUsage::kShaderRead;
    auto input_texture =
        borrowed
            ? TextureGLES::WrapFBOTexture(context_->GetReactor(), descriptor,
                                          framebuffer, texture)
            : TextureGLES::WrapTexture(context_->GetReactor(), descriptor,
                                       context_->GetReactor()->CreateHandle(
                                           HandleType::kTexture, texture));
    ASSERT_TRUE(input_texture);

    struct Uniforms {
      Size size;
      Vector2 displacement;
      std::array<Scalar, 4> shape;
    } uniform_values = {{}, displacement, {32, 24, 8, 8}};
    auto uniforms = std::make_shared<std::vector<uint8_t>>(sizeof(Uniforms));
    memcpy(uniforms->data(), &uniform_values, sizeof(Uniforms));
    auto filter = FilterContents::MakeRuntimeEffect(
        FilterInput::Make(input_texture,
                          Matrix::MakeTranslation(Vector3(input_offset))),
        stage_, uniforms,
        {{.sampler_descriptor = SamplerDescriptor{}, .texture = nullptr}});
    Entity entity;
    entity.SetTransform(Matrix::MakeTranslation(Vector3(translation)) *
                        Matrix::MakeScale(scale));
    // The runtime shape stays in screen-resolution coordinates, even when the
    // input is scaled. The hint restricts the output, NOT the shader's reads.
    const auto output_coverage =
        Rect::MakeXYWH(translation.x + 32, translation.y + 24, 8, 8);
    filter->SetCoverageHint(output_coverage);
    ASSERT_EQ(filter->GetCoverage(entity), output_coverage);
    auto snapshot = filter->RenderToSnapshot(
        *renderer_, entity, {.coverage_limit = output_coverage});
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_TRUE(static_cast<Context&>(*context_).FlushCommandBuffers());
    EXPECT_EQ(snapshot->transform,
              Matrix::MakeTranslation(output_coverage.GetOrigin()));
    EXPECT_EQ(snapshot->texture->GetSize(), ISize(8, 8));

    // Read an owned snapshot through a temporary FBO: its row zero corresponds
    // to the top screen row. No screenshot or CPU shader emulation is involved.
    auto result_texture = TextureGLES::Cast(*snapshot->texture).GetGLHandle();
    ASSERT_TRUE(result_texture.has_value());
    gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                            result_texture.value(), 0);
    ASSERT_EQ(gl.CheckFramebufferStatus(GL_FRAMEBUFFER),
              static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    std::array<uint8_t, 8 * 8 * 4> result;
    gl.ReadPixels(0, 0, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, result.data());
    for (const auto& point :
         {IPoint(1, 1), IPoint(6, 1), IPoint(1, 6), IPoint(6, 6)}) {
      SCOPED_TRACE(::testing::Message()
                   << "pixel " << point.x << "," << point.y);
      Point shader_position(32 + point.x + 0.5f, 24 + point.y + 0.5f);
      Point sample_position = shader_position + Point(displacement);
      EXPECT_FALSE(Rect::MakeXYWH(32, 24, 8, 8).Contains(sample_position));
      const auto input_coverage = Rect::MakeSize(kSize).TransformBounds(
          Matrix::MakeScale(scale) *
          Matrix::MakeTranslation(Vector3(input_offset)));
      EXPECT_TRUE(input_coverage.Contains(sample_position));
      Point source_position = sample_position / scale - input_offset;
      const auto offset = (point.y * 8 + point.x) * 4;
      EXPECT_NEAR(result[offset], 20 + (source_position.x - 0.5f) * 2, 2);
      EXPECT_NEAR(result[offset + 1], 20 + (source_position.y - 0.5f) * 2, 2);
      EXPECT_NEAR(result[offset + 2], 60, 1);
      EXPECT_EQ(result[offset + 3], 255);
    }
    gl.DeleteFramebuffers(1, &framebuffer);
    input_texture.reset();
    if (borrowed) {
      gl.DeleteTextures(1, &texture);
    }
  }

 private:
  void* egl_ = nullptr;
  EGLDisplay display_ = EGL_NO_DISPLAY;
  EGLContext egl_context_ = EGL_NO_CONTEXT;
  PFNEGLGETPROCADDRESSPROC GetProcAddress = nullptr;
  PFNEGLINITIALIZEPROC Initialize = nullptr;
  PFNEGLBINDAPIPROC BindAPI = nullptr;
  PFNEGLCHOOSECONFIGPROC ChooseConfig = nullptr;
  PFNEGLCREATECONTEXTPROC CreateContext = nullptr;
  PFNEGLMAKECURRENTPROC MakeCurrent = nullptr;
  PFNEGLDESTROYCONTEXTPROC DestroyContext = nullptr;
  PFNEGLTERMINATEPROC Terminate = nullptr;
  std::shared_ptr<ContextGLES> context_;
  std::shared_ptr<RuntimeFilterWorker> worker_;
  std::unique_ptr<ContentContext> renderer_;
  std::shared_ptr<RuntimeStage> stage_;
};

TEST_F(RuntimeFilterGLES, BorrowedDisplacementReadsOutsideOutputCoverage) {
  for (auto translation : {Point(0, 0), Point(12, 7), Point(-12, -7)}) {
    for (auto scale : {Vector2(1, 1), Vector2(1.5f, 2)}) {
      for (auto displacement : {Vector2(20, 16), Vector2(-20, -16)}) {
        SCOPED_TRACE(::testing::Message()
                     << "translate " << translation.x << "," << translation.y
                     << " scale " << scale.x << "," << scale.y
                     << " displacement " << displacement.x << ","
                     << displacement.y);
        CheckDisplacement(translation, scale, displacement, true);
      }
    }
  }
}

TEST_F(RuntimeFilterGLES, BorrowedTranslatedInputRetainsShaderPadding) {
  // A separately translated input starts after the entity origin. Keep the
  // existing transparent padding so shape and sample coordinates do not shift.
  for (auto scale : {Vector2(1, 1), Vector2(1.5f, 2)}) {
    CheckDisplacement(Point(12, 7), scale, Vector2(20, 16), true, Point(8, 5));
  }
}

TEST_F(RuntimeFilterGLES, OwnedScaledDisplacementReadsOutsideOutputCoverage) {
  CheckDisplacement(Point(12, 7), Vector2(1.5f, 2), Vector2(20, 16), false);
}

}  // namespace
}  // namespace testing
}  // namespace impeller
