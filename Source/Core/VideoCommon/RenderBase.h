// Copyright 2010 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

// ---------------------------------------------------------------------------------------------
// GC graphics pipeline
// ---------------------------------------------------------------------------------------------
// 3d commands are issued through the fifo. The GPU draws to the 2MB EFB.
// The efb can be copied back into ram in two forms: as textures or as XFB.
// The XFB is the region in RAM that the VI chip scans out to the television.
// So, after all rendering to EFB is done, the image is copied into one of two XFBs in RAM.
// Next frame, that one is scanned out and the other one gets the copy. = double buffering.
// ---------------------------------------------------------------------------------------------

#pragma once

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Event.h"
#include "Common/Flag.h"
#include "Common/MathUtil.h"
#include "VideoCommon/AsyncShaderCompiler.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/FPSCounter.h"
#include "VideoCommon/FrameDump.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/TextureConfig.h"

class AbstractFramebuffer;
class AbstractPipeline;
class AbstractShader;
class AbstractTexture;
class AbstractStagingTexture;
class NativeVertexFormat;
class NetPlayChatUI;
class PointerWrap;
struct TextureConfig;
struct ComputePipelineConfig;
struct AbstractPipelineConfig;
struct PortableVertexDeclaration;
enum class ShaderStage;
enum class EFBAccessType;
enum class EFBReinterpretType;
enum class StagingTextureType;
enum class AspectMode;

namespace VideoCommon
{
class PostProcessing;
}  // namespace VideoCommon

struct EfbPokeData
{
  u16 x, y;
  u32 data;
};

// Renderer really isn't a very good name for this class - it's more like "Misc".
// The long term goal is to get rid of this class and replace it with others that make
// more sense.
class Renderer
{
public:
  Renderer(int backbuffer_width, int backbuffer_height, float backbuffer_scale,
           AbstractTextureFormat backbuffer_format);
  virtual ~Renderer();

  using ClearColor = std::array<float, 4>;

  virtual bool IsHeadless() const = 0;

  virtual bool Initialize();
  virtual void Shutdown();

