// Copyright 2019 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Common/Logging/Log.h"

#include "VideoBackends/D3D12/Common.h"
#include "VideoBackends/D3D12/D3D12BoundingBox.h"
#include "VideoBackends/D3D12/D3D12PerfQuery.h"
#include "VideoBackends/D3D12/D3D12Renderer.h"
#include "VideoBackends/D3D12/D3D12SwapChain.h"
#include "VideoBackends/D3D12/DX12Context.h"
#include "VideoBackends/D3D12/DX12Pipeline.h"
#include "VideoBackends/D3D12/DX12Shader.h"
#include "VideoBackends/D3D12/DX12Texture.h"
#include "VideoBackends/D3D12/DX12VertexFormat.h"
#include "VideoBackends/D3D12/DescriptorHeapManager.h"
#include "VideoCommon/VideoConfig.h"

namespace DX12
{
Renderer::Renderer(std::unique_ptr<SwapChain> swap_chain, float backbuffer_scale)
    : ::Renderer(swap_chain ? swap_chain->GetWidth() : 0, swap_chain ? swap_chain->GetHeight() : 0,
                 backbuffer_scale,
                 swap_chain ? swap_chain->GetFormat() : AbstractTextureFormat::Undefined),
      m_swap_chain(std::move(swap_chain))
{
  m_state.root_signature = g_dx_context->GetGXRootSignature();

  // Textures must be populated with null descriptors, since we copy directly from this array.
  for (u32 i = 0; i < MAX_TEXTURES; i++)
  {
    m_state.textures[i].ptr = g_dx_context->GetNullSRVDescriptor().cpu_handle.ptr;
    m_state.samplers.states[i] = RenderState::GetPointSamplerState();
  }
}

Renderer::~Renderer() = default;

bool Renderer::IsHeadless() const
{
  return !m_swap_chain;
}

bool Renderer::Initialize()
{
  if (!::Renderer::Initialize())
    return false;

  m_bounding_box = BoundingBox::Create();
  if (!m_bounding_box)
    return false;

  SetPixelShaderUAV(m_bounding_box->GetGPUDescriptor().cpu_handle);
  return true;
}

void Renderer::Shutdown()
{
  m_bounding_box.reset();
  m_swap_chain.reset();

  ::Renderer::Shutdown();
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
  return DXShader::CreateFromSource(stage, source);
}

std::unique_ptr<AbstractShader> Renderer::CreateShaderFromBinary(ShaderStage stage,
                                                                 const void* data, size_t length)
{
  return DXShader::CreateFromBytecode(stage, DXShader::CreateByteCode(data, length));
}

std::unique_ptr<NativeVertexFormat>
Renderer::CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl)
{
  return std::make_unique<DXVertexFormat>(vtx_decl);
}

std::unique_ptr<AbstractPipeline> Renderer::CreatePipeline(const AbstractPipelineConfig& config,
                                                           const void* cache_data,
                                                           size_t cache_data_length)
{
  return DXPipeline::Create(config, cache_data, cache_data_length);
}

u16 Renderer::BBoxReadImpl(int index)
{
  return static_cast<u16>(m_bounding_box->Get(index));
}

void Renderer::BBoxWriteImpl(int index, u16 value)
{
  m_bounding_box->Set(index, value);
}

void Renderer::BBoxFlushImpl()
{
  m_bounding_box->Flush();
  m_bounding_box->Invalidate();
}

void Renderer::Flush()
{
  ExecuteCommandList(false);
}

void Renderer::WaitForGPUIdle()
{
  ExecuteCommandList(true);
}

