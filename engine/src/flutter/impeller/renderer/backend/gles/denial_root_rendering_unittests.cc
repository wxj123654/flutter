// Copyright 2026 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gtest/gtest.h"
#include "impeller/display_list/canvas.h"
#include "impeller/entity/gles/entity_shaders_gles.h"
#include "impeller/entity/gles/framebuffer_blend_shaders_gles.h"
#include "impeller/entity/gles/modern_shaders_gles.h"
#include "impeller/renderer/backend/gles/context_gles.h"
#include "impeller/renderer/backend/gles/test/mock_gles.h"
#include "impeller/renderer/backend/gles/texture_gles.h"
#include "impeller/renderer/shader_function.h"

namespace impeller {
namespace testing {
namespace {

class RootRenderingWorker : public ReactorGLES::Worker {
 public:
  bool CanReactorReactOnCurrentThreadNow(const ReactorGLES&) const override {
    return true;
  }
};

class RootRenderingTest : public ::testing::Test {
 public:
  void SetUp() override {
    auto impl = std::make_unique<::testing::NiceMock<MockGLESImpl>>();
    ON_CALL(*impl, GetIntegerv(GL_MAX_SAMPLES, ::testing::_))
        .WillByDefault(::testing::SetArgPointee<1>(4));
    ON_CALL(*impl, CheckFramebufferStatus(::testing::_))
        .WillByDefault(::testing::Return(GL_FRAMEBUFFER_COMPLETE));
    mock_gl_ = MockGLES::Init(std::move(impl), std::nullopt, "OpenGL ES 3.0");
    const ProcTableGLES::Resolver resolver = [](const char* name) -> void* {
      if (strcmp(name, "glCreateShader") == 0) {
        return reinterpret_cast<void*>(+[](GLenum) -> GLuint { return 1; });
      }
      if (strcmp(name, "glCreateProgram") == 0) {
        return reinterpret_cast<void*>(+[]() -> GLuint { return 1; });
      }
      if (strcmp(name, "glIsProgram") == 0) {
        return reinterpret_cast<void*>(
            +[](GLuint) -> GLboolean { return GL_TRUE; });
      }
      if (strcmp(name, "glGetShaderiv") == 0) {
        return reinterpret_cast<void*>(+[](GLuint, GLenum query, GLint* value) {
          *value = query == GL_COMPILE_STATUS ? GL_TRUE : 0;
        });
      }
      if (strcmp(name, "glGetProgramiv") == 0) {
        return reinterpret_cast<void*>(+[](GLuint, GLenum query, GLint* value) {
          *value = query == GL_LINK_STATUS ? GL_TRUE : 0;
        });
      }
      return kMockResolverGLES(name);
    };
    context_ = ContextGLES::Create(
        Flags{.use_sdfs = true}, std::make_unique<ProcTableGLES>(resolver),
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
    worker_ = std::make_shared<RootRenderingWorker>();
    context_->AddReactorWorker(worker_);
    ASSERT_TRUE(std::static_pointer_cast<Context>(context_)
                    ->GetCapabilities()
                    ->SupportsOffscreenMSAA());
    ASSERT_FALSE(std::static_pointer_cast<Context>(context_)
                     ->GetCapabilities()
                     ->SupportsImplicitResolvingMSAA());
    renderer_ = std::make_unique<ContentContext>(context_, nullptr);
    ASSERT_TRUE(renderer_->IsValid());
  }

  RenderTarget MakeOutput(LoadAction load) {
    TextureDescriptor desc;
    desc.format = PixelFormat::kR8G8B8A8UNormInt;
    desc.size = {100, 80};
    desc.storage_mode = StorageMode::kDevicePrivate;
    desc.usage = TextureUsage::kRenderTarget | TextureUsage::kShaderRead;
    ColorAttachment color;
    color.texture =
        TextureGLES::WrapFBOTexture(context_->GetReactor(), desc, 7, 8);
    color.load_action = load;
    color.store_action = StoreAction::kStore;
    RenderTarget target;
    target.SetColorAttachment(color, 0);
    return target;
  }

 protected:
  std::shared_ptr<MockGLES> mock_gl_;
  std::shared_ptr<RootRenderingWorker> worker_;
  std::shared_ptr<ContextGLES> context_;
  std::unique_ptr<ContentContext> renderer_;
};

TEST_F(RootRenderingTest, RoundRectUsesSDFOnSingleSampleNativeOutput) {
  auto output = MakeOutput(LoadAction::kClear);
  Canvas canvas(*renderer_, output, false, false);
  canvas.DrawRoundRect(
      RoundRect::MakeRectXY(Rect::MakeXYWH(3, 5, 60, 30), 15, 15),
      Paint{.color = Color::White()});
  const auto& pass = canvas.GetCurrentRenderPass();
  ASSERT_FALSE(pass.GetCommands().empty());
  const auto& pipeline = pass.GetCommands().back().pipeline->GetDescriptor();
  EXPECT_EQ(pipeline.GetSampleCount(), SampleCount::kCount1);
  const auto fragment = pipeline.GetEntrypointForStage(ShaderStage::kFragment);
  ASSERT_TRUE(fragment);
  EXPECT_NE(fragment->GetName().find("uber_sdf"), std::string::npos);
  EXPECT_FALSE(canvas.RequiresReadback());
}

TEST_F(RootRenderingTest, ExplicitNonAntialiasedRoundRectKeepsGeometryPath) {
  auto output = MakeOutput(LoadAction::kClear);
  Canvas canvas(*renderer_, output, false, false);
  Paint paint{.color = Color::White()};
  paint.anti_alias = false;
  canvas.DrawRoundRect(
      RoundRect::MakeRectXY(Rect::MakeXYWH(3, 5, 60, 30), 15, 15), paint);
  const auto& pass = canvas.GetCurrentRenderPass();
  ASSERT_FALSE(pass.GetCommands().empty());
  const auto fragment =
      pass.GetCommands().back().pipeline->GetDescriptor().GetEntrypointForStage(
          ShaderStage::kFragment);
  ASSERT_TRUE(fragment);
  EXPECT_EQ(fragment->GetName().find("uber_sdf"), std::string::npos);
}

TEST_F(RootRenderingTest, ReadablePersistentRootRetainsLoadAndDirectBackdrop) {
  auto output = MakeOutput(LoadAction::kLoad);
  Canvas canvas(*renderer_, output, false, true,
                Rect::MakeXYWH(20, 15, 10, 12));
  const auto& pass = canvas.GetCurrentRenderPass();
  EXPECT_FALSE(canvas.RequiresReadback());
  EXPECT_EQ(pass.GetRenderTarget().GetSampleCount(), SampleCount::kCount1);
  EXPECT_EQ(pass.GetRenderTarget().GetRenderTargetTexture(),
            output.GetRenderTargetTexture());
  EXPECT_EQ(pass.GetRenderTarget().GetColorAttachment(0).load_action,
            LoadAction::kLoad);
  EXPECT_TRUE(pass.GetCommands().empty());
}

TEST_F(RootRenderingTest, OnlyBorrowedFramebufferTexturesNeedSamplingFlip) {
  auto output = MakeOutput(LoadAction::kClear);
  EXPECT_EQ(output.GetRenderTargetTexture()->GetYCoordScale(), -1.0f);
  const auto desc = output.GetRenderTargetTexture()->GetTextureDescriptor();
  auto owned = context_->GetResourceAllocator()->CreateTexture(desc);
  ASSERT_TRUE(owned);
  EXPECT_EQ(owned->GetYCoordScale(), 1.0f);
  auto handle = context_->GetReactor()->CreateHandle(HandleType::kTexture, 42);
  auto external =
      TextureGLES::WrapTexture(context_->GetReactor(), desc, handle);
  ASSERT_TRUE(external);
  EXPECT_EQ(external->GetYCoordScale(), 1.0f);
}

}  // namespace
}  // namespace testing
}  // namespace impeller