  virtual void SetPipeline(const AbstractPipeline* pipeline) {}
  virtual void SetScissorRect(const MathUtil::Rectangle<int>& rc) {}
  virtual void SetTexture(u32 index, const AbstractTexture* texture) {}
  virtual void SetSamplerState(u32 index, const SamplerState& state) {}
  virtual void SetComputeImageTexture(AbstractTexture* texture, bool read, bool write) {}
  virtual void UnbindTexture(const AbstractTexture* texture) {}
  virtual void SetViewport(float x, float y, float width, float height, float near_depth,
                           float far_depth)
  {
  }
  virtual void SetFullscreen(bool enable_fullscreen) {}
  virtual bool IsFullscreen() const { return false; }
  virtual void BeginUtilityDrawing();
  virtual void EndUtilityDrawing();
  virtual std::unique_ptr<AbstractTexture> CreateTexture(const TextureConfig& config) = 0;
  virtual std::unique_ptr<AbstractStagingTexture>
  CreateStagingTexture(StagingTextureType type, const TextureConfig& config) = 0;
  virtual std::unique_ptr<AbstractFramebuffer>
  CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment) = 0;

  // Framebuffer operations.
  virtual void SetFramebuffer(AbstractFramebuffer* framebuffer);
  virtual void SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer);
  virtual void SetAndClearFramebuffer(AbstractFramebuffer* framebuffer,
                                      const ClearColor& color_value = {}, float depth_value = 0.0f);

  // Drawing with currently-bound pipeline state.
  virtual void Draw(u32 base_vertex, u32 num_vertices) {}
  virtual void DrawIndexed(u32 base_index, u32 num_indices, u32 base_vertex) {}

  // Dispatching compute shaders with currently-bound state.
  virtual void DispatchComputeShader(const AbstractShader* shader, u32 groups_x, u32 groups_y,
                                     u32 groups_z)
  {
  }

  // Binds the backbuffer for rendering. The buffer will be cleared immediately after binding.
  // This is where any window size changes are detected, therefore m_backbuffer_width and/or
  // m_backbuffer_height may change after this function returns.
  virtual void BindBackbuffer(const ClearColor& clear_color = {}) {}

  // Presents the backbuffer to the window system, or "swaps buffers".
  virtual void PresentBackbuffer() {}

  // Shader modules/objects.
  virtual std::unique_ptr<AbstractShader> CreateShaderFromSource(ShaderStage stage,
                                                                 std::string_view source) = 0;
  virtual std::unique_ptr<AbstractShader>
  CreateShaderFromBinary(ShaderStage stage, const void* data, size_t length) = 0;
  virtual std::unique_ptr<NativeVertexFormat>
  CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl) = 0;
  virtual std::unique_ptr<AbstractPipeline> CreatePipeline(const AbstractPipelineConfig& config,
                                                           const void* cache_data = nullptr,
                                                           size_t cache_data_length = 0) = 0;

  AbstractFramebuffer* GetCurrentFramebuffer() const { return m_current_framebuffer; }

  // Ideal internal resolution - multiple of the native EFB resolution
  int GetTargetWidth() const { return m_target_width; }
  int GetTargetHeight() const { return m_target_height; }
  // Display resolution
  int GetBackbufferWidth() const { return m_backbuffer_width; }
  int GetBackbufferHeight() const { return m_backbuffer_height; }
  float GetBackbufferScale() const { return m_backbuffer_scale; }
  void SetWindowSize(int width, int height);

  // Sets viewport and scissor to the specified rectangle. rect is assumed to be in framebuffer
  // coordinates, i.e. lower-left origin in OpenGL.
  void SetViewportAndScissor(const MathUtil::Rectangle<int>& rect, float min_depth = 0.0f,
                             float max_depth = 1.0f);

  // Scales a GPU texture using a copy shader.
  virtual void ScaleTexture(AbstractFramebuffer* dst_framebuffer,
                            const MathUtil::Rectangle<int>& dst_rect,
                            const AbstractTexture* src_texture,
                            const MathUtil::Rectangle<int>& src_rect);

  // Converts an upper-left to lower-left if required by the backend, optionally
  // clamping to the framebuffer size.
  MathUtil::Rectangle<int> ConvertFramebufferRectangle(const MathUtil::Rectangle<int>& rect,
                                                       u32 fb_width, u32 fb_height) const;
  MathUtil::Rectangle<int>
  ConvertFramebufferRectangle(const MathUtil::Rectangle<int>& rect,
                              const AbstractFramebuffer* framebuffer) const;

  // EFB coordinate conversion functions
  // Use this to convert a whole native EFB rect to backbuffer coordinates
  MathUtil::Rectangle<int> ConvertEFBRectangle(const MathUtil::Rectangle<int>& rc) const;

  const MathUtil::Rectangle<int>& GetTargetRectangle() const { return m_target_rectangle; }
  float CalculateDrawAspectRatio() const;

  // Crops the target rectangle to the framebuffer dimensions, reducing the size of the source
  // rectangle if it is greater. Works even if the source and target rectangles don't have a
  // 1:1 pixel mapping, scaling as appropriate.
  void AdjustRectanglesToFitBounds(MathUtil::Rectangle<int>& target_rect,
                                   MathUtil::Rectangle<int>& source_rect);

  std::tuple<float, float> ScaleToDisplayAspectRatio(int width, int height) const;
  void UpdateDrawRectangle();

  std::tuple<float, float> ApplyStandardAspectCrop(float width, float height) const;

  // Use this to convert a single target rectangle to two stereo rectangles
  std::tuple<MathUtil::Rectangle<int>, MathUtil::Rectangle<int>>
  ConvertStereoRectangle(const MathUtil::Rectangle<int>& rc) const;

  unsigned int GetEFBScale() const;

  // Use this to upscale native EFB coordinates to IDEAL internal resolution
  int EFBToScaledX(int x) const;
  int EFBToScaledY(int y) const;

  // Floating point versions of the above - only use them if really necessary
  float EFBToScaledXf(float x) const;
  float EFBToScaledYf(float y) const;

  // Random utilities
  void SaveScreenshot(std::string filename);
  void DrawDebugText();

  virtual void ClearScreen(const MathUtil::Rectangle<int>& rc, bool colorEnable, bool alphaEnable,
                           bool zEnable, u32 color, u32 z);
  virtual void ReinterpretPixelData(EFBReinterpretType convtype);
  void RenderToXFB(u32 xfbAddr, const MathUtil::Rectangle<int>& sourceRc, u32 fbStride,
                   u32 fbHeight, float Gamma = 1.0f);

  virtual u32 AccessEFB(EFBAccessType type, u32 x, u32 y, u32 poke_data);
  virtual void PokeEFB(EFBAccessType type, const EfbPokeData* points, size_t num_points);

  u16 BBoxRead(int index);
  void BBoxWrite(int index, u16 value);
  void BBoxFlush();

  virtual void Flush() {}
  virtual void WaitForGPUIdle() {}

  // Finish up the current frame, print some stats
  void Swap(u32 xfb_addr, u32 fb_width, u32 fb_stride, u32 fb_height, u64 ticks);

  void UpdateWidescreenHeuristic();

  // Draws the specified XFB buffer to the screen, performing any post-processing.
  // Assumes that the backbuffer has already been bound and cleared.
  virtual void RenderXFBToScreen(const MathUtil::Rectangle<int>& target_rc,
                                 const AbstractTexture* source_texture,
                                 const MathUtil::Rectangle<int>& source_rc);

  // Called when the configuration changes, and backend structures need to be updated.
  virtual void OnConfigChanged(u32 bits) {}

  PixelFormat GetPrevPixelFormat() const { return m_prev_efb_format; }
  void StorePixelFormat(PixelFormat new_format) { m_prev_efb_format = new_format; }
  bool EFBHasAlphaChannel() const;
  VideoCommon::PostProcessing* GetPostProcessor() const { return m_post_processor.get(); }
  // Final surface changing
  // This is called when the surface is resized (WX) or the window changes (Android).
  void ChangeSurface(void* new_surface_handle);
  void ResizeSurface();
  bool UseVertexDepthRange() const;
  void DoState(PointerWrap& p);

  virtual std::unique_ptr<VideoCommon::AsyncShaderCompiler> CreateAsyncShaderCompiler();

  // Returns true if a layer-expanding geometry shader should be used when rendering the user
  // interface and final XFB.
  bool UseGeometryShaderForUI() const;

  // Returns a lock for the ImGui mutex, enabling data structures to be modified from outside.
  // Use with care, only non-drawing functions should be called from outside the video thread,
  // as the drawing is tied to a "frame".
  std::unique_lock<std::mutex> GetImGuiLock();

  // Begins/presents a "UI frame". UI frames do not draw any of the console XFB, but this could
  // change in the future.
  void BeginUIFrame();
  void EndUIFrame();

  // Will forcibly reload all textures on the next swap
  void ForceReloadTextures();