void Renderer::ClearScreen(const MathUtil::Rectangle<int>& rc, bool color_enable, bool alpha_enable,
                           bool z_enable, u32 color, u32 z)
{
  // Use a fast path without the shader if both color/alpha are enabled.
  const bool fast_color_clear = color_enable && (alpha_enable || !EFBHasAlphaChannel());
  if (fast_color_clear || z_enable)
  {
    MathUtil::Rectangle<int> native_rc = ConvertEFBRectangle(rc);
    native_rc.ClampUL(0, 0, m_current_framebuffer->GetWidth(), m_current_framebuffer->GetHeight());
    const D3D12_RECT d3d_clear_rc{native_rc.left, native_rc.top, native_rc.right, native_rc.bottom};

    if (fast_color_clear)
    {
      static_cast<DXTexture*>(m_current_framebuffer->GetColorAttachment())
          ->TransitionToState(D3D12_RESOURCE_STATE_RENDER_TARGET);

      const std::array<float, 4> clear_color = {
          {static_cast<float>((color >> 16) & 0xFF) / 255.0f,
           static_cast<float>((color >> 8) & 0xFF) / 255.0f,
           static_cast<float>((color >> 0) & 0xFF) / 255.0f,
           static_cast<float>((color >> 24) & 0xFF) / 255.0f}};
      g_dx_context->GetCommandList()->ClearRenderTargetView(
          static_cast<const DXFramebuffer*>(m_current_framebuffer)->GetRTVDescriptor().cpu_handle,
          clear_color.data(), 1, &d3d_clear_rc);
      color_enable = false;
      alpha_enable = false;
    }

    if (z_enable)
    {
      static_cast<DXTexture*>(m_current_framebuffer->GetDepthAttachment())
          ->TransitionToState(D3D12_RESOURCE_STATE_DEPTH_WRITE);

      // D3D does not support reversed depth ranges.
      const float clear_depth = 1.0f - static_cast<float>(z & 0xFFFFFF) / 16777216.0f;
      g_dx_context->GetCommandList()->ClearDepthStencilView(
          static_cast<const DXFramebuffer*>(m_current_framebuffer)->GetDSVDescriptor().cpu_handle,
          D3D12_CLEAR_FLAG_DEPTH, clear_depth, 0, 1, &d3d_clear_rc);
      z_enable = false;
    }
  }

  // Anything left over, fall back to clear triangle.
  if (color_enable || alpha_enable || z_enable)
    ::Renderer::ClearScreen(rc, color_enable, alpha_enable, z_enable, color, z);
}

void Renderer::SetPipeline(const AbstractPipeline* pipeline)
{
  const DXPipeline* dx_pipeline = static_cast<const DXPipeline*>(pipeline);
  if (m_current_pipeline == dx_pipeline)
    return;

  m_current_pipeline = dx_pipeline;
  m_dirty_bits |= DirtyState_Pipeline;

  if (dx_pipeline)
  {
    if (dx_pipeline->GetRootSignature() != m_state.root_signature)
    {
      m_state.root_signature = dx_pipeline->GetRootSignature();
      m_dirty_bits |= DirtyState_RootSignature | DirtyState_PS_CBV | DirtyState_VS_CBV |
                      DirtyState_GS_CBV | DirtyState_SRV_Descriptor |
                      DirtyState_Sampler_Descriptor | DirtyState_UAV_Descriptor;
    }
    if (dx_pipeline->UseIntegerRTV() != m_state.using_integer_rtv)
    {
      m_state.using_integer_rtv = dx_pipeline->UseIntegerRTV();
      m_dirty_bits |= DirtyState_Framebuffer;
    }
    if (dx_pipeline->GetPrimitiveTopology() != m_state.primitive_topology)
    {
      m_state.primitive_topology = dx_pipeline->GetPrimitiveTopology();
      m_dirty_bits |= DirtyState_PrimitiveTopology;
    }
  }
}

void Renderer::BindFramebuffer(DXFramebuffer* fb)
{
  if (fb->HasColorBuffer())
  {
    static_cast<DXTexture*>(fb->GetColorAttachment())
        ->TransitionToState(D3D12_RESOURCE_STATE_RENDER_TARGET);
  }
  if (fb->HasDepthBuffer())
  {
    static_cast<DXTexture*>(fb->GetDepthAttachment())
        ->TransitionToState(D3D12_RESOURCE_STATE_DEPTH_WRITE);
  }

  g_dx_context->GetCommandList()->OMSetRenderTargets(
      fb->GetRTVDescriptorCount(),
      m_state.using_integer_rtv ? fb->GetIntRTVDescriptorArray() : fb->GetRTVDescriptorArray(),
      FALSE, fb->GetDSVDescriptorArray());
  m_current_framebuffer = fb;
  m_dirty_bits &= ~DirtyState_Framebuffer;
}

