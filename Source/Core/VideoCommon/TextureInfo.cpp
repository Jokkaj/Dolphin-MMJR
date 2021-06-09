// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoCommon/TextureInfo.h"

#include <xxhash.h>

#include "Common/Align.h"
#include "Core/HW/Memmap.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/SamplerCommon.h"
#include "VideoCommon/TextureDecoder.h"

TextureInfo TextureInfo::FromStage(u32 stage)
{
  const FourTexUnits& tex = bpmem.tex[stage >> 2];
  const u32 id = stage & 3;

  const auto texture_format = tex.texImage0[id].format;
  const auto tlut_format = tex.texTlut[id].tlut_format;

  const auto width = tex.texImage0[id].width + 1;
  const auto height = tex.texImage0[id].height + 1;

  const u32 address = (tex.texImage3[id].image_base /* & 0x1FFFFF*/) << 5;

  const u32 tlutaddr = tex.texTlut[id].tmem_offset << 9;
  const u8* tlut_ptr = &texMem[tlutaddr];

  std::optional<u32> mip_count;
  const bool has_mipmaps = SamplerCommon::AreBpTexMode0MipmapsEnabled(tex.texMode0[id]);
  if (has_mipmaps)
  {
    mip_count = (tex.texMode1[id].max_lod + 0xf) / 0x10;
  }

  const bool from_tmem = tex.texImage1[id].cache_manually_managed != 0;
  const u32 tmem_address_even = from_tmem ? tex.texImage1[id].tmem_even * TMEM_LINE_SIZE : 0;
  const u32 tmem_address_odd = from_tmem ? tex.texImage2[id].tmem_odd * TMEM_LINE_SIZE : 0;

  if (from_tmem)
  {
    return TextureInfo(&texMem[tmem_address_even], tlut_ptr, address, texture_format, tlut_format,
                       width, height, true, &texMem[tmem_address_odd], &texMem[tmem_address_even],
                       mip_count);
  }

  return TextureInfo(Memory::GetPointer(address), tlut_ptr, address, texture_format, tlut_format,
                     width, height, false, nullptr, nullptr, mip_count);
}

TextureInfo::TextureInfo(const u8* ptr, const u8* tlut_ptr, u32 address,
                         TextureFormat texture_format, TLUTFormat tlut_format, u32 width,
                         u32 height, bool from_tmem, const u8* tmem_odd, const u8* tmem_even,
                         std::optional<u32> mip_count)
    : m_ptr(ptr), m_tlut_ptr(tlut_ptr), m_address(address), m_from_tmem(from_tmem),
      m_tmem_odd(tmem_odd), m_texture_format(texture_format), m_tlut_format(tlut_format),
      m_raw_width(width), m_raw_height(height)
{
  const bool is_palette_texture = IsColorIndexed(m_texture_format);
  if (is_palette_texture)
    m_palette_size = TexDecoder_GetPaletteSize(m_texture_format);

  // TexelSizeInNibbles(format) * width * height / 16;
  m_block_width = TexDecoder_GetBlockWidthInTexels(m_texture_format);
  m_block_height = TexDecoder_GetBlockHeightInTexels(m_texture_format);

  m_expanded_width = Common::AlignUp(m_raw_width, m_block_width);
  m_expanded_height = Common::AlignUp(m_raw_height, m_block_height);

  m_texture_size =
      TexDecoder_GetTextureSizeInBytes(m_expanded_width, m_expanded_height, m_texture_format);

  if (mip_count)
  {
    m_mipmaps_enabled = true;
    const u32 raw_mip_count = *mip_count;

    // GPUs don't like when the specified mipmap count would require more than one 1x1-sized LOD in
    // the mipmap chain
    // e.g. 64x64 with 7 LODs would have the mipmap chain 64x64,32x32,16x16,8x8,4x4,2x2,1x1,0x0, so
    // we limit the mipmap count to 6 there
    const u32 limited_mip_count =
        std::min<u32>(IntLog2(std::max(width, height)) + 1, raw_mip_count + 1) - 1;

    // load mips - TODO: Loading mipmaps from tmem is untested!
    const u8* src_data = m_ptr + GetTextureSize();

    for (u32 i = 0; i < limited_mip_count; i++)
    {
      MipLevel mip_level(i + 1, *this, m_from_tmem, src_data, tmem_even, tmem_odd);
      m_mip_levels.push_back(std::move(mip_level));
    }
  }
}