protected:
  // Bitmask containing information about which configuration has changed for the backend.
  enum ConfigChangeBits : u32
  {
    CONFIG_CHANGE_BIT_HOST_CONFIG = (1 << 0),
    CONFIG_CHANGE_BIT_MULTISAMPLES = (1 << 1),
    CONFIG_CHANGE_BIT_STEREO_MODE = (1 << 2),
    CONFIG_CHANGE_BIT_TARGET_SIZE = (1 << 3),
    CONFIG_CHANGE_BIT_ANISOTROPY = (1 << 4),
    CONFIG_CHANGE_BIT_FORCE_TEXTURE_FILTERING = (1 << 5),
    CONFIG_CHANGE_BIT_VSYNC = (1 << 6),
    CONFIG_CHANGE_BIT_BBOX = (1 << 7)
  };

  std::tuple<int, int> CalculateTargetScale(int x, int y) const;
  bool CalculateTargetSize();

  void CheckForConfigChanges();

  void CheckFifoRecording();
  void RecordVideoMemory();

  // ImGui initialization depends on being able to create textures and pipelines, so do it last.
  bool InitializeImGui();

  // Recompiles ImGui pipeline - call when stereo mode changes.
  bool RecompileImGuiPipeline();

  // Sets up ImGui state for the next frame.
  // This function itself acquires the ImGui lock, so it should not be held.
  void BeginImGuiFrame();

  // Destroys all ImGui GPU resources, must do before shutdown.
  void ShutdownImGui();

  // Renders ImGui windows to the currently-bound framebuffer.
  // Should be called with the ImGui lock held.
  void DrawImGui();

  virtual u16 BBoxReadImpl(int index) = 0;
  virtual void BBoxWriteImpl(int index, u16 value) = 0;
  virtual void BBoxFlushImpl() {}

  AbstractFramebuffer* m_current_framebuffer = nullptr;
  const AbstractPipeline* m_current_pipeline = nullptr;

  Common::Flag m_screenshot_request;
  Common::Event m_screenshot_completed;
  std::mutex m_screenshot_lock;
  std::string m_screenshot_name;

  bool m_is_game_widescreen = false;
  bool m_was_orthographically_anamorphic = false;

  // The framebuffer size
  int m_target_width = 1;
  int m_target_height = 1;

  // Backbuffer (window) size and render area
  int m_backbuffer_width = 0;
  int m_backbuffer_height = 0;
  float m_backbuffer_scale = 1.0f;
  AbstractTextureFormat m_backbuffer_format = AbstractTextureFormat::Undefined;
  MathUtil::Rectangle<int> m_target_rectangle = {};
  int m_frame_count = 0;

  FPSCounter m_fps_counter;

  std::unique_ptr<VideoCommon::PostProcessing> m_post_processor;

  void* m_new_surface_handle = nullptr;
  Common::Flag m_surface_changed;
  Common::Flag m_surface_resized;
  std::mutex m_swap_mutex;

  // ImGui resources.
  std::unique_ptr<NativeVertexFormat> m_imgui_vertex_format;
  std::vector<std::unique_ptr<AbstractTexture>> m_imgui_textures;
  std::unique_ptr<AbstractPipeline> m_imgui_pipeline;
  std::mutex m_imgui_mutex;
  u64 m_imgui_last_frame_time;