void Renderer::SetFramebuffer(AbstractFramebuffer* framebuffer)
{
  if (m_current_framebuffer == framebuffer)
    return;

  m_current_framebuffer = framebuffer;
  m_dirty_bits |= DirtyState_Framebuffer;
}

void Renderer::SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer)
{
  BindFramebuffer(static_cast<DXFramebuffer*>(framebuffer));

  static const D3D12_DISCARD_REGION dr = {0, nullptr, 0, 1};
  if (framebuffer->HasColorBuffer())
  {
    g_dx_context->GetCommandList()->DiscardResource(
        static_cast<DXTexture*>(framebuffer->GetColorAttachment())->GetResource(), &dr);
  }
  if (framebuffer->HasDepthBuffer())
  {
    g_dx_context->GetCommandList()->DiscardResource(
        static_cast<DXTexture*>(framebuffer->GetDepthAttachment())->GetResource(), &dr);
  }
}

void Renderer::SetAndClearFramebuffer(AbstractFramebuffer* framebuffer,
                                      const ClearColor& color_value, float depth_value)
{
  DXFramebuffer* dxfb = static_cast<DXFramebuffer*>(framebuffer);
  BindFramebuffer(dxfb);

  static const D3D12_DISCARD_REGION dr = {0, nullptr, 0, 1};
  if (framebuffer->HasColorBuffer())
  {
    g_dx_context->GetCommandList()->ClearRenderTargetView(dxfb->GetRTVDescriptor().cpu_handle,
                                                          color_value.data(), 0, nullptr);
  }
  if (framebuffer->HasDepthBuffer())
  {
    g_dx_context->GetCommandList()->ClearDepthStencilView(
        dxfb->GetDSVDescriptor().cpu_handle, D3D12_CLEAR_FLAG_DEPTH, depth_value, 0, 0, nullptr);
  }
}

void Renderer::SetScissorRect(const MathUtil::Rectangle<int>& rc)
{
  if (m_state.scissor.left == rc.left && m_state.scissor.right == rc.right &&
      m_state.scissor.top == rc.top && m_state.scissor.bottom == rc.bottom)
  {
    return;
  }

  m_state.scissor.left = rc.left;
  m_state.scissor.right = rc.right;
  m_state.scissor.top = rc.top;
  m_state.scissor.bottom = rc.bottom;
  m_dirty_bits |= DirtyState_ScissorRect;
}

void Renderer::SetTexture(u32 index, const AbstractTexture* texture)
{
  const DXTexture* dxtex = static_cast<const DXTexture*>(texture);
  if (m_state.textures[index].ptr == dxtex->GetSRVDescriptor().cpu_handle.ptr)
    return;

  m_state.textures[index].ptr = dxtex->GetSRVDescriptor().cpu_handle.ptr;
  if (dxtex)
    dxtex->TransitionToState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

  m_dirty_bits |= DirtyState_Textures;
}

void Renderer::SetSamplerState(u32 index, const SamplerState& state)
{
  if (m_state.samplers.states[index] == state)
    return;

  m_state.samplers.states[index] = state;
  m_dirty_bits |= DirtyState_Samplers;
}

void Renderer::SetComputeImageTexture(AbstractTexture* texture, bool read, bool write)
{
  const DXTexture* dxtex = static_cast<const DXTexture*>(texture);
  if (m_state.compute_image_texture == dxtex)
    return;

  m_state.compute_image_texture = dxtex;
  m_dirty_bits |= DirtyState_ComputeImageTexture;
}

void Renderer::UnbindTexture(const AbstractTexture* texture)
{
  const auto srv_shadow_descriptor =
      static_cast<const DXTexture*>(texture)->GetSRVDescriptor().cpu_handle;
  for (u32 i = 0; i < MAX_TEXTURES; i++)
  {
    if (m_state.textures[i].ptr == srv_shadow_descriptor.ptr)
    {
      m_state.textures[i].ptr = g_dx_context->GetNullSRVDescriptor().cpu_handle.ptr;
      m_dirty_bits |= DirtyState_Textures;
    }
  }
  if (m_state.compute_image_texture == texture)
  {
    m_state.compute_image_texture = nullptr;
    m_dirty_bits |= DirtyState_ComputeImageTexture;
  }
}

