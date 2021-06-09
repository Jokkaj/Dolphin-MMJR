// Copyright 2010 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoBackends/D3D/D3DRender.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <strsafe.h>
#include <tuple>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MathUtil.h"

#include "Core/Core.h"

#include "VideoBackends/D3D/D3DBase.h"
#include "VideoBackends/D3D/D3DBoundingBox.h"
#include "VideoBackends/D3D/D3DState.h"
#include "VideoBackends/D3D/D3DSwapChain.h"
#include "VideoBackends/D3D/DXPipeline.h"
#include "VideoBackends/D3D/DXShader.h"
#include "VideoBackends/D3D/DXTexture.h"

#include "VideoCommon/BPFunctions.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/PostProcessing.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

namespace DX11
{
Renderer::Renderer(std::unique_ptr<SwapChain> swap_chain, float backbuffer_scale)
    : ::Renderer(swap_chain ? swap_chain->GetWidth() : 0, swap_chain ? swap_chain->GetHeight() : 0,
                 backbuffer_scale,
                 swap_chain ? swap_chain->GetFormat() : AbstractTextureFormat::Undefined),
      m_swap_chain(std::move(swap_chain))
{
}

Renderer::~Renderer() = default;

bool Renderer::IsHeadless() const
{
  return !m_swap_chain;
}

std::unique_ptr<AbstractTexture> Renderer::CreateTexture(const TextureConfig& config)
{
  return DXTexture::Create(config);
}

std::unique_ptr<AbstractStagingTexture> Renderer::CreateStagingTexture(StagingTextureType type,
                                                                       const TextureConfig& config)
{
  return DXStagingTexture::Create(type, config);
}

std::unique_ptr<AbstractFramebuffer> Renderer::CreateFramebuffer(AbstractTexture* color_attachment,
                                                                 AbstractTexture* depth_attachment)
{
  return DXFramebuffer::Create(static_cast<DXTexture*>(color_attachment),
                               static_cast<DXTexture*>(depth_attachment));
}

std::unique_ptr<AbstractShader> Renderer::CreateShaderFromSource(ShaderStage stage,
                                                                 std::string_view source)
{
  auto bytecode = DXShader::CompileShader(D3D::feature_level, stage, source);
  if (!bytecode)
    return nullptr;

  return DXShader::CreateFromBytecode(stage, std::move(*bytecode));
}

std::unique_ptr<AbstractShader> Renderer::CreateShaderFromBinary(ShaderStage stage,
                                                                 const void* data, size_t length)
{
  return DXShader::CreateFromBytecode(stage, DXShader::CreateByteCode(data, length));
}

std::unique_ptr<AbstractPipeline> Renderer::CreatePipeline(const AbstractPipelineConfig& config,
                                                           const void* cache_data,
                                                           size_t cache_data_length)
{
  return DXPipeline::Create(config);
}

void Renderer::SetPipeline(const AbstractPipeline* pipeline)
{
  const DXPipeline* dx_pipeline = static_cast<const DXPipeline*>(pipeline);
  if (m_current_pipeline == dx_pipeline)
    return;

  if (dx_pipeline)
  {
    D3D::stateman->SetRasterizerState(dx_pipeline->GetRasterizerState());
    D3D::stateman->SetDepthState(dx_pipeline->GetDepthState());
    D3D::stateman->SetBlendState(dx_pipeline->GetBlendState());
    D3D::stateman->SetPrimitiveTopology(dx_pipeline->GetPrimitiveTopology());
    D3D::stateman->SetInputLayout(dx_pipeline->GetInputLayout());
    D3D::stateman->SetVertexShader(dx_pipeline->GetVertexShader());
    D3D::stateman->SetGeometryShader(dx_pipeline->GetGeometryShader());
    D3D::stateman->SetPixelShader(dx_pipeline->GetPixelShader());
    D3D::stateman->SetIntegerRTV(dx_pipeline->UseLogicOp());
  }
  else
  {
    // These will be destroyed at pipeline destruction.
    D3D::stateman->SetInputLayout(nullptr);
    D3D::stateman->SetVertexShader(nullptr);
    D3D::stateman->SetGeometryShader(nullptr);
    D3D::stateman->SetPixelShader(nullptr);
  }
}

void Renderer::SetScissorRect(const MathUtil::Rectangle<int>& rc)
{
  // TODO: Move to stateman
  const CD3D11_RECT rect(rc.left, rc.top, std::max(rc.right, rc.left + 1),
                         std::max(rc.bottom, rc.top + 1));
  D3D::context->RSSetScissorRects(1, &rect);
}

void Renderer::SetViewport(float x, float y, float width, float height, float near_depth,
                           float far_depth)
{
  // TODO: Move to stateman
  const CD3D11_VIEWPORT vp(x, y, width, height, near_depth, far_depth);
  D3D::context->RSSetViewports(1, &vp);
}

void Renderer::Draw(u32 base_vertex, u32 num_vertices)
{
  D3D::stateman->Apply();
  D3D::context->Draw(num_vertices, base_vertex);
}

void Renderer::DrawIndexed(u32 base_index, u32 num_indices, u32 base_vertex)
{
  D3D::stateman->Apply();
  D3D::context->DrawIndexed(num_indices, base_index, base_vertex);
}

void Renderer::DispatchComputeShader(const AbstractShader* shader, u32 groups_x, u32 groups_y,
                                     u32 groups_z)
{
  D3D::stateman->SetComputeShader(static_cast<const DXShader*>(shader)->GetD3DComputeShader());
  D3D::stateman->SyncComputeBindings();
  D3D::context->Dispatch(groups_x, groups_y, groups_z);
}

void Renderer::BindBackbuffer(const ClearColor& clear_color)
{
  CheckForSwapChainChanges();
  SetAndClearFramebuffer(m_swap_chain->GetFramebuffer(), clear_color);
}

void Renderer::PresentBackbuffer()
{
  m_swap_chain->Present();
}

void Renderer::OnConfigChanged(u32 bits)
{
  // Quad-buffer changes require swap chain recreation.
  if (bits & CONFIG_CHANGE_BIT_STEREO_MODE && m_swap_chain)
    m_swap_chain->SetStereo(SwapChain::WantsStereo());
}

void Renderer::CheckForSwapChainChanges()
{
  const bool surface_changed = m_surface_changed.TestAndClear();
  const bool surface_resized =
      m_surface_resized.TestAndClear() || m_swap_chain->CheckForFullscreenChange();
  if (!surface_changed && !surface_resized)
    return;

  if (surface_changed)
  {
    m_swap_chain->ChangeSurface(m_new_surface_handle);
    m_new_surface_handle = nullptr;
  }
  else
  {
    m_swap_chain->ResizeSwapChain();
  }

  m_backbuffer_width = m_swap_chain->GetWidth();
  m_backbuffer_height = m_swap_chain->GetHeight();
}

void Renderer::SetFramebuffer(AbstractFramebuffer* framebuffer)
{
  if (m_current_framebuffer == framebuffer)
    return;

  // We can't leave the framebuffer bound as a texture and a render target.
  DXFramebuffer* fb = static_cast<DXFramebuffer*>(framebuffer);
  if ((fb->GetColorAttachment() &&
       D3D::stateman->UnsetTexture(
           static_cast<DXTexture*>(fb->GetColorAttachment())->GetD3DSRV()) != 0) ||
      (fb->GetDepthAttachment() &&
       D3D::stateman->UnsetTexture(
           static_cast<DXTexture*>(fb->GetDepthAttachment())->GetD3DSRV()) != 0))
  {
    D3D::stateman->ApplyTextures();
  }

  D3D::stateman->SetFramebuffer(fb);
  m_current_framebuffer = fb;
}

void Renderer::SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer)
{
  SetFramebuffer(framebuffer);
}

