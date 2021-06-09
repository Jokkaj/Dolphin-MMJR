// Copyright 2019 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <d3d12.h>
#include "VideoBackends/D3D12/DescriptorAllocator.h"
#include "VideoBackends/D3D12/DescriptorHeapManager.h"
#include "VideoCommon/RenderBase.h"

namespace DX12
{
class BoundingBox;
class DXFramebuffer;
class DXTexture;
class DXShader;
class DXPipeline;
class SwapChain;

class Renderer final : public ::Renderer
{
public:
  Renderer(std::unique_ptr<SwapChain> swap_chain, float backbuffer_scale);
  ~Renderer() override;

  static Renderer* GetInstance() { return static_cast<Renderer*>(g_renderer.get()); }

  bool IsHeadless() const override;

  bool Initialize() override;
  void Shutdown() override;

  std::unique_ptr<AbstractTexture> CreateTexture(const TextureConfig& config) override;
  std::unique_ptr<AbstractStagingTexture>
  CreateStagingTexture(StagingTextureType type, const TextureConfig& config) override;
  std::unique_ptr<AbstractFramebuffer>
  CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment) override;

  std::unique_ptr<AbstractShader> CreateShaderFromSource(ShaderStage stage,
                                                         std::string_view source) override;
  std::unique_ptr<AbstractShader> CreateShaderFromBinary(ShaderStage stage, const void* data,
                                                         size_t length) override;
  std::unique_ptr<NativeVertexFormat>
  CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl) override;
  std::unique_ptr<AbstractPipeline> CreatePipeline(const AbstractPipelineConfig& config,
                                                   const void* cache_data = nullptr,
                                                   size_t cache_data_length = 0) override;

  u16 BBoxReadImpl(int index) override;
  void BBoxWriteImpl(int index, u16 value) override;
  void BBoxFlushImpl() override;

  void Flush() override;
  void WaitForGPUIdle() override;

  void ClearScreen(const MathUtil::Rectangle<int>& rc, bool color_enable, bool alpha_enable,
                   bool z_enable, u32 color, u32 z) override;

  void SetPipeline(const AbstractPipeline* pipeline) override;
  void SetFramebuffer(AbstractFramebuffer* framebuffer) override;
  void SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer) override;
  void SetAndClearFramebuffer(AbstractFramebuffer* framebuffer, const ClearColor& color_value = {},
                              float depth_value = 0.0f) override;
  void SetScissorRect(const MathUtil::Rectangle<int>& rc) override;
  void SetTexture(u32 index, const AbstractTexture* texture) override;
  void SetSamplerState(u32 index, const SamplerState& state) override;
  void SetComputeImageTexture(AbstractTexture* texture, bool read, bool write) override;
  void UnbindTexture(const AbstractTexture* texture) override;
  void SetViewport(float x, float y, float width, float height, float near_depth,
                   float far_depth) override;
  void Draw(u32 base_vertex, u32 num_vertices) override;
  void DrawIndexed(u32 base_index, u32 num_indices, u32 base_vertex) override;
  void DispatchComputeShader(const AbstractShader* shader, u32 groups_x, u32 groups_y,
                             u32 groups_z) override;
  void BindBackbuffer(const ClearColor& clear_color = {}) override;
  void PresentBackbuffer() override;

  // Completes the current render pass, executes the command buffer, and restores state ready for
  // next render. Use when you want to kick the current buffer to make room for new data.
  void ExecuteCommandList(bool wait_for_completion);

  // Setting constant buffer handles.
  void SetConstantBuffer(u32 index, D3D12_GPU_VIRTUAL_ADDRESS address);

  // Setting textures via descriptor handles. This is assumed to be in the shadow heap.
  void SetTextureDescriptor(u32 index, D3D12_CPU_DESCRIPTOR_HANDLE handle);

  // Pixel shader UAV.
  void SetPixelShaderUAV(D3D12_CPU_DESCRIPTOR_HANDLE handle);

  // Graphics vertex/index buffer binding.
  void SetVertexBuffer(D3D12_GPU_VIRTUAL_ADDRESS address, u32 stride, u32 size);
  void SetIndexBuffer(D3D12_GPU_VIRTUAL_ADDRESS address, u32 size, DXGI_FORMAT format);

  // Binds all dirty state
  bool ApplyState();