void Renderer::SetViewport(float x, float y, float width, float height, float near_depth,
                           float far_depth)
{
  if (m_state.viewport.TopLeftX == x && m_state.viewport.TopLeftY == y &&
      m_state.viewport.Width == width && m_state.viewport.Height == height &&
      near_depth == m_state.viewport.MinDepth && far_depth == m_state.viewport.MaxDepth)
  {
    return;
  }

  m_state.viewport.TopLeftX = x;
  m_state.viewport.TopLeftY = y;
  m_state.viewport.Width = width;
  m_state.viewport.Height = height;
  m_state.viewport.MinDepth = near_depth;
  m_state.viewport.MaxDepth = far_depth;
  m_dirty_bits |= DirtyState_Viewport;
}

void Renderer::Draw(u32 base_vertex, u32 num_vertices)
{
  if (!ApplyState())
    return;

  g_dx_context->GetCommandList()->DrawInstanced(num_vertices, 1, base_vertex, 0);
}

void Renderer::DrawIndexed(u32 base_index, u32 num_indices, u32 base_vertex)
{
  if (!ApplyState())
    return;

  g_dx_context->GetCommandList()->DrawIndexedInstanced(num_indices, 1, base_index, base_vertex, 0);
}

void Renderer::DispatchComputeShader(const AbstractShader* shader, u32 groups_x, u32 groups_y,
                                     u32 groups_z)
{
  SetRootSignatures();
  SetDescriptorHeaps();
  UpdateDescriptorTables();

  if (m_dirty_bits & DirtyState_ComputeImageTexture && !UpdateComputeUAVDescriptorTable())
  {
    ExecuteCommandList(false);
    SetRootSignatures();
    SetDescriptorHeaps();
    UpdateDescriptorTables();
    UpdateComputeUAVDescriptorTable();
  }

  // Share graphics and compute state. No need to track now since dispatches are infrequent.
  auto* const cmdlist = g_dx_context->GetCommandList();
  cmdlist->SetPipelineState(static_cast<const DXShader*>(shader)->GetComputePipeline());
  cmdlist->SetComputeRootConstantBufferView(CS_ROOT_PARAMETER_CBV, m_state.constant_buffers[0]);
  cmdlist->SetComputeRootDescriptorTable(CS_ROOT_PARAMETER_SRV, m_state.srv_descriptor_base);
  cmdlist->SetComputeRootDescriptorTable(CS_ROOT_PARAMETER_SAMPLERS,
                                         m_state.sampler_descriptor_base);
  cmdlist->SetComputeRootDescriptorTable(CS_ROOT_PARAMETER_UAV,
                                         m_state.compute_uav_descriptor_base);
  cmdlist->Dispatch(groups_x, groups_y, groups_z);

  // Compute and graphics state share the same pipeline object? :(
  m_dirty_bits |= DirtyState_Pipeline;
}

void Renderer::BindBackbuffer(const ClearColor& clear_color)
{
  CheckForSwapChainChanges();
  SetAndClearFramebuffer(m_swap_chain->GetCurrentFramebuffer(), clear_color);
}