TextureInfo::NameDetails TextureInfo::CalculateTextureName()
{
  if (!m_ptr)
    return NameDetails{};

  const u8* tlut = m_tlut_ptr;
  size_t tlut_size = m_palette_size ? *m_palette_size : 0;

  // checking for min/max on paletted textures
  u32 min = 0xffff;
  u32 max = 0;
  switch (tlut_size)
  {
  case 0:
    break;
  case 16 * 2:
    for (size_t i = 0; i < m_texture_size; i++)
    {
      const u32 low_nibble = m_ptr[i] & 0xf;
      const u32 high_nibble = m_ptr[i] >> 4;

      min = std::min({min, low_nibble, high_nibble});
      max = std::max({max, low_nibble, high_nibble});
    }
    break;
  case 256 * 2:
  {
    for (size_t i = 0; i < m_texture_size; i++)
    {
      const u32 texture_byte = m_ptr[i];

      min = std::min(min, texture_byte);
      max = std::max(max, texture_byte);
    }
    break;
  }
  case 16384 * 2:
    for (size_t i = 0; i < m_texture_size; i += sizeof(u16))
    {
      const u32 texture_halfword = Common::swap16(m_ptr[i]) & 0x3fff;

      min = std::min(min, texture_halfword);
      max = std::max(max, texture_halfword);
    }
    break;
  }
  if (tlut_size > 0)
  {
    tlut_size = 2 * (max + 1 - min);
    tlut += 2 * min;
  }

  const u64 tex_hash = XXH64(m_ptr, m_texture_size, 0);
  const u64 tlut_hash = tlut_size ? XXH64(tlut, tlut_size, 0) : 0;

  NameDetails result;
  result.base_name = fmt::format("{}{}x{}{}_{:016x}", format_prefix, m_raw_width, m_raw_height,
                                 m_mipmaps_enabled ? "_m" : "", tex_hash);
  result.tlut_name = tlut_size ? fmt::format("_{:016x}", tlut_hash) : "";
  result.format_name = fmt::format("_{}", static_cast<int>(m_texture_format));

  return result;
}

const u8* TextureInfo::GetData() const
{
  return m_ptr;
}

const u8* TextureInfo::GetTlutAddress() const
{
  return m_tlut_ptr;
}

u32 TextureInfo::GetRawAddress() const
{
  return m_address;
}

bool TextureInfo::IsFromTmem() const
{
  return m_from_tmem;
}

const u8* TextureInfo::GetTmemOddAddress() const
{
  return m_tmem_odd;
}

TextureFormat TextureInfo::GetTextureFormat() const
{
  return m_texture_format;
}

TLUTFormat TextureInfo::GetTlutFormat() const
{
  return m_tlut_format;
}

std::optional<u32> TextureInfo::GetPaletteSize() const
{
  return m_palette_size;
}

u32 TextureInfo::GetTextureSize() const
{
  return m_texture_size;
}

u32 TextureInfo::GetBlockWidth() const
{
  return m_block_width;
}

u32 TextureInfo::GetBlockHeight() const
{
  return m_block_height;
}

u32 TextureInfo::GetExpandedWidth() const
{
  return m_expanded_width;
}

u32 TextureInfo::GetExpandedHeight() const
{
  return m_expanded_height;
}

u32 TextureInfo::GetRawWidth() const
{
  return m_raw_width;
}

u32 TextureInfo::GetRawHeight() const
{
  return m_raw_height;
}

bool TextureInfo::HasMipMaps() const
{
  return !m_mip_levels.empty();
}

u32 TextureInfo::GetLevelCount() const
{
  return static_cast<u32>(m_mip_levels.size()) + 1;
}

const TextureInfo::MipLevel* TextureInfo::GetMipMapLevel(u32 level) const
{
  if (level < m_mip_levels.size())
    return &m_mip_levels[level];

  return nullptr;
}

TextureInfo::MipLevel::MipLevel(u32 level, const TextureInfo& parent, bool from_tmem,
                                const u8*& src_data, const u8*& ptr_even, const u8*& ptr_odd)
{
  m_raw_width = std::max(parent.GetRawWidth() >> level, 1u);
  m_raw_height = std::max(parent.GetRawHeight() >> level, 1u);
  m_expanded_width = Common::AlignUp(m_raw_width, parent.GetBlockWidth());
  m_expanded_height = Common::AlignUp(m_raw_height, parent.GetBlockHeight());

  // load mips - TODO: Loading mipmaps from tmem is untested!

  m_texture_size = TexDecoder_GetTextureSizeInBytes(m_expanded_width, m_expanded_height,
                                                    parent.GetTextureFormat());

  const u8*& ptr = from_tmem ? ((level % 2) ? ptr_odd : ptr_even) : src_data;
  m_ptr = ptr;
  ptr += m_texture_size;
}

u32 TextureInfo::GetFullLevelSize() const
{
  u32 all_mips_size = 0;
  for (const auto& mip_map : m_mip_levels)
  {
    all_mips_size += mip_map.GetTextureSize();
  }
  return m_texture_size + all_mips_size;
}

const u8* TextureInfo::MipLevel::GetData() const
{
  return m_ptr;
}

u32 TextureInfo::MipLevel::GetTextureSize() const
{
  return m_texture_size;
}

u32 TextureInfo::MipLevel::GetExpandedWidth() const
{
  return m_expanded_width;
}

u32 TextureInfo::MipLevel::GetExpandedHeight() const
{
  return m_expanded_height;
}

u32 TextureInfo::MipLevel::GetRawWidth() const
{
  return m_raw_width;
}

u32 TextureInfo::MipLevel::GetRawHeight() const
{
  return m_raw_height;
}