private:
  std::tuple<int, int> CalculateOutputDimensions(int width, int height) const;

  PixelFormat m_prev_efb_format = PixelFormat::INVALID_FMT;
  unsigned int m_efb_scale = 1;

  // These will be set on the first call to SetWindowSize.
  int m_last_window_request_width = 0;
  int m_last_window_request_height = 0;

  // frame dumping:
  FrameDump m_frame_dump;
  std::thread m_frame_dump_thread;
  Common::Flag m_frame_dump_thread_running;

  // Used to kick frame dump thread.
  Common::Event m_frame_dump_start;

  // Set by frame dump thread on frame completion.
  Common::Event m_frame_dump_done;

  // Holds emulation state during the last swap when dumping.
  FrameDump::FrameState m_last_frame_state;

  // Communication of frame between video and dump threads.
  FrameDump::FrameData m_frame_dump_data;

  // Texture used for screenshot/frame dumping
  std::unique_ptr<AbstractTexture> m_frame_dump_render_texture;
  std::unique_ptr<AbstractFramebuffer> m_frame_dump_render_framebuffer;

  // Double buffer:
  std::unique_ptr<AbstractStagingTexture> m_frame_dump_readback_texture;
  std::unique_ptr<AbstractStagingTexture> m_frame_dump_output_texture;
  // Set when readback texture holds a frame that needs to be dumped.
  bool m_frame_dump_needs_flush = false;
  // Set when thread is processing output texture.
  bool m_frame_dump_frame_running = false;

  // Used to generate screenshot names.
  u32 m_frame_dump_image_counter = 0;

  // Tracking of XFB textures so we don't render duplicate frames.
  u64 m_last_xfb_id = std::numeric_limits<u64>::max();
  u64 m_last_xfb_ticks = 0;
  u32 m_last_xfb_addr = 0;
  u32 m_last_xfb_width = 0;
  u32 m_last_xfb_stride = 0;
  u32 m_last_xfb_height = 0;

  // Nintendo's SDK seems to write "default" bounding box values before every draw (1023 0 1023 0
  // are the only values encountered so far, which happen to be the extents allowed by the BP
  // registers) to reset the registers for comparison in the pixel engine, and presumably to detect
  // whether GX has updated the registers with real values.
  //
  // We can store these values when Bounding Box emulation is disabled and return them on read,
  // which the game will interpret as "no pixels have been drawn"
  //
  // This produces much better results than just returning garbage, which can cause games like
  // Ultimate Spider-Man to crash
  std::array<u16, 4> m_bounding_box_fallback = {};

  // NOTE: The methods below are called on the framedumping thread.
  void FrameDumpThreadFunc();
  bool StartFrameDumpToFFMPEG(const FrameDump::FrameData&);
  void DumpFrameToFFMPEG(const FrameDump::FrameData&);
  void StopFrameDumpToFFMPEG();
  std::string GetFrameDumpNextImageFileName() const;
  bool StartFrameDumpToImage(const FrameDump::FrameData&);
  void DumpFrameToImage(const FrameDump::FrameData&);

  void ShutdownFrameDumping();

  bool IsFrameDumping() const;

  // Checks that the frame dump render texture exists and is the correct size.
  bool CheckFrameDumpRenderTexture(u32 target_width, u32 target_height);

  // Checks that the frame dump readback texture exists and is the correct size.
  bool CheckFrameDumpReadbackTexture(u32 target_width, u32 target_height);

  // Fills the frame dump staging texture with the current XFB texture.
  void DumpCurrentFrame(const AbstractTexture* src_texture,
                        const MathUtil::Rectangle<int>& src_rect, u64 ticks, int frame_number);

  // Asynchronously encodes the specified pointer of frame data to the frame dump.
  void DumpFrameData(const u8* data, int w, int h, int stride);

  // Ensures all rendered frames are queued for encoding.
  void FlushFrameDump();

  // Ensures all encoded frames have been written to the output file.
  void FinishFrameData();

  std::unique_ptr<NetPlayChatUI> m_netplay_chat_ui;

  Common::Flag m_force_reload_textures;
};

extern std::unique_ptr<Renderer> g_renderer;