void Renderer::CheckForSwapChainChanges()
{
  const bool surface_changed = m_surface_changed.TestAndClear();
  const bool surface_resized =
      m_surface_resized.TestAndClear() || m_swap_chain->CheckForFullscreenChange();
  if (!surface_changed && !surface_resized)
    return;

  // The swap chain could be in use from a previous frame.
  WaitForGPUIdle();
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

void Renderer::PresentBackbuffer()
{
  m_current_framebuffer = nullptr;

  m_swap_chain->GetCurrentTexture()->TransitionToState(D3D12_RESOURCE_STATE_PRESENT);
  ExecuteCommandList(false);

  m_swap_chain->Present();
}

void Renderer::OnConfigChanged(u32 bits)
{
  ::Renderer::OnConfigChanged(bits);

  // For quad-buffered stereo we need to change the layer count, so recreate the swap chain.
  if (m_swap_chain && bits & CONFIG_CHANGE_BIT_STEREO_MODE)
  {
    ExecuteCommandList(true);
    m_swap_chain->SetStereo(SwapChain::WantsStereo());
  }

  // Wipe sampler cache if force texture filtering or anisotropy changes.
  if (bits & (CONFIG_CHANGE_BIT_ANISOTROPY | CONFIG_CHANGE_BIT_FORCE_TEXTURE_FILTERING))
  {
    ExecuteCommandList(true);
    g_dx_context->GetSamplerHeapManager().Clear();
    g_dx_context->ResetSamplerAllocators();
  }

  // If the host config changed (e.g. bbox/per-pixel-shading), recreate the root signature.
  if (bits & CONFIG_CHANGE_BIT_HOST_CONFIG)
    g_dx_context->RecreateGXRootSignature();
}

void Renderer::ExecuteCommandList(bool wait_for_completion)
{
  PerfQuery::GetInstance()->ResolveQueries();
  g_dx_context->ExecuteCommandList(wait_for_completion);
  m_dirty_bits = DirtyState_All;
}

void Renderer::SetConstantBuffer(u32 index, D3D12_GPU_VIRTUAL_ADDRESS address)
{
  if (m_state.constant_buffers[index] == address)
    return;

  m_state.constant_buffers[index] = address;
  m_dirty_bits |= DirtyState_PS_CBV << index;
}

void Renderer::SetTextureDescriptor(u32 index, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
  if (m_state.textures[index].ptr == handle.ptr)
    return;

  m_state.textures[index].ptr = handle.ptr;
  m_dirty_bits |= DirtyState_Textures;
}

void Renderer::SetPixelShaderUAV(D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
  if (m_state.ps_uav.ptr == handle.ptr)
    return;

  m_state.ps_uav = handle;
  m_dirty_bits |= DirtyState_PS_UAV;
}

void Renderer::SetVertexBuffer(D3D12_GPU_VIRTUAL_ADDRESS address, u32 stride, u32 size)
{
  if (m_state.vertex_buffer.BufferLocation == address &&
      m_state.vertex_buffer.StrideInBytes == stride && m_state.vertex_buffer.SizeInBytes == size)
  {
    return;
  }

  m_state.vertex_buffer.BufferLocation = address;
  m_state.vertex_buffer.StrideInBytes = stride;
  m_state.vertex_buffer.SizeInBytes = size;
  m_dirty_bits |= DirtyState_VertexBuffer;
}

void Renderer::SetIndexBuffer(D3D12_GPU_VIRTUAL_ADDRESS address, u32 size, DXGI_FORMAT format)
{
  if (m_state.index_buffer.BufferLocation == address && m_state.index_buffer.SizeInBytes == size &&
      m_state.index_buffer.Format == format)
  {
    return;
  }

  m_state.index_buffer.BufferLocation = address;
  m_state.index_buffer.SizeInBytes = size;
  m_state.index_buffer.Format = format;
  m_dirty_bits |= DirtyState_IndexBuffer;
}

bool Renderer::ApplyState()
{
  if (!m_current_framebuffer || !m_current_pipeline)
    return false;

  // Updating the descriptor tables can cause command list execution if no descriptors remain.
  SetRootSignatures();
  SetDescriptorHeaps();
  UpdateDescriptorTables();

  // Clear bits before actually changing state. Some state (e.g. cbuffers) can't be set
  // if utility pipelines are bound.
  const u32 dirty_bits = m_dirty_bits;
  m_dirty_bits &= ~(
      DirtyState_Framebuffer | DirtyState_Pipeline | DirtyState_Viewport | DirtyState_ScissorRect |
      DirtyState_PS_UAV | DirtyState_PS_CBV | DirtyState_VS_CBV | DirtyState_GS_CBV |
      DirtyState_SRV_Descriptor | DirtyState_Sampler_Descriptor | DirtyState_UAV_Descriptor |
      DirtyState_VertexBuffer | DirtyState_IndexBuffer | DirtyState_PrimitiveTopology);

  auto* const cmdlist = g_dx_context->GetCommandList();
  if (dirty_bits & DirtyState_Pipeline)
    cmdlist->SetPipelineState(static_cast<const DXPipeline*>(m_current_pipeline)->GetPipeline());

  if (dirty_bits & DirtyState_Framebuffer)
    BindFramebuffer(static_cast<DXFramebuffer*>(m_current_framebuffer));

  if (dirty_bits & DirtyState_Viewport)
    cmdlist->RSSetViewports(1, &m_state.viewport);

  if (dirty_bits & DirtyState_ScissorRect)
    cmdlist->RSSetScissorRects(1, &m_state.scissor);

  if (dirty_bits & DirtyState_VertexBuffer)
    cmdlist->IASetVertexBuffers(0, 1, &m_state.vertex_buffer);

  if (dirty_bits & DirtyState_IndexBuffer)
    cmdlist->IASetIndexBuffer(&m_state.index_buffer);

  if (dirty_bits & DirtyState_PrimitiveTopology)
    cmdlist->IASetPrimitiveTopology(m_state.primitive_topology);

  if (dirty_bits & DirtyState_SRV_Descriptor)
    cmdlist->SetGraphicsRootDescriptorTable(ROOT_PARAMETER_PS_SRV, m_state.srv_descriptor_base);

  if (dirty_bits & DirtyState_Sampler_Descriptor)
  {
    cmdlist->SetGraphicsRootDescriptorTable(ROOT_PARAMETER_PS_SAMPLERS,
                                            m_state.sampler_descriptor_base);
  }

  if (static_cast<const DXPipeline*>(m_current_pipeline)->GetUsage() == AbstractPipelineUsage::GX)
  {
    if (dirty_bits & DirtyState_VS_CBV)
    {
      cmdlist->SetGraphicsRootConstantBufferView(ROOT_PARAMETER_VS_CBV,
                                                 m_state.constant_buffers[1]);

      if (g_ActiveConfig.bEnablePixelLighting)
      {
        cmdlist->SetGraphicsRootConstantBufferView(
            g_ActiveConfig.bBBoxEnable ? ROOT_PARAMETER_PS_CBV2 : ROOT_PARAMETER_PS_UAV_OR_CBV2,
            m_state.constant_buffers[1]);
      }
    }

    if (dirty_bits & DirtyState_GS_CBV)
    {
      cmdlist->SetGraphicsRootConstantBufferView(ROOT_PARAMETER_GS_CBV,
                                                 m_state.constant_buffers[2]);
    }

    if (dirty_bits & DirtyState_UAV_Descriptor && g_ActiveConfig.bBBoxEnable)
    {
      cmdlist->SetGraphicsRootDescriptorTable(ROOT_PARAMETER_PS_UAV_OR_CBV2,
                                              m_state.uav_descriptor_base);
    }
  }

  if (dirty_bits & DirtyState_PS_CBV)
  {
    cmdlist->SetGraphicsRootConstantBufferView(ROOT_PARAMETER_PS_CBV, m_state.constant_buffers[0]);
  }

  return true;
}

void Renderer::SetRootSignatures()
{
  const u32 dirty_bits = m_dirty_bits;
  if (dirty_bits & DirtyState_RootSignature)
    g_dx_context->GetCommandList()->SetGraphicsRootSignature(m_state.root_signature);
  if (dirty_bits & DirtyState_ComputeRootSignature)
  {
    g_dx_context->GetCommandList()->SetComputeRootSignature(
        g_dx_context->GetComputeRootSignature());
  }
  m_dirty_bits &= ~(DirtyState_RootSignature | DirtyState_ComputeRootSignature);
}

void Renderer::SetDescriptorHeaps()
{
  if (m_dirty_bits & DirtyState_DescriptorHeaps)
  {
    g_dx_context->GetCommandList()->SetDescriptorHeaps(g_dx_context->GetGPUDescriptorHeapCount(),
                                                       g_dx_context->GetGPUDescriptorHeaps());
    m_dirty_bits &= ~DirtyState_DescriptorHeaps;
  }
}

void Renderer::UpdateDescriptorTables()
{
  // Samplers force a full sync because any of the samplers could be in use.
  const bool texture_update_failed =
      (m_dirty_bits & DirtyState_Textures) && !UpdateSRVDescriptorTable();
  const bool sampler_update_failed =
      (m_dirty_bits & DirtyState_Samplers) && !UpdateSamplerDescriptorTable();
  const bool uav_update_failed = (m_dirty_bits & DirtyState_PS_UAV) && !UpdateUAVDescriptorTable();
  if (texture_update_failed || sampler_update_failed || uav_update_failed)
  {
    WARN_LOG_FMT(VIDEO, "Executing command list while waiting for temporary {}",
                 texture_update_failed ? "descriptors" : "samplers");
    ExecuteCommandList(false);
    SetRootSignatures();
    SetDescriptorHeaps();
    UpdateSRVDescriptorTable();
    UpdateSamplerDescriptorTable();
    UpdateUAVDescriptorTable();
  }
}

bool Renderer::UpdateSRVDescriptorTable()
{
  static constexpr std::array<UINT, MAX_TEXTURES> src_sizes = {1, 1, 1, 1, 1, 1, 1, 1};
  DescriptorHandle dst_base_handle;
  const UINT dst_handle_sizes = 8;
  if (!g_dx_context->GetDescriptorAllocator()->Allocate(MAX_TEXTURES, &dst_base_handle))
    return false;

  g_dx_context->GetDevice()->CopyDescriptors(
      1, &dst_base_handle.cpu_handle, &dst_handle_sizes, MAX_TEXTURES, m_state.textures.data(),
      src_sizes.data(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  m_state.srv_descriptor_base = dst_base_handle.gpu_handle;
  m_dirty_bits = (m_dirty_bits & ~DirtyState_Textures) | DirtyState_SRV_Descriptor;
  return true;
}

bool Renderer::UpdateSamplerDescriptorTable()
{
  if (!g_dx_context->GetSamplerAllocator()->GetGroupHandle(m_state.samplers,
                                                           &m_state.sampler_descriptor_base))
  {
    g_dx_context->ResetSamplerAllocators();
    return false;
  }

  m_dirty_bits = (m_dirty_bits & ~DirtyState_Samplers) | DirtyState_Sampler_Descriptor;
  return true;
}

bool Renderer::UpdateUAVDescriptorTable()
{
  // We can skip writing the UAV descriptor if bbox isn't enabled, since it's not used otherwise.
  if (!g_ActiveConfig.bBBoxEnable)
    return true;

  DescriptorHandle handle;
  if (!g_dx_context->GetDescriptorAllocator()->Allocate(1, &handle))
    return false;

  g_dx_context->GetDevice()->CopyDescriptorsSimple(1, handle.cpu_handle, m_state.ps_uav,
                                                   D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  m_state.uav_descriptor_base = handle.gpu_handle;
  m_dirty_bits = (m_dirty_bits & ~DirtyState_PS_UAV) | DirtyState_UAV_Descriptor;
  return true;
}

bool Renderer::UpdateComputeUAVDescriptorTable()
{
  DescriptorHandle handle;
  if (!g_dx_context->GetDescriptorAllocator()->Allocate(1, &handle))
    return false;

  if (m_state.compute_image_texture)
  {
    g_dx_context->GetDevice()->CopyDescriptorsSimple(
        1, handle.cpu_handle, m_state.compute_image_texture->GetUAVDescriptor().cpu_handle,
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  }
  else
  {
    constexpr D3D12_UNORDERED_ACCESS_VIEW_DESC null_uav_desc = {};
    g_dx_context->GetDevice()->CreateUnorderedAccessView(nullptr, nullptr, &null_uav_desc,
                                                         handle.cpu_handle);
  }

  m_dirty_bits &= ~DirtyState_ComputeImageTexture;
  m_state.compute_uav_descriptor_base = handle.gpu_handle;
  return true;
}

}  // namespace DX12