protected:
  void OnConfigChanged(u32 bits) override;

private:
  static const u32 MAX_TEXTURES = 8;
  static const u32 NUM_CONSTANT_BUFFERS = 3;

  // Dirty bits
  enum DirtyStates
  {
    DirtyState_Framebuffer = (1 << 0),
    DirtyState_Pipeline = (1 << 1),
    DirtyState_Textures = (1 << 2),
    DirtyState_Samplers = (1 << 3),
    DirtyState_Viewport = (1 << 4),
    DirtyState_ScissorRect = (1 << 5),
    DirtyState_ComputeImageTexture = (1 << 6),
    DirtyState_PS_UAV = (1 << 7),
    DirtyState_PS_CBV = (1 << 8),
    DirtyState_VS_CBV = (1 << 9),
    DirtyState_GS_CBV = (1 << 10),
    DirtyState_SRV_Descriptor = (1 << 11),
    DirtyState_Sampler_Descriptor = (1 << 12),
    DirtyState_UAV_Descriptor = (1 << 13),
    DirtyState_VertexBuffer = (1 << 14),
    DirtyState_IndexBuffer = (1 << 15),
    DirtyState_PrimitiveTopology = (1 << 16),
    DirtyState_RootSignature = (1 << 17),
    DirtyState_ComputeRootSignature = (1 << 18),
    DirtyState_DescriptorHeaps = (1 << 19),

    DirtyState_All =
        DirtyState_Framebuffer | DirtyState_Pipeline | DirtyState_Textures | DirtyState_Samplers |
        DirtyState_Viewport | DirtyState_ScissorRect | DirtyState_ComputeImageTexture |
        DirtyState_PS_UAV | DirtyState_PS_CBV | DirtyState_VS_CBV | DirtyState_GS_CBV |
        DirtyState_SRV_Descriptor | DirtyState_Sampler_Descriptor | DirtyState_UAV_Descriptor |
        DirtyState_VertexBuffer | DirtyState_IndexBuffer | DirtyState_PrimitiveTopology |
        DirtyState_RootSignature | DirtyState_ComputeRootSignature | DirtyState_DescriptorHeaps
  };

  void CheckForSwapChainChanges();

  void BindFramebuffer(DXFramebuffer* fb);
  void SetRootSignatures();
  void SetDescriptorHeaps();
  void UpdateDescriptorTables();
  bool UpdateSRVDescriptorTable();
  bool UpdateUAVDescriptorTable();
  bool UpdateComputeUAVDescriptorTable();
  bool UpdateSamplerDescriptorTable();

  // Owned objects
  std::unique_ptr<SwapChain> m_swap_chain;
  std::unique_ptr<BoundingBox> m_bounding_box;

  // Current state
  struct
  {
    ID3D12RootSignature* root_signature = nullptr;
    DXShader* compute_shader = nullptr;
    std::array<D3D12_GPU_VIRTUAL_ADDRESS, 3> constant_buffers = {};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, MAX_TEXTURES> textures = {};
    D3D12_CPU_DESCRIPTOR_HANDLE ps_uav = {};
    SamplerStateSet samplers = {};
    const DXTexture* compute_image_texture = nullptr;
    D3D12_VIEWPORT viewport = {};
    D3D12_RECT scissor = {};
    D3D12_GPU_DESCRIPTOR_HANDLE srv_descriptor_base = {};
    D3D12_GPU_DESCRIPTOR_HANDLE sampler_descriptor_base = {};
    D3D12_GPU_DESCRIPTOR_HANDLE uav_descriptor_base = {};
    D3D12_GPU_DESCRIPTOR_HANDLE compute_uav_descriptor_base = {};
    D3D12_VERTEX_BUFFER_VIEW vertex_buffer = {};
    D3D12_INDEX_BUFFER_VIEW index_buffer = {};
    D3D12_PRIMITIVE_TOPOLOGY primitive_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    bool using_integer_rtv = false;
  } m_state;
  u32 m_dirty_bits = DirtyState_All;
};
}  // namespace DX12