void Renderer::SetAndClearFramebuffer(AbstractFramebuffer* framebuffer,
                                      const ClearColor& color_value, float depth_value)
{
  SetFramebuffer(framebuffer);
  D3D::stateman->Apply();

  if (framebuffer->GetColorFormat() != AbstractTextureFormat::Undefined)
  {
    D3D::context->ClearRenderTargetView(
        static_cast<const DXFramebuffer*>(framebuffer)->GetRTVArray()[0], color_value.data());
  }
  if (framebuffer->GetDepthFormat() != AbstractTextureFormat::Undefined)
  {
    D3D::context->ClearDepthStencilView(static_cast<const DXFramebuffer*>(framebuffer)->GetDSV(),
                                        D3D11_CLEAR_DEPTH, depth_value, 0);
  }
}

void Renderer::SetTexture(u32 index, const AbstractTexture* texture)
{
  D3D::stateman->SetTexture(index, texture ? static_cast<const DXTexture*>(texture)->GetD3DSRV() :
                                             nullptr);
}

void Renderer::SetSamplerState(u32 index, const SamplerState& state)
{
  D3D::stateman->SetSampler(index, m_state_cache.Get(state));
}

void Renderer::SetComputeImageTexture(AbstractTexture* texture, bool read, bool write)
{
  D3D::stateman->SetComputeUAV(texture ? static_cast<DXTexture*>(texture)->GetD3DUAV() : nullptr);
}

void Renderer::UnbindTexture(const AbstractTexture* texture)
{
  if (D3D::stateman->UnsetTexture(static_cast<const DXTexture*>(texture)->GetD3DSRV()) != 0)
    D3D::stateman->ApplyTextures();
}

u16 Renderer::BBoxReadImpl(int index)
{
  return static_cast<u16>(BBox::Get(index));
}

void Renderer::BBoxWriteImpl(int index, u16 value)
{
  BBox::Set(index, value);
}

void Renderer::BBoxFlushImpl()
{
  BBox::Flush();
}

void Renderer::Flush()
{
  D3D::context->Flush();
}

void Renderer::WaitForGPUIdle()
{
  // There is no glFinish() equivalent in D3D.
  D3D::context->Flush();
}

void Renderer::SetFullscreen(bool enable_fullscreen)
{
  if (m_swap_chain)
    m_swap_chain->SetFullscreen(enable_fullscreen);
}

bool Renderer::IsFullscreen() const
{
  return m_swap_chain && m_swap_chain->GetFullscreen();
}

}  // namespace DX11
