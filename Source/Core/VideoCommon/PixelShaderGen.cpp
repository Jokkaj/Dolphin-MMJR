// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoCommon/PixelShaderGen.h"

#include <cmath>
#include <cstdio>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/LightingShaderGen.h"
#include "VideoCommon/NativeVertexFormat.h"
#include "VideoCommon/RenderState.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"  // for texture projection mode

// TODO: Get rid of these
enum : u32
{
  C_COLORMATRIX = 0,                //  0
  C_COLORS = 0,                     //  0
  C_KCOLORS = C_COLORS + 4,         //  4
  C_ALPHA = C_KCOLORS + 4,          //  8
  C_TEXDIMS = C_ALPHA + 1,          //  9
  C_ZBIAS = C_TEXDIMS + 8,          // 17
  C_INDTEXSCALE = C_ZBIAS + 2,      // 19
  C_INDTEXMTX = C_INDTEXSCALE + 2,  // 21
  C_FOGCOLOR = C_INDTEXMTX + 6,     // 27
  C_FOGI = C_FOGCOLOR + 1,          // 28
  C_FOGF = C_FOGI + 1,              // 29
  C_ZSLOPE = C_FOGF + 2,            // 31
  C_EFBSCALE = C_ZSLOPE + 1,        // 32
  C_PENVCONST_END = C_EFBSCALE + 1
};

constexpr std::array<const char*, 32> tev_ksel_table_c{
    "255,255,255",        // 1   = 0x00
    "223,223,223",        // 7_8 = 0x01
    "191,191,191",        // 3_4 = 0x02
    "159,159,159",        // 5_8 = 0x03
    "128,128,128",        // 1_2 = 0x04
    "96,96,96",           // 3_8 = 0x05
    "64,64,64",           // 1_4 = 0x06
    "32,32,32",           // 1_8 = 0x07
    "0,0,0",              // INVALID = 0x08
    "0,0,0",              // INVALID = 0x09
    "0,0,0",              // INVALID = 0x0a
    "0,0,0",              // INVALID = 0x0b
    I_KCOLORS "[0].rgb",  // K0 = 0x0C
    I_KCOLORS "[1].rgb",  // K1 = 0x0D
    I_KCOLORS "[2].rgb",  // K2 = 0x0E
    I_KCOLORS "[3].rgb",  // K3 = 0x0F
    I_KCOLORS "[0].rrr",  // K0_R = 0x10
    I_KCOLORS "[1].rrr",  // K1_R = 0x11
    I_KCOLORS "[2].rrr",  // K2_R = 0x12
    I_KCOLORS "[3].rrr",  // K3_R = 0x13
    I_KCOLORS "[0].ggg",  // K0_G = 0x14
    I_KCOLORS "[1].ggg",  // K1_G = 0x15
    I_KCOLORS "[2].ggg",  // K2_G = 0x16
    I_KCOLORS "[3].ggg",  // K3_G = 0x17
    I_KCOLORS "[0].bbb",  // K0_B = 0x18
    I_KCOLORS "[1].bbb",  // K1_B = 0x19
    I_KCOLORS "[2].bbb",  // K2_B = 0x1A
    I_KCOLORS "[3].bbb",  // K3_B = 0x1B
    I_KCOLORS "[0].aaa",  // K0_A = 0x1C
    I_KCOLORS "[1].aaa",  // K1_A = 0x1D
    I_KCOLORS "[2].aaa",  // K2_A = 0x1E
    I_KCOLORS "[3].aaa",  // K3_A = 0x1F
};

constexpr std::array<const char*, 32> tev_ksel_table_a{
    "255",              // 1   = 0x00
    "223",              // 7_8 = 0x01
    "191",              // 3_4 = 0x02
    "159",              // 5_8 = 0x03
    "128",              // 1_2 = 0x04
    "96",               // 3_8 = 0x05
    "64",               // 1_4 = 0x06
    "32",               // 1_8 = 0x07
    "0",                // INVALID = 0x08
    "0",                // INVALID = 0x09
    "0",                // INVALID = 0x0a
    "0",                // INVALID = 0x0b
    "0",                // INVALID = 0x0c
    "0",                // INVALID = 0x0d
    "0",                // INVALID = 0x0e
    "0",                // INVALID = 0x0f
    I_KCOLORS "[0].r",  // K0_R = 0x10
    I_KCOLORS "[1].r",  // K1_R = 0x11
    I_KCOLORS "[2].r",  // K2_R = 0x12
    I_KCOLORS "[3].r",  // K3_R = 0x13
    I_KCOLORS "[0].g",  // K0_G = 0x14
    I_KCOLORS "[1].g",  // K1_G = 0x15
    I_KCOLORS "[2].g",  // K2_G = 0x16
    I_KCOLORS "[3].g",  // K3_G = 0x17
    I_KCOLORS "[0].b",  // K0_B = 0x18
    I_KCOLORS "[1].b",  // K1_B = 0x19
    I_KCOLORS "[2].b",  // K2_B = 0x1A
    I_KCOLORS "[3].b",  // K3_B = 0x1B
    I_KCOLORS "[0].a",  // K0_A = 0x1C
    I_KCOLORS "[1].a",  // K1_A = 0x1D
    I_KCOLORS "[2].a",  // K2_A = 0x1E
    I_KCOLORS "[3].a",  // K3_A = 0x1F
};

constexpr std::array<const char*, 16> tev_c_input_table{
    "prev.rgb",           // CPREV,
    "prev.aaa",           // APREV,
    "c0.rgb",             // C0,
    "c0.aaa",             // A0,
    "c1.rgb",             // C1,
    "c1.aaa",             // A1,
    "c2.rgb",             // C2,
    "c2.aaa",             // A2,
    "textemp.rgb",        // TEXC,
    "textemp.aaa",        // TEXA,
    "rastemp.rgb",        // RASC,
    "rastemp.aaa",        // RASA,
    "int3(255,255,255)",  // ONE
    "int3(128,128,128)",  // HALF
    "konsttemp.rgb",      // KONST
    "int3(0,0,0)",        // ZERO
};

constexpr std::array<const char*, 8> tev_a_input_table{
    "prev.a",       // APREV,
    "c0.a",         // A0,
    "c1.a",         // A1,
    "c2.a",         // A2,
    "textemp.a",    // TEXA,
    "rastemp.a",    // RASA,
    "konsttemp.a",  // KONST,  (hw1 had quarter)
    "0",            // ZERO
};

constexpr std::array<const char*, 8> tev_ras_table{
    "iround(col0 * 255.0)",
    "iround(col1 * 255.0)",
    "ERROR13",                                              // 2
    "ERROR14",                                              // 3
    "ERROR15",                                              // 4
    "(int4(1, 1, 1, 1) * alphabump)",                       // bump alpha (0..248)
    "(int4(1, 1, 1, 1) * (alphabump | (alphabump >> 5)))",  // normalized bump alpha (0..255)
    "int4(0, 0, 0, 0)",                                     // zero
};

constexpr std::array<const char*, 4> tev_c_output_table{
    "prev.rgb",
    "c0.rgb",
    "c1.rgb",
    "c2.rgb",
};

constexpr std::array<const char*, 4> tev_a_output_table{
    "prev.a",
    "c0.a",
    "c1.a",
    "c2.a",
};

// FIXME: Some of the video card's capabilities (BBox support, EarlyZ support, dstAlpha support)
//        leak into this UID; This is really unhelpful if these UIDs ever move from one machine to
//        another.
PixelShaderUid GetPixelShaderUid()
{
  PixelShaderUid out;

  pixel_shader_uid_data* const uid_data = out.GetUidData();
  uid_data->useDstAlpha = bpmem.dstalpha.enable && bpmem.blendmode.alphaupdate &&
                          bpmem.zcontrol.pixel_format == PixelFormat::RGBA6_Z24;

  uid_data->genMode_numindstages = bpmem.genMode.numindstages;
  uid_data->genMode_numtevstages = bpmem.genMode.numtevstages;
  uid_data->genMode_numtexgens = bpmem.genMode.numtexgens;
  uid_data->bounding_box = g_ActiveConfig.bBBoxEnable && BoundingBox::IsEnabled();
  uid_data->rgba6_format =
      bpmem.zcontrol.pixel_format == PixelFormat::RGBA6_Z24 && !g_ActiveConfig.bForceTrueColor;
  uid_data->dither = bpmem.blendmode.dither && uid_data->rgba6_format;
  uid_data->uint_output = bpmem.blendmode.UseLogicOp();

  u32 numStages = uid_data->genMode_numtevstages + 1;

  const bool forced_early_z =
      bpmem.UseEarlyDepthTest() &&
      (g_ActiveConfig.bFastDepthCalc ||
       bpmem.alpha_test.TestResult() == AlphaTestResult::Undetermined)
      // We can't allow early_ztest for zfreeze because depth is overridden per-pixel.
      // This means it's impossible for zcomploc to be emulated on a zfrozen polygon.
      && !(bpmem.zmode.testenable && bpmem.genMode.zfreeze);
  const bool per_pixel_depth =
      (bpmem.ztex2.op != ZTexOp::Disabled && bpmem.UseLateDepthTest()) ||
      (!g_ActiveConfig.bFastDepthCalc && bpmem.zmode.testenable && !forced_early_z) ||
      (bpmem.zmode.testenable && bpmem.genMode.zfreeze);

  uid_data->per_pixel_depth = per_pixel_depth;
  uid_data->forced_early_z = forced_early_z;

  if (g_ActiveConfig.bEnablePixelLighting)
  {
    uid_data->numColorChans = xfmem.numChan.numColorChans;
    GetLightingShaderUid(uid_data->lighting);
  }

  if (uid_data->genMode_numtexgens > 0)
  {
    for (unsigned int i = 0; i < uid_data->genMode_numtexgens; ++i)
    {
      // optional perspective divides
      uid_data->texMtxInfo_n_projection |= static_cast<u32>(xfmem.texMtxInfo[i].projection.Value())
                                           << i;
    }
  }

  // indirect texture map lookup
  int nIndirectStagesUsed = 0;
  for (unsigned int i = 0; i < numStages; ++i)
  {
    if (bpmem.tevind[i].IsActive())
      nIndirectStagesUsed |= 1 << bpmem.tevind[i].bt;
  }

  uid_data->nIndirectStagesUsed = nIndirectStagesUsed;
  for (u32 i = 0; i < uid_data->genMode_numindstages; ++i)
  {
    if (uid_data->nIndirectStagesUsed & (1 << i))
      uid_data->SetTevindrefValues(i, bpmem.tevindref.getTexCoord(i), bpmem.tevindref.getTexMap(i));
  }

  for (unsigned int n = 0; n < numStages; n++)
  {
    uid_data->stagehash[n].tevorders_texcoord = bpmem.tevorders[n / 2].getTexCoord(n & 1);

    // hasindstage previously was used as a criterion to set tevind to 0, but there are variables in
    // tevind that are used even if the indirect stage is disabled, so now it is only left in to
    // avoid breaking existing UIDs (in most cases, games will have 0 in tevind anyways)
    // TODO: Remove hasindstage on the next UID version bump
    uid_data->stagehash[n].hasindstage = bpmem.tevind[n].bt < bpmem.genMode.numindstages;
    uid_data->stagehash[n].tevind = bpmem.tevind[n].hex;

    TevStageCombiner::ColorCombiner& cc = bpmem.combiners[n].colorC;
    TevStageCombiner::AlphaCombiner& ac = bpmem.combiners[n].alphaC;
    uid_data->stagehash[n].cc = cc.hex & 0xFFFFFF;
    uid_data->stagehash[n].ac = ac.hex & 0xFFFFF0;  // Storing rswap and tswap later

    if (cc.a == TevColorArg::RasAlpha || cc.a == TevColorArg::RasColor ||
        cc.b == TevColorArg::RasAlpha || cc.b == TevColorArg::RasColor ||
        cc.c == TevColorArg::RasAlpha || cc.c == TevColorArg::RasColor ||
        cc.d == TevColorArg::RasAlpha || cc.d == TevColorArg::RasColor ||
        ac.a == TevAlphaArg::RasAlpha || ac.b == TevAlphaArg::RasAlpha ||
        ac.c == TevAlphaArg::RasAlpha || ac.d == TevAlphaArg::RasAlpha)
    {
      const int i = bpmem.combiners[n].alphaC.rswap;
      uid_data->stagehash[n].tevksel_swap1a = bpmem.tevksel[i * 2].swap1;
      uid_data->stagehash[n].tevksel_swap2a = bpmem.tevksel[i * 2].swap2;
      uid_data->stagehash[n].tevksel_swap1b = bpmem.tevksel[i * 2 + 1].swap1;
      uid_data->stagehash[n].tevksel_swap2b = bpmem.tevksel[i * 2 + 1].swap2;
      uid_data->stagehash[n].tevorders_colorchan = bpmem.tevorders[n / 2].getColorChan(n & 1);
    }

    uid_data->stagehash[n].tevorders_enable = bpmem.tevorders[n / 2].getEnable(n & 1);
    if (uid_data->stagehash[n].tevorders_enable)
    {
      const int i = bpmem.combiners[n].alphaC.tswap;
      uid_data->stagehash[n].tevksel_swap1c = bpmem.tevksel[i * 2].swap1;
      uid_data->stagehash[n].tevksel_swap2c = bpmem.tevksel[i * 2].swap2;
      uid_data->stagehash[n].tevksel_swap1d = bpmem.tevksel[i * 2 + 1].swap1;
      uid_data->stagehash[n].tevksel_swap2d = bpmem.tevksel[i * 2 + 1].swap2;
      uid_data->stagehash[n].tevorders_texmap = bpmem.tevorders[n / 2].getTexMap(n & 1);
    }

    if (cc.a == TevColorArg::Konst || cc.b == TevColorArg::Konst || cc.c == TevColorArg::Konst ||
        cc.d == TevColorArg::Konst || ac.a == TevAlphaArg::Konst || ac.b == TevAlphaArg::Konst ||
        ac.c == TevAlphaArg::Konst || ac.d == TevAlphaArg::Konst)
    {
      uid_data->stagehash[n].tevksel_kc = bpmem.tevksel[n / 2].getKC(n & 1);
      uid_data->stagehash[n].tevksel_ka = bpmem.tevksel[n / 2].getKA(n & 1);
    }
  }

#define MY_STRUCT_OFFSET(str, elem) ((u32)((u64) & (str).elem - (u64) & (str)))
  uid_data->num_values = (g_ActiveConfig.bEnablePixelLighting) ?
                             sizeof(*uid_data) :
                             MY_STRUCT_OFFSET(*uid_data, stagehash[numStages]);

  uid_data->Pretest = bpmem.alpha_test.TestResult();
  uid_data->late_ztest = bpmem.UseLateDepthTest();

  // NOTE: Fragment may not be discarded if alpha test always fails and early depth test is enabled
  // (in this case we need to write a depth value if depth test passes regardless of the alpha
  // testing result)
  if (uid_data->Pretest == AlphaTestResult::Undetermined ||
      (uid_data->Pretest == AlphaTestResult::Fail && uid_data->late_ztest))
  {
    uid_data->alpha_test_comp0 = bpmem.alpha_test.comp0;
    uid_data->alpha_test_comp1 = bpmem.alpha_test.comp1;
    uid_data->alpha_test_logic = bpmem.alpha_test.logic;

    // ZCOMPLOC HACK:
    // The only way to emulate alpha test + early-z is to force early-z in the shader.
    // As this isn't available on all drivers and as we can't emulate this feature otherwise,
    // we are only able to choose which one we want to respect more.
    // Tests seem to have proven that writing depth even when the alpha test fails is more
    // important that a reliable alpha test, so we just force the alpha test to always succeed.
    // At least this seems to be less buggy.
    uid_data->alpha_test_use_zcomploc_hack =
        bpmem.UseEarlyDepthTest() && bpmem.zmode.updateenable &&
        !g_ActiveConfig.backend_info.bSupportsEarlyZ && !bpmem.genMode.zfreeze;
  }

  uid_data->zfreeze = bpmem.genMode.zfreeze;
  uid_data->ztex_op = bpmem.ztex2.op;
  uid_data->early_ztest = bpmem.UseEarlyDepthTest();

  uid_data->fog_fsel = bpmem.fog.c_proj_fsel.fsel;
  uid_data->fog_proj = bpmem.fog.c_proj_fsel.proj;
  uid_data->fog_RangeBaseEnabled = bpmem.fogRange.Base.Enabled;

  BlendingState state = {};
  state.Generate(bpmem);

  if (state.usedualsrc && state.dstalpha && g_ActiveConfig.backend_info.bSupportsFramebufferFetch &&
      !g_ActiveConfig.backend_info.bSupportsDualSourceBlend)
  {
    uid_data->blend_enable = state.blendenable;
    uid_data->blend_src_factor = state.srcfactor;
    uid_data->blend_src_factor_alpha = state.srcfactoralpha;
    uid_data->blend_dst_factor = state.dstfactor;
    uid_data->blend_dst_factor_alpha = state.dstfactoralpha;
    uid_data->blend_subtract = state.subtract;
    uid_data->blend_subtract_alpha = state.subtractAlpha;
  }

  return out;
}

void ClearUnusedPixelShaderUidBits(APIType api_type, const ShaderHostConfig& host_config,
                                   PixelShaderUid* uid)
{
  pixel_shader_uid_data* const uid_data = uid->GetUidData();

  // OpenGL and Vulkan convert implicitly normalized color outputs to their uint representation.
  // Therefore, it is not necessary to use a uint output on these backends. We also disable the
  // uint output when logic op is not supported (i.e. driver/device does not support D3D11.1).
  if (api_type != APIType::D3D || !host_config.backend_logic_op)
    uid_data->uint_output = 0;

  // If bounding box is enabled when a UID cache is created, then later disabled, we shouldn't
  // emit the bounding box portion of the shader.
  uid_data->bounding_box &= host_config.bounding_box & host_config.backend_bbox;
}

void WritePixelShaderCommonHeader(ShaderCode& out, APIType api_type,
                                  const ShaderHostConfig& host_config, bool bounding_box)
{
  // dot product for integer vectors
  out.Write("int idot(int3 x, int3 y)\n"
            "{{\n"
            "\tint3 tmp = x * y;\n"
            "\treturn tmp.x + tmp.y + tmp.z;\n"
            "}}\n");

  out.Write("int idot(int4 x, int4 y)\n"
            "{{\n"
            "\tint4 tmp = x * y;\n"
            "\treturn tmp.x + tmp.y + tmp.z + tmp.w;\n"
            "}}\n\n");

  // rounding + casting to integer at once in a single function
  out.Write("int  iround(float  x) {{ return int (round(x)); }}\n"
            "int2 iround(float2 x) {{ return int2(round(x)); }}\n"
            "int3 iround(float3 x) {{ return int3(round(x)); }}\n"
            "int4 iround(float4 x) {{ return int4(round(x)); }}\n\n");

  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
  {
    out.Write("SAMPLER_BINDING(0) uniform sampler2DArray samp[8];\n");
  }
  else  // D3D
  {
    // Declare samplers
    out.Write("SamplerState samp[8] : register(s0);\n"
              "\n"
              "Texture2DArray Tex[8] : register(t0);\n");
  }
  out.Write("\n");

  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
    out.Write("UBO_BINDING(std140, 1) uniform PSBlock {{\n");
  else
    out.Write("cbuffer PSBlock : register(b0) {{\n");

  out.Write("\tint4 " I_COLORS "[4];\n"
            "\tint4 " I_KCOLORS "[4];\n"
            "\tint4 " I_ALPHA ";\n"
            "\tfloat4 " I_TEXDIMS "[8];\n"
            "\tint4 " I_ZBIAS "[2];\n"
            "\tint4 " I_INDTEXSCALE "[2];\n"
            "\tint4 " I_INDTEXMTX "[6];\n"
            "\tint4 " I_FOGCOLOR ";\n"
            "\tint4 " I_FOGI ";\n"
            "\tfloat4 " I_FOGF ";\n"
            "\tfloat4 " I_FOGRANGE "[3];\n"
            "\tfloat4 " I_ZSLOPE ";\n"
            "\tfloat2 " I_EFBSCALE ";\n"
            "\tuint  bpmem_genmode;\n"
            "\tuint  bpmem_alphaTest;\n"
            "\tuint  bpmem_fogParam3;\n"
            "\tuint  bpmem_fogRangeBase;\n"
            "\tuint  bpmem_dstalpha;\n"
            "\tuint  bpmem_ztex_op;\n"
            "\tbool  bpmem_late_ztest;\n"
            "\tbool  bpmem_rgba6_format;\n"
            "\tbool  bpmem_dither;\n"
            "\tbool  bpmem_bounding_box;\n"
            "\tuint4 bpmem_pack1[16];\n"  // .xy - combiners, .z - tevind
            "\tuint4 bpmem_pack2[8];\n"   // .x - tevorder, .y - tevksel
            "\tint4  konstLookup[32];\n"
            "\tbool  blend_enable;\n"
            "\tuint  blend_src_factor;\n"
            "\tuint  blend_src_factor_alpha;\n"
            "\tuint  blend_dst_factor;\n"
            "\tuint  blend_dst_factor_alpha;\n"
            "\tbool  blend_subtract;\n"
            "\tbool  blend_subtract_alpha;\n"
            "}};\n\n");
  out.Write("#define bpmem_combiners(i) (bpmem_pack1[(i)].xy)\n"
            "#define bpmem_tevind(i) (bpmem_pack1[(i)].z)\n"
            "#define bpmem_iref(i) (bpmem_pack1[(i)].w)\n"
            "#define bpmem_tevorder(i) (bpmem_pack2[(i)].x)\n"
            "#define bpmem_tevksel(i) (bpmem_pack2[(i)].y)\n\n");

  if (host_config.per_pixel_lighting)
  {
    out.Write("{}", s_lighting_struct);

    if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
      out.Write("UBO_BINDING(std140, 2) uniform VSBlock {{\n");
    else
      out.Write("cbuffer VSBlock : register(b1) {{\n");

    out.Write("{}", s_shader_uniforms);
    out.Write("}};\n");
  }

  if (bounding_box)
  {
    if (api_type == APIType::D3D)
    {
      out.Write("globallycoherent RWBuffer<int> bbox_data : register(u2);\n"
                "#define atomicMin InterlockedMin\n"
                "#define atomicMax InterlockedMax");
    }
    else
    {
      out.Write("SSBO_BINDING(0) buffer BBox {{\n");

      if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_SSBO_FIELD_ATOMICS))
      {
        // AMD drivers on Windows seemingly ignore atomic writes to fields or array elements of an
        // SSBO other than the first one, but using an int4 seems to work fine
        out.Write("  int4 bbox_data;\n");
      }
      else
      {
        // The Metal shader compiler fails to compile the atomic instructions when operating on
        // individual components of a vector
        out.Write("  int bbox_data[4];\n");
      }

      out.Write("}};");
    }

    out.Write(R"(
#define bbox_left bbox_data[0]
#define bbox_right bbox_data[1]
#define bbox_top bbox_data[2]
#define bbox_bottom bbox_data[3]

void UpdateBoundingBoxBuffer(int2 min_pos, int2 max_pos) {{
  if (bbox_left > min_pos.x)
    atomicMin(bbox_left, min_pos.x);
  if (bbox_right < max_pos.x)
    atomicMax(bbox_right, max_pos.x);
  if (bbox_top > min_pos.y)
    atomicMin(bbox_top, min_pos.y);
  if (bbox_bottom < max_pos.y)
    atomicMax(bbox_bottom, max_pos.y);
}}

void UpdateBoundingBox(float2 rawpos) {{
  // The rightmost shaded pixel is not included in the right bounding box register,
  // such that width = right - left + 1. This has been verified on hardware.
  int2 pos = int2(rawpos * cefbscale);

#ifdef API_OPENGL
  // We need to invert the Y coordinate due to OpenGL's lower-left origin
  pos.y = {efb_height} - pos.y - 1;
#endif

  // The GC/Wii GPU rasterizes in 2x2 pixel groups, so bounding box values will be rounded to the
  // extents of these groups, rather than the exact pixel.
  int2 pos_tl = pos & ~1;
  int2 pos_br = pos | 1;

#ifdef SUPPORTS_SUBGROUP_REDUCTION
  if (CAN_USE_SUBGROUP_REDUCTION) {{
    int2 min_pos = IS_HELPER_INVOCATION ? int2(2147483647, 2147483647) : pos_tl;
    int2 max_pos = IS_HELPER_INVOCATION ? int2(-2147483648, -2147483648) : pos_br;
    SUBGROUP_MIN(min_pos);
    SUBGROUP_MAX(max_pos);
    if (IS_FIRST_ACTIVE_INVOCATION)
      UpdateBoundingBoxBuffer(min_pos, max_pos);
  }} else {{
    UpdateBoundingBoxBuffer(pos_tl, pos_br);
  }}
#else
  UpdateBoundingBoxBuffer(pos_tl, pos_br);
#endif
}}

)",
              fmt::arg("efb_height", EFB_HEIGHT));
  }
}

static void WriteStage(ShaderCode& out, const pixel_shader_uid_data* uid_data, int n,
                       APIType api_type, bool stereo);
static void WriteTevRegular(ShaderCode& out, std::string_view components, TevBias bias, TevOp op,
                            bool clamp, TevScale scale, bool alpha);
static void SampleTexture(ShaderCode& out, std::string_view texcoords, std::string_view texswap,
                          int texmap, bool stereo, APIType api_type);
static void WriteAlphaTest(ShaderCode& out, const pixel_shader_uid_data* uid_data, APIType api_type,
                           bool per_pixel_depth, bool use_dual_source);
static void WriteFog(ShaderCode& out, const pixel_shader_uid_data* uid_data);
static void WriteColor(ShaderCode& out, APIType api_type, const pixel_shader_uid_data* uid_data,
                       bool use_dual_source);
static void WriteBlend(ShaderCode& out, const pixel_shader_uid_data* uid_data);

ShaderCode GeneratePixelShaderCode(APIType api_type, const ShaderHostConfig& host_config,
                                   const pixel_shader_uid_data* uid_data)
{
  ShaderCode out;

  const bool per_pixel_lighting = g_ActiveConfig.bEnablePixelLighting;
  const bool msaa = host_config.msaa;
  const bool ssaa = host_config.ssaa;
  const bool stereo = host_config.stereo;
  const u32 numStages = uid_data->genMode_numtevstages + 1;

  out.Write("// Pixel Shader for TEV stages\n");
  out.Write("// {} TEV stages, {} texgens, {} IND stages\n", numStages,
            uid_data->genMode_numtexgens, uid_data->genMode_numindstages);

  // Stuff that is shared between ubershaders and pixelgen.
  WritePixelShaderCommonHeader(out, api_type, host_config, uid_data->bounding_box);

  if (uid_data->forced_early_z && g_ActiveConfig.backend_info.bSupportsEarlyZ)
  {
    // Zcomploc (aka early_ztest) is a way to control whether depth test is done before
    // or after texturing and alpha test. PC graphics APIs used to provide no way to emulate
    // this feature properly until 2012: Depth tests were always done after alpha testing.
    // Most importantly, it was not possible to write to the depth buffer without also writing
    // a color value (unless color writing was disabled altogether).

    // OpenGL 4.2 actually provides two extensions which can force an early z test:
    //  * ARB_image_load_store has 'layout(early_fragment_tests)' which forces the driver to do z
    //  and stencil tests early.
    //  * ARB_conservative_depth has 'layout(depth_unchanged) which signals to the driver that it
    //  can make optimisations
    //    which assume the pixel shader won't update the depth buffer.

    // early_fragment_tests is the best option, as it requires the driver to do early-z and defines
    // early-z exactly as
    // we expect, with discard causing the shader to exit with only the depth buffer updated.

    // Conservative depth's 'depth_unchanged' only hints to the driver that an early-z optimisation
    // can be made and
    // doesn't define what will happen if we discard the fragment. But the way modern graphics
    // hardware is implemented
    // means it is not unreasonable to expect the same behaviour as early_fragment_tests.
    // We can also assume that if a driver has gone out of its way to support conservative depth and
    // not image_load_store
    // as required by OpenGL 4.2 that it will be doing the optimisation.
    // If the driver doesn't actually do an early z optimisation, ZCompLoc will be broken and depth
    // will only be written
    // if the alpha test passes.

    // We support Conservative as a fallback, because many drivers based on Mesa haven't implemented
    // all of the
    // ARB_image_load_store extension yet.

    // D3D11 also has a way to force the driver to enable early-z, so we're fine here.
    if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
    {
      // This is a #define which signals whatever early-z method the driver supports.
      out.Write("FORCE_EARLY_Z; \n");
    }
    else
    {
      out.Write("[earlydepthstencil]\n");
    }
  }

  // Only use dual-source blending when required on drivers that don't support it very well.
  const bool use_dual_source =
      host_config.backend_dual_source_blend &&
      (!DriverDetails::HasBug(DriverDetails::BUG_BROKEN_DUAL_SOURCE_BLENDING) ||
       uid_data->useDstAlpha);
  const bool use_shader_blend =
      !use_dual_source && (uid_data->useDstAlpha && host_config.backend_shader_framebuffer_fetch);

  if (api_type == APIType::OpenGL || api_type == APIType::Vulkan)
  {
    if (use_dual_source)
    {
      if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_FRAGMENT_SHADER_INDEX_DECORATION))
      {
        out.Write("FRAGMENT_OUTPUT_LOCATION(0) out vec4 ocol0;\n"
                  "FRAGMENT_OUTPUT_LOCATION(1) out vec4 ocol1;\n");
      }
      else
      {
        out.Write("FRAGMENT_OUTPUT_LOCATION_INDEXED(0, 0) out vec4 ocol0;\n"
                  "FRAGMENT_OUTPUT_LOCATION_INDEXED(0, 1) out vec4 ocol1;\n");
      }
    }
    else if (use_shader_blend)
    {
      // QComm's Adreno driver doesn't seem to like using the framebuffer_fetch value as an
      // intermediate value with multiple reads & modifications, so pull out the "real" output value
      // and use a temporary for calculations, then set the output value once at the end of the
      // shader
      if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_FRAGMENT_SHADER_INDEX_DECORATION))
      {
        out.Write("FRAGMENT_OUTPUT_LOCATION(0) FRAGMENT_INOUT vec4 real_ocol0;\n");
      }
      else
      {
        out.Write("FRAGMENT_OUTPUT_LOCATION_INDEXED(0, 0) FRAGMENT_INOUT vec4 real_ocol0;\n");
      }
    }
    else
    {
      out.Write("FRAGMENT_OUTPUT_LOCATION(0) out vec4 ocol0;\n");
    }

    if (uid_data->per_pixel_depth)
      out.Write("#define depth gl_FragDepth\n");

    if (host_config.backend_geometry_shaders)
    {
      out.Write("VARYING_LOCATION(0) in VertexData {{\n");
      GenerateVSOutputMembers(out, api_type, uid_data->genMode_numtexgens, host_config,
                              GetInterpolationQualifier(msaa, ssaa, true, true));

      if (stereo)
        out.Write("\tflat int layer;\n");

      out.Write("}};\n");
    }
    else
    {
      // Let's set up attributes
      u32 counter = 0;
      out.Write("VARYING_LOCATION({}) {} in float4 colors_0;\n", counter++,
                GetInterpolationQualifier(msaa, ssaa));
      out.Write("VARYING_LOCATION({}) {} in float4 colors_1;\n", counter++,
                GetInterpolationQualifier(msaa, ssaa));
      for (u32 i = 0; i < uid_data->genMode_numtexgens; ++i)
      {
        out.Write("VARYING_LOCATION({}) {} in float3 tex{};\n", counter++,
                  GetInterpolationQualifier(msaa, ssaa), i);
      }
      if (!host_config.fast_depth_calc)
      {
        out.Write("VARYING_LOCATION({}) {} in float4 clipPos;\n", counter++,
                  GetInterpolationQualifier(msaa, ssaa));
      }
      if (per_pixel_lighting)
      {
        out.Write("VARYING_LOCATION({}) {} in float3 Normal;\n", counter++,
                  GetInterpolationQualifier(msaa, ssaa));
        out.Write("VARYING_LOCATION({}) {} in float3 WorldPos;\n", counter++,
                  GetInterpolationQualifier(msaa, ssaa));
      }
    }

    out.Write("void main()\n{{\n");
    out.Write("\tfloat4 rawpos = gl_FragCoord;\n");
    if (use_shader_blend)
    {
      // Store off a copy of the initial fb value for blending
      out.Write("\tfloat4 initial_ocol0 = FB_FETCH_VALUE;\n"
                "\tfloat4 ocol0;\n"
                "\tfloat4 ocol1;\n");
    }
  }
  else  // D3D
  {
    out.Write("void main(\n");
    if (uid_data->uint_output)
    {
      out.Write("  out uint4 ocol0 : SV_Target,\n");
    }
    else
    {
      out.Write("  out float4 ocol0 : SV_Target0,\n"
                "  out float4 ocol1 : SV_Target1,\n");
    }
    out.Write("{}"
              "  in float4 rawpos : SV_Position,\n",
              uid_data->per_pixel_depth ? "  out float depth : SV_Depth,\n" : "");

    out.Write("  in {} float4 colors_0 : COLOR0,\n", GetInterpolationQualifier(msaa, ssaa));
    out.Write("  in {} float4 colors_1 : COLOR1\n", GetInterpolationQualifier(msaa, ssaa));

    // compute window position if needed because binding semantic WPOS is not widely supported
    for (u32 i = 0; i < uid_data->genMode_numtexgens; ++i)
    {
      out.Write(",\n  in {} float3 tex{} : TEXCOORD{}", GetInterpolationQualifier(msaa, ssaa), i,
                i);
    }
    if (!host_config.fast_depth_calc)
    {
      out.Write(",\n  in {} float4 clipPos : TEXCOORD{}", GetInterpolationQualifier(msaa, ssaa),
                uid_data->genMode_numtexgens);
    }
    if (per_pixel_lighting)
    {
      out.Write(",\n  in {} float3 Normal : TEXCOORD{}", GetInterpolationQualifier(msaa, ssaa),
                uid_data->genMode_numtexgens + 1);
      out.Write(",\n  in {} float3 WorldPos : TEXCOORD{}", GetInterpolationQualifier(msaa, ssaa),
                uid_data->genMode_numtexgens + 2);
    }
    if (host_config.backend_geometry_shaders)
    {
      out.Write(",\n  in float clipDist0 : SV_ClipDistance0\n"
                ",\n  in float clipDist1 : SV_ClipDistance1\n");
    }
    if (stereo)
      out.Write(",\n  in uint layer : SV_RenderTargetArrayIndex\n");
    out.Write("        ) {{\n");
  }

  out.Write("\tint4 c0 = " I_COLORS "[1], c1 = " I_COLORS "[2], c2 = " I_COLORS
            "[3], prev = " I_COLORS "[0];\n"
            "\tint4 rastemp = int4(0, 0, 0, 0), textemp = int4(0, 0, 0, 0), konsttemp = int4(0, 0, "
            "0, 0);\n"
            "\tint3 comp16 = int3(1, 256, 0), comp24 = int3(1, 256, 256*256);\n"
            "\tint alphabump=0;\n"
            "\tint3 tevcoord=int3(0, 0, 0);\n"
            "\tint2 wrappedcoord=int2(0,0), tempcoord=int2(0,0);\n"
            "\tint4 "
            "tevin_a=int4(0,0,0,0),tevin_b=int4(0,0,0,0),tevin_c=int4(0,0,0,0),tevin_d=int4(0,0,0,"
            "0);\n\n");  // tev combiner inputs

  // On GLSL, input variables must not be assigned to.
  // This is why we declare these variables locally instead.
  out.Write("\tfloat4 col0 = colors_0;\n"
            "\tfloat4 col1 = colors_1;\n");

  if (per_pixel_lighting)
  {
    out.Write("\tfloat3 _norm0 = normalize(Normal.xyz);\n\n"
              "\tfloat3 pos = WorldPos;\n");

    out.Write("\tint4 lacc;\n"
              "\tfloat3 ldir, h, cosAttn, distAttn;\n"
              "\tfloat dist, dist2, attn;\n");

    // TODO: Our current constant usage code isn't able to handle more than one buffer.
    //       So we can't mark the VS constant as used here. But keep them here as reference.
    // out.SetConstantsUsed(C_PLIGHT_COLORS, C_PLIGHT_COLORS+7); // TODO: Can be optimized further
    // out.SetConstantsUsed(C_PLIGHTS, C_PLIGHTS+31); // TODO: Can be optimized further
    // out.SetConstantsUsed(C_PMATERIALS, C_PMATERIALS+3);
    GenerateLightingShaderCode(out, uid_data->lighting, "colors_", "col");
    if (uid_data->numColorChans == 0)
      out.Write("col0 = float4(0.0, 0.0, 0.0, 0.0);\n");
    if (uid_data->numColorChans <= 1)
      out.Write("col1 = float4(0.0, 0.0, 0.0, 0.0);\n");
  }

  if (uid_data->genMode_numtexgens == 0)
  {
    // TODO: This is a hack to ensure that shaders still compile when setting out of bounds tex
    // coord indices to 0.  Ideally, it shouldn't exist at all, but the exact behavior hasn't been
    // tested.
    out.Write("\tint2 fixpoint_uv0 = int2(0, 0);\n\n");
  }
  else
  {
    out.SetConstantsUsed(C_TEXDIMS, C_TEXDIMS + uid_data->genMode_numtexgens - 1);
    for (u32 i = 0; i < uid_data->genMode_numtexgens; ++i)
    {
      out.Write("\tint2 fixpoint_uv{} = int2(", i);
      out.Write("(tex{}.z == 0.0 ? tex{}.xy : tex{}.xy / tex{}.z)", i, i, i, i);
      out.Write(" * " I_TEXDIMS "[{}].zw);\n", i);
      // TODO: S24 overflows here?
    }
  }

  for (u32 i = 0; i < uid_data->genMode_numindstages; ++i)
  {
    if ((uid_data->nIndirectStagesUsed & (1U << i)) != 0)
    {
      u32 texcoord = uid_data->GetTevindirefCoord(i);
      const u32 texmap = uid_data->GetTevindirefMap(i);

      // Quirk: when the tex coord is not less than the number of tex gens (i.e. the tex coord does
      // not exist), then tex coord 0 is used (though sometimes glitchy effects happen on console).
      // This affects the Mario portrait in Luigi's Mansion, where the developers forgot to set
      // the number of tex gens to 2 (bug 11462).
      if (texcoord >= uid_data->genMode_numtexgens)
        texcoord = 0;

      out.SetConstantsUsed(C_INDTEXSCALE + i / 2, C_INDTEXSCALE + i / 2);
      out.Write("\ttempcoord = fixpoint_uv{} >> " I_INDTEXSCALE "[{}].{};\n", texcoord, i / 2,
                (i & 1) ? "zw" : "xy");

      out.Write("\tint3 iindtex{} = ", i);
      SampleTexture(out, "float2(tempcoord)", "abg", texmap, stereo, api_type);
    }
  }

  for (u32 i = 0; i < numStages; i++)
  {
    // Build the equation for this stage
    WriteStage(out, uid_data, i, api_type, stereo);
  }

  {
    // The results of the last texenv stage are put onto the screen,
    // regardless of the used destination register
    TevStageCombiner::ColorCombiner last_cc;
    TevStageCombiner::AlphaCombiner last_ac;
    last_cc.hex = uid_data->stagehash[uid_data->genMode_numtevstages].cc;
    last_ac.hex = uid_data->stagehash[uid_data->genMode_numtevstages].ac;
    if (last_cc.dest != TevOutput::Prev)
    {
      out.Write("\tprev.rgb = {};\n", tev_c_output_table[u32(last_cc.dest.Value())]);
    }
    if (last_ac.dest != TevOutput::Prev)
    {
      out.Write("\tprev.a = {};\n", tev_a_output_table[u32(last_ac.dest.Value())]);
    }
  }
  out.Write("\tprev = prev & 255;\n");

  // NOTE: Fragment may not be discarded if alpha test always fails and early depth test is enabled
  // (in this case we need to write a depth value if depth test passes regardless of the alpha
  // testing result)
  if (uid_data->Pretest == AlphaTestResult::Undetermined ||
      (uid_data->Pretest == AlphaTestResult::Fail && uid_data->late_ztest))
  {
    WriteAlphaTest(out, uid_data, api_type, uid_data->per_pixel_depth,
                   use_dual_source || use_shader_blend);
  }

  if (uid_data->zfreeze)
  {
    out.SetConstantsUsed(C_ZSLOPE, C_ZSLOPE);
    out.SetConstantsUsed(C_EFBSCALE, C_EFBSCALE);

    out.Write("\tfloat2 screenpos = rawpos.xy * " I_EFBSCALE ".xy;\n");

    // Opengl has reversed vertical screenspace coordinates
    if (api_type == APIType::OpenGL)
      out.Write("\tscreenpos.y = {}.0 - screenpos.y;\n", EFB_HEIGHT);

    out.Write("\tint zCoord = int(" I_ZSLOPE ".z + " I_ZSLOPE ".x * screenpos.x + " I_ZSLOPE
              ".y * screenpos.y);\n");
  }
  else if (!host_config.fast_depth_calc)
  {
    // FastDepth means to trust the depth generated in perspective division.
    // It should be correct, but it seems not to be as accurate as required. TODO: Find out why!
    // For disabled FastDepth we just calculate the depth value again.
    // The performance impact of this additional calculation doesn't matter, but it prevents
    // the host GPU driver from performing any early depth test optimizations.
    out.SetConstantsUsed(C_ZBIAS + 1, C_ZBIAS + 1);
    // the screen space depth value = far z + (clip z / clip w) * z range
    out.Write("\tint zCoord = " I_ZBIAS "[1].x + int((clipPos.z / clipPos.w) * float(" I_ZBIAS
              "[1].y));\n");
  }
  else
  {
    if (!host_config.backend_reversed_depth_range)
      out.Write("\tint zCoord = int((1.0 - rawpos.z) * 16777216.0);\n");
    else
      out.Write("\tint zCoord = int(rawpos.z * 16777216.0);\n");
  }
  out.Write("\tzCoord = clamp(zCoord, 0, 0xFFFFFF);\n");

  // depth texture can safely be ignored if the result won't be written to the depth buffer
  // (early_ztest) and isn't used for fog either
  const bool skip_ztexture = !uid_data->per_pixel_depth && uid_data->fog_fsel == FogType::Off;

  // Note: z-textures are not written to depth buffer if early depth test is used
  if (uid_data->per_pixel_depth && uid_data->early_ztest)
  {
    if (!host_config.backend_reversed_depth_range)
      out.Write("\tdepth = 1.0 - float(zCoord) / 16777216.0;\n");
    else
      out.Write("\tdepth = float(zCoord) / 16777216.0;\n");
  }

  // Note: depth texture output is only written to depth buffer if late depth test is used
  // theoretical final depth value is used for fog calculation, though, so we have to emulate
  // ztextures anyway
  if (uid_data->ztex_op != ZTexOp::Disabled && !skip_ztexture)
  {
    // use the texture input of the last texture stage (textemp), hopefully this has been read and
    // is in correct format...
    out.SetConstantsUsed(C_ZBIAS, C_ZBIAS + 1);
    out.Write("\tzCoord = idot(" I_ZBIAS "[0].xyzw, textemp.xyzw) + " I_ZBIAS "[1].w {};\n",
              (uid_data->ztex_op == ZTexOp::Add) ? "+ zCoord" : "");
    out.Write("\tzCoord = zCoord & 0xFFFFFF;\n");
  }

  if (uid_data->per_pixel_depth && uid_data->late_ztest)
  {
    if (!host_config.backend_reversed_depth_range)
      out.Write("\tdepth = 1.0 - float(zCoord) / 16777216.0;\n");
    else
      out.Write("\tdepth = float(zCoord) / 16777216.0;\n");
  }

  // No dithering for RGB8 mode
  if (uid_data->dither)
  {
    // Flipper uses a standard 2x2 Bayer Matrix for 6 bit dithering
    // Here the matrix is encoded into the two factor constants
    out.Write("\tint2 dither = int2(rawpos.xy) & 1;\n");
    out.Write("\tprev.rgb = (prev.rgb - (prev.rgb >> 6)) + abs(dither.y * 3 - dither.x * 2);\n");
  }

  WriteFog(out, uid_data);

  // Write the color and alpha values to the framebuffer
  // If using shader blend, we still use the separate alpha
  WriteColor(out, api_type, uid_data, use_dual_source || use_shader_blend);

  if (use_shader_blend)
    WriteBlend(out, uid_data);

  if (uid_data->bounding_box)
    out.Write("\tUpdateBoundingBox(rawpos.xy);\n");

  out.Write("}}\n");

  return out;
}

static void WriteStage(ShaderCode& out, const pixel_shader_uid_data* uid_data, int n,
                       APIType api_type, bool stereo)
{
  const auto& stage = uid_data->stagehash[n];
  out.Write("\n\t// TEV stage {}\n", n);

  // Quirk: when the tex coord is not less than the number of tex gens (i.e. the tex coord does not
  // exist), then tex coord 0 is used (though sometimes glitchy effects happen on console).
  u32 texcoord = stage.tevorders_texcoord;
  const bool has_tex_coord = texcoord < uid_data->genMode_numtexgens;
  if (!has_tex_coord)
    texcoord = 0;

  {
    const TevStageIndirect tevind{.hex = stage.tevind};
    out.Write("\t// indirect op\n");

    // Quirk: Referencing a stage above the number of ind stages is undefined behavior,
    // and on console produces a noise pattern (details unknown).
    // Instead, just skip applying the indirect operation, which is close enough.
    // We need to do *something*, as there won't be an iindtex variable otherwise.
    // Viewtiful Joe hits this case (bug 12525).
    // Wrapping and add to previous still apply in this case (and when the stage is disabled).
    const bool has_ind_stage = tevind.bt < uid_data->genMode_numindstages;

    // Perform the indirect op on the incoming regular coordinates
    // using iindtex{} as the offset coords
    if (has_ind_stage && tevind.bs != IndTexBumpAlpha::Off)
    {
      static constexpr std::array<const char*, 4> tev_ind_alpha_sel{
          "",
          "x",
          "y",
          "z",
      };

      // 0b11111000, 0b11100000, 0b11110000, 0b11111000
      static constexpr std::array<const char*, 4> tev_ind_alpha_mask{
          "248",
          "224",
          "240",
          "248",
      };

      out.Write("alphabump = iindtex{}.{} & {};\n", tevind.bt.Value(),
                tev_ind_alpha_sel[u32(tevind.bs.Value())],
                tev_ind_alpha_mask[u32(tevind.fmt.Value())]);
    }
    else
    {
      // TODO: Should we reset alphabump to 0 here?
    }

    if (has_ind_stage && tevind.matrix_index != IndMtxIndex::Off)
    {
      // format
      static constexpr std::array<const char*, 4> tev_ind_fmt_mask{
          "255",
          "31",
          "15",
          "7",
      };
      out.Write("\tint3 iindtevcrd{} = iindtex{} & {};\n", n, tevind.bt.Value(),
                tev_ind_fmt_mask[u32(tevind.fmt.Value())]);

      // bias - TODO: Check if this needs to be this complicated...
      // indexed by bias
      static constexpr std::array<const char*, 8> tev_ind_bias_field{
          "", "x", "y", "xy", "z", "xz", "yz", "xyz",
      };

      // indexed by fmt
      static constexpr std::array<const char*, 4> tev_ind_bias_add{
          "-128",
          "1",
          "1",
          "1",
      };

      if (tevind.bias == IndTexBias::S || tevind.bias == IndTexBias::T ||
          tevind.bias == IndTexBias::U)
      {
        out.Write("\tiindtevcrd{}.{} += int({});\n", n,
                  tev_ind_bias_field[u32(tevind.bias.Value())],
                  tev_ind_bias_add[u32(tevind.fmt.Value())]);
      }
      else if (tevind.bias == IndTexBias::ST || tevind.bias == IndTexBias::SU ||
               tevind.bias == IndTexBias::TU_)
      {
        out.Write("\tiindtevcrd{0}.{1} += int2({2}, {2});\n", n,
                  tev_ind_bias_field[u32(tevind.bias.Value())],
                  tev_ind_bias_add[u32(tevind.fmt.Value())]);
      }
      else if (tevind.bias == IndTexBias::STU)
      {
        out.Write("\tiindtevcrd{0}.{1} += int3({2}, {2}, {2});\n", n,
                  tev_ind_bias_field[u32(tevind.bias.Value())],
                  tev_ind_bias_add[u32(tevind.fmt.Value())]);
      }

      // Multiplied by 2 because each matrix has two rows.
      // Note also that the 4th column of the matrix contains the scale factor.
      const u32 mtxidx = 2 * (static_cast<u32>(tevind.matrix_index.Value()) - 1);

      // multiply by offset matrix and scale - calculations are likely to overflow badly,
      // yet it works out since we only care about the lower 23 bits (+1 sign bit) of the result
      if (tevind.matrix_id == IndMtxId::Indirect)
      {
        out.SetConstantsUsed(C_INDTEXMTX + mtxidx, C_INDTEXMTX + mtxidx);

        out.Write("\tint2 indtevtrans{} = int2(idot(" I_INDTEXMTX
                  "[{}].xyz, iindtevcrd{}), idot(" I_INDTEXMTX "[{}].xyz, iindtevcrd{})) >> 3;\n",
                  n, mtxidx, n, mtxidx + 1, n);

        // TODO: should use a shader uid branch for this for better performance
        if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_BITWISE_OP_NEGATION))
        {
          out.Write("\tint indtexmtx_w_inverse_{} = -" I_INDTEXMTX "[{}].w;\n", n, mtxidx);
          out.Write("\tif (" I_INDTEXMTX "[{}].w >= 0) indtevtrans{} >>= " I_INDTEXMTX "[{}].w;\n",
                    mtxidx, n, mtxidx);
          out.Write("\telse indtevtrans{} <<= indtexmtx_w_inverse_{};\n", n, n);
        }
        else
        {
          out.Write("\tif (" I_INDTEXMTX "[{}].w >= 0) indtevtrans{} >>= " I_INDTEXMTX "[{}].w;\n",
                    mtxidx, n, mtxidx);
          out.Write("\telse indtevtrans{} <<= (-" I_INDTEXMTX "[{}].w);\n", n, mtxidx);
        }
      }
      else if (tevind.matrix_id == IndMtxId::S)
      {
        ASSERT(has_tex_coord);
        out.SetConstantsUsed(C_INDTEXMTX + mtxidx, C_INDTEXMTX + mtxidx);

        out.Write("\tint2 indtevtrans{} = int2(fixpoint_uv{} * iindtevcrd{}.xx) >> 8;\n", n,
                  texcoord, n);
        if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_BITWISE_OP_NEGATION))
        {
          out.Write("\tint  indtexmtx_w_inverse_{} = -" I_INDTEXMTX "[{}].w;\n", n, mtxidx);
          out.Write("\tif (" I_INDTEXMTX "[{}].w >= 0) indtevtrans{} >>= " I_INDTEXMTX "[{}].w;\n",
                    mtxidx, n, mtxidx);
          out.Write("\telse indtevtrans{} <<= (indtexmtx_w_inverse_{});\n", n, n);
        }
        else
        {
          out.Write("\tif (" I_INDTEXMTX "[{}].w >= 0) indtevtrans{} >>= " I_INDTEXMTX "[{}].w;\n",
                    mtxidx, n, mtxidx);
          out.Write("\telse indtevtrans{} <<= (-" I_INDTEXMTX "[{}].w);\n", n, mtxidx);
        }
      }
      else if (tevind.matrix_id == IndMtxId::T)
      {
        ASSERT(has_tex_coord);
        out.SetConstantsUsed(C_INDTEXMTX + mtxidx, C_INDTEXMTX + mtxidx);

        out.Write("\tint2 indtevtrans{} = int2(fixpoint_uv{} * iindtevcrd{}.yy) >> 8;\n", n,
                  texcoord, n);

        if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_BITWISE_OP_NEGATION))
        {
          out.Write("\tint  indtexmtx_w_inverse_{} = -" I_INDTEXMTX "[{}].w;\n", n, mtxidx);
          out.Write("\tif (" I_INDTEXMTX "[{}].w >= 0) indtevtrans{} >>= " I_INDTEXMTX "[{}].w;\n",
                    mtxidx, n, mtxidx);
          out.Write("\telse indtevtrans{} <<= (indtexmtx_w_inverse_{});\n", n, n);
        }
        else
        {
          out.Write("\tif (" I_INDTEXMTX "[{}].w >= 0) indtevtrans{} >>= " I_INDTEXMTX "[{}].w;\n",
                    mtxidx, n, mtxidx);
          out.Write("\telse indtevtrans{} <<= (-" I_INDTEXMTX "[{}].w);\n", n, mtxidx);
        }
      }
      else
      {
        out.Write("\tint2 indtevtrans{} = int2(0, 0);\n", n);
        ASSERT(false);  // Unknown value for matrix_id
      }
    }
    else
    {
      out.Write("\tint2 indtevtrans{} = int2(0, 0);\n", n);
      if (tevind.matrix_index == IndMtxIndex::Off)
      {
        // If matrix_index is Off (0), matrix_id should be Indirect (0)
        ASSERT(tevind.matrix_id == IndMtxId::Indirect);
      }
    }

    // ---------
    // Wrapping
    // ---------

    static constexpr std::array<const char*, 5> tev_ind_wrap_start{
        "(256<<7)", "(128<<7)", "(64<<7)", "(32<<7)", "(16<<7)",
    };

    // wrap S
    if (tevind.sw == IndTexWrap::ITW_OFF)
    {
      out.Write("\twrappedcoord.x = fixpoint_uv{}.x;\n", texcoord);
    }
    else if (tevind.sw >= IndTexWrap::ITW_0)  // 7 (Invalid) appears to behave the same as 6 (ITW_0)
    {
      out.Write("\twrappedcoord.x = 0;\n");
    }
    else
    {
      out.Write("\twrappedcoord.x = fixpoint_uv{}.x & ({} - 1);\n", texcoord,
                tev_ind_wrap_start[u32(tevind.sw.Value()) - u32(IndTexWrap::ITW_256)]);
    }

    // wrap T
    if (tevind.tw == IndTexWrap::ITW_OFF)
    {
      out.Write("\twrappedcoord.y = fixpoint_uv{}.y;\n", texcoord);
    }
    else if (tevind.tw >= IndTexWrap::ITW_0)  // 7 (Invalid) appears to behave the same as 6 (ITW_0)
    {
      out.Write("\twrappedcoord.y = 0;\n");
    }
    else
    {
      out.Write("\twrappedcoord.y = fixpoint_uv{}.y & ({} - 1);\n", texcoord,
                tev_ind_wrap_start[u32(tevind.tw.Value()) - u32(IndTexWrap::ITW_256)]);
    }

    if (tevind.fb_addprev)  // add previous tevcoord
      out.Write("\ttevcoord.xy += wrappedcoord + indtevtrans{};\n", n);
    else
      out.Write("\ttevcoord.xy = wrappedcoord + indtevtrans{};\n", n);

    // Emulate s24 overflows
    out.Write("\ttevcoord.xy = (tevcoord.xy << 8) >> 8;\n");
  }

  TevStageCombiner::ColorCombiner cc;
  TevStageCombiner::AlphaCombiner ac;
  cc.hex = stage.cc;
  ac.hex = stage.ac;

  if (cc.a == TevColorArg::RasAlpha || cc.a == TevColorArg::RasColor ||
      cc.b == TevColorArg::RasAlpha || cc.b == TevColorArg::RasColor ||
      cc.c == TevColorArg::RasAlpha || cc.c == TevColorArg::RasColor ||
      cc.d == TevColorArg::RasAlpha || cc.d == TevColorArg::RasColor ||
      ac.a == TevAlphaArg::RasAlpha || ac.b == TevAlphaArg::RasAlpha ||
      ac.c == TevAlphaArg::RasAlpha || ac.d == TevAlphaArg::RasAlpha)
  {
    // Generate swizzle string to represent the Ras color channel swapping
    const char rasswap[5] = {
        "rgba"[stage.tevksel_swap1a],
        "rgba"[stage.tevksel_swap2a],
        "rgba"[stage.tevksel_swap1b],
        "rgba"[stage.tevksel_swap2b],
        '\0',
    };

    out.Write("\trastemp = {}.{};\n", tev_ras_table[u32(stage.tevorders_colorchan)], rasswap);
  }

  if (stage.tevorders_enable && uid_data->genMode_numtexgens > 0)
  {
    // Generate swizzle string to represent the texture color channel swapping
    const char texswap[5] = {
        "rgba"[stage.tevksel_swap1c],
        "rgba"[stage.tevksel_swap2c],
        "rgba"[stage.tevksel_swap1d],
        "rgba"[stage.tevksel_swap2d],
        '\0',
    };

    out.Write("\ttextemp = ");
    SampleTexture(out, "float2(tevcoord.xy)", texswap, stage.tevorders_texmap, stereo, api_type);
  }
  else if (uid_data->genMode_numtexgens == 0)
  {
    // It seems like the result is always black when no tex coords are enabled, but further testing
    // is needed.
    out.Write("\ttextemp = int4(0, 0, 0, 0);\n");
  }
  else
  {
    out.Write("\ttextemp = int4(255, 255, 255, 255);\n");
  }

  if (cc.a == TevColorArg::Konst || cc.b == TevColorArg::Konst || cc.c == TevColorArg::Konst ||
      cc.d == TevColorArg::Konst || ac.a == TevAlphaArg::Konst || ac.b == TevAlphaArg::Konst ||
      ac.c == TevAlphaArg::Konst || ac.d == TevAlphaArg::Konst)
  {
    out.Write("\tkonsttemp = int4({}, {});\n", tev_ksel_table_c[u32(stage.tevksel_kc)],
              tev_ksel_table_a[u32(stage.tevksel_ka)]);

    if (u32(stage.tevksel_kc) > 7)
    {
      out.SetConstantsUsed(C_KCOLORS + ((u32(stage.tevksel_kc) - 0xc) % 4),
                           C_KCOLORS + ((u32(stage.tevksel_kc) - 0xc) % 4));
    }
    if (u32(stage.tevksel_ka) > 7)
    {
      out.SetConstantsUsed(C_KCOLORS + ((u32(stage.tevksel_ka) - 0xc) % 4),
                           C_KCOLORS + ((u32(stage.tevksel_ka) - 0xc) % 4));
    }
  }

  if (cc.d == TevColorArg::Color0 || cc.d == TevColorArg::Alpha0 || ac.d == TevAlphaArg::Alpha0)
    out.SetConstantsUsed(C_COLORS + 1, C_COLORS + 1);

  if (cc.d == TevColorArg::Color1 || cc.d == TevColorArg::Alpha1 || ac.d == TevAlphaArg::Alpha1)
    out.SetConstantsUsed(C_COLORS + 2, C_COLORS + 2);

  if (cc.d == TevColorArg::Color2 || cc.d == TevColorArg::Alpha2 || ac.d == TevAlphaArg::Alpha2)
    out.SetConstantsUsed(C_COLORS + 3, C_COLORS + 3);

  if (cc.dest >= TevOutput::Color0)
    out.SetConstantsUsed(C_COLORS + u32(cc.dest.Value()), C_COLORS + u32(cc.dest.Value()));

  if (ac.dest >= TevOutput::Color0)
    out.SetConstantsUsed(C_COLORS + u32(ac.dest.Value()), C_COLORS + u32(ac.dest.Value()));

  if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_VECTOR_BITWISE_AND))
  {
    out.Write("\ttevin_a = int4({} & 255, {} & 255);\n", tev_c_input_table[u32(cc.a.Value())],
              tev_a_input_table[u32(ac.a.Value())]);
    out.Write("\ttevin_b = int4({} & 255, {} & 255);\n", tev_c_input_table[u32(cc.b.Value())],
              tev_a_input_table[u32(ac.b.Value())]);
    out.Write("\ttevin_c = int4({} & 255, {} & 255);\n", tev_c_input_table[u32(cc.c.Value())],
              tev_a_input_table[u32(ac.c.Value())]);
  }
  else
  {
    out.Write("\ttevin_a = int4({}, {})&int4(255, 255, 255, 255);\n",
              tev_c_input_table[u32(cc.a.Value())], tev_a_input_table[u32(ac.a.Value())]);
    out.Write("\ttevin_b = int4({}, {})&int4(255, 255, 255, 255);\n",
              tev_c_input_table[u32(cc.b.Value())], tev_a_input_table[u32(ac.b.Value())]);
    out.Write("\ttevin_c = int4({}, {})&int4(255, 255, 255, 255);\n",
              tev_c_input_table[u32(cc.c.Value())], tev_a_input_table[u32(ac.c.Value())]);
  }
  out.Write("\ttevin_d = int4({}, {});\n", tev_c_input_table[u32(cc.d.Value())],
            tev_a_input_table[u32(ac.d.Value())]);

  out.Write("\t// color combine\n");
  out.Write("\t{} = clamp(", tev_c_output_table[u32(cc.dest.Value())]);
  if (cc.bias != TevBias::Compare)
  {
    WriteTevRegular(out, "rgb", cc.bias, cc.op, cc.clamp, cc.scale, false);
  }
  else
  {
    static constexpr std::array<const char*, 8> function_table{
        "((tevin_a.r > tevin_b.r) ? tevin_c.rgb : int3(0,0,0))",   // TevCompareMode::R8, GT
        "((tevin_a.r == tevin_b.r) ? tevin_c.rgb : int3(0,0,0))",  // R8, TevComparison::EQ
        "((idot(tevin_a.rgb, comp16) >  idot(tevin_b.rgb, comp16)) ? tevin_c.rgb : "
        "int3(0,0,0))",  // GR16, GT
        "((idot(tevin_a.rgb, comp16) == idot(tevin_b.rgb, comp16)) ? tevin_c.rgb : "
        "int3(0,0,0))",  // GR16, EQ
        "((idot(tevin_a.rgb, comp24) >  idot(tevin_b.rgb, comp24)) ? tevin_c.rgb : "
        "int3(0,0,0))",  // BGR24, GT
        "((idot(tevin_a.rgb, comp24) == idot(tevin_b.rgb, comp24)) ? tevin_c.rgb : "
        "int3(0,0,0))",                                                         // BGR24, EQ
        "(max(sign(tevin_a.rgb - tevin_b.rgb), int3(0,0,0)) * tevin_c.rgb)",    // RGB8, GT
        "((int3(1,1,1) - sign(abs(tevin_a.rgb - tevin_b.rgb))) * tevin_c.rgb)"  // RGB8, EQ
    };

    const u32 mode = (u32(cc.compare_mode.Value()) << 1) | u32(cc.comparison.Value());
    out.Write("   tevin_d.rgb + ");
    out.Write("{}", function_table[mode]);
  }
  if (cc.clamp)
    out.Write(", int3(0,0,0), int3(255,255,255))");
  else
    out.Write(", int3(-1024,-1024,-1024), int3(1023,1023,1023))");
  out.Write(";\n");

  out.Write("\t// alpha combine\n");
  out.Write("\t{} = clamp(", tev_a_output_table[u32(ac.dest.Value())]);
  if (ac.bias != TevBias::Compare)
  {
    WriteTevRegular(out, "a", ac.bias, ac.op, ac.clamp, ac.scale, true);
  }
  else
  {
    static constexpr std::array<const char*, 8> function_table{
        "((tevin_a.r > tevin_b.r) ? tevin_c.a : 0)",   // TevCompareMode::R8, GT
        "((tevin_a.r == tevin_b.r) ? tevin_c.a : 0)",  // R8, TevComparison::EQ
        "((idot(tevin_a.rgb, comp16) >  idot(tevin_b.rgb, comp16)) ? tevin_c.a : 0)",  // GR16, GT
        "((idot(tevin_a.rgb, comp16) == idot(tevin_b.rgb, comp16)) ? tevin_c.a : 0)",  // GR16, EQ
        "((idot(tevin_a.rgb, comp24) >  idot(tevin_b.rgb, comp24)) ? tevin_c.a : 0)",  // BGR24, GT
        "((idot(tevin_a.rgb, comp24) == idot(tevin_b.rgb, comp24)) ? tevin_c.a : 0)",  // BGR24, EQ
        "((tevin_a.a >  tevin_b.a) ? tevin_c.a : 0)",                                  // A8, GT
        "((tevin_a.a == tevin_b.a) ? tevin_c.a : 0)"                                   // A8, EQ
    };

    const u32 mode = (u32(ac.compare_mode.Value()) << 1) | u32(ac.comparison.Value());
    out.Write("   tevin_d.a + ");
    out.Write("{}", function_table[mode]);
  }
  if (ac.clamp)
    out.Write(", 0, 255)");
  else
    out.Write(", -1024, 1023)");

  out.Write(";\n");
}

static void WriteTevRegular(ShaderCode& out, std::string_view components, TevBias bias, TevOp op,
                            bool clamp, TevScale scale, bool alpha)
{
  static constexpr std::array<const char*, 4> tev_scale_table_left{
      "",       // Scale1
      " << 1",  // Scale2
      " << 2",  // Scale4
      "",       // Divide2
  };

  static constexpr std::array<const char*, 4> tev_scale_table_right{
      "",       // Scale1
      "",       // Scale2
      "",       // Scale4
      " >> 1",  // Divide2
  };

  // indexed by 2*op+(scale==Divide2)
  static constexpr std::array<const char*, 4> tev_lerp_bias{
      "",
      " + 128",
      "",
      " + 127",
  };

  static constexpr std::array<const char*, 4> tev_bias_table{
      "",        // Zero,
      " + 128",  // AddHalf,
      " - 128",  // SubHalf,
      "",
  };

  static constexpr std::array<char, 2> tev_op_table{
      '+',  // TevOp::Add = 0,
      '-',  // TevOp::Sub = 1,
  };

  // Regular TEV stage: (d + bias + lerp(a,b,c)) * scale
  // The GameCube/Wii GPU uses a very sophisticated algorithm for scale-lerping:
  // - c is scaled from 0..255 to 0..256, which allows dividing the result by 256 instead of 255
  // - if scale is bigger than one, it is moved inside the lerp calculation for increased accuracy
  // - a rounding bias is added before dividing by 256
  out.Write("(((tevin_d.{}{}){})", components, tev_bias_table[u32(bias)],
            tev_scale_table_left[u32(scale)]);
  out.Write(" {} ", tev_op_table[u32(op)]);
  out.Write("(((((tevin_a.{}<<8) + (tevin_b.{}-tevin_a.{})*(tevin_c.{}+(tevin_c.{}>>7))){}){})>>8)",
            components, components, components, components, components,
            tev_scale_table_left[u32(scale)],
            tev_lerp_bias[2 * u32(op) + ((scale == TevScale::Divide2) == alpha)]);
  out.Write("){}", tev_scale_table_right[u32(scale)]);
}

static void SampleTexture(ShaderCode& out, std::string_view texcoords, std::string_view texswap,
                          int texmap, bool stereo, APIType api_type)
{
  out.SetConstantsUsed(C_TEXDIMS + texmap, C_TEXDIMS + texmap);

  if (api_type == APIType::D3D)
  {
    out.Write("iround(255.0 * Tex[{}].Sample(samp[{}], float3({}.xy * " I_TEXDIMS
              "[{}].xy, {}))).{};\n",
              texmap, texmap, texcoords, texmap, stereo ? "layer" : "0.0", texswap);
  }
  else
  {
    out.Write("iround(255.0 * texture(samp[{}], float3({}.xy * " I_TEXDIMS "[{}].xy, {}))).{};\n",
              texmap, texcoords, texmap, stereo ? "layer" : "0.0", texswap);
  }
}

constexpr std::array<const char*, 8> tev_alpha_funcs_table{
    "(false)",         // CompareMode::Never
    "(prev.a <  {})",  // CompareMode::Less
    "(prev.a == {})",  // CompareMode::Equal
    "(prev.a <= {})",  // CompareMode::LEqual
    "(prev.a >  {})",  // CompareMode::Greater
    "(prev.a != {})",  // CompareMode::NEqual
    "(prev.a >= {})",  // CompareMode::GEqual
    "(true)"           // CompareMode::Always
};

constexpr std::array<const char*, 4> tev_alpha_funclogic_table{
    " && ",  // and
    " || ",  // or
    " != ",  // xor
    " == "   // xnor
};

static void WriteAlphaTest(ShaderCode& out, const pixel_shader_uid_data* uid_data, APIType api_type,
                           bool per_pixel_depth, bool use_dual_source)
{
  static constexpr std::array<std::string_view, 2> alpha_ref{
      I_ALPHA ".r",
      I_ALPHA ".g",
  };

  const auto write_alpha_func = [&out](CompareMode mode, std::string_view ref) {
    const bool has_no_arguments = mode == CompareMode::Never || mode == CompareMode::Always;
    if (has_no_arguments)
      out.Write("{}", tev_alpha_funcs_table[u32(mode)]);
    else
      out.Write(tev_alpha_funcs_table[u32(mode)], ref);
  };

  out.SetConstantsUsed(C_ALPHA, C_ALPHA);

  if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_NEGATED_BOOLEAN))
    out.Write("\tif(( ");
  else
    out.Write("\tif(!( ");

  // Lookup the first component from the alpha function table
  write_alpha_func(uid_data->alpha_test_comp0, alpha_ref[0]);

  // Lookup the logic op
  out.Write("{}", tev_alpha_funclogic_table[u32(uid_data->alpha_test_logic)]);

  // Lookup the second component from the alpha function table
  write_alpha_func(uid_data->alpha_test_comp1, alpha_ref[1]);

  if (DriverDetails::HasBug(DriverDetails::BUG_BROKEN_NEGATED_BOOLEAN))
    out.Write(") == false) {{\n");
  else
    out.Write(")) {{\n");

  out.Write("\t\tocol0 = float4(0.0, 0.0, 0.0, 0.0);\n");
  if (use_dual_source && !(api_type == APIType::D3D && uid_data->uint_output))
    out.Write("\t\tocol1 = float4(0.0, 0.0, 0.0, 0.0);\n");
  if (per_pixel_depth)
  {
    out.Write("\t\tdepth = {};\n",
              !g_ActiveConfig.backend_info.bSupportsReversedDepthRange ? "0.0" : "1.0");
  }

  // ZCOMPLOC HACK:
  if (!uid_data->alpha_test_use_zcomploc_hack)
  {
    out.Write("\t\tdiscard;\n");
    if (api_type == APIType::D3D)
      out.Write("\t\treturn;\n");
  }

  out.Write("\t}}\n");
}

constexpr std::array<const char*, 8> tev_fog_funcs_table{
    "",                                                       // No Fog
    "",                                                       // ?
    "",                                                       // Linear
    "",                                                       // ?
    "\tfog = 1.0 - exp2(-8.0 * fog);\n",                      // exp
    "\tfog = 1.0 - exp2(-8.0 * fog * fog);\n",                // exp2
    "\tfog = exp2(-8.0 * (1.0 - fog));\n",                    // backward exp
    "\tfog = 1.0 - fog;\n   fog = exp2(-8.0 * fog * fog);\n"  // backward exp2
};

static void WriteFog(ShaderCode& out, const pixel_shader_uid_data* uid_data)
{
  if (uid_data->fog_fsel == FogType::Off)
    return;  // no Fog

  out.SetConstantsUsed(C_FOGCOLOR, C_FOGCOLOR);
  out.SetConstantsUsed(C_FOGI, C_FOGI);
  out.SetConstantsUsed(C_FOGF, C_FOGF + 1);
  if (uid_data->fog_proj == FogProjection::Perspective)
  {
    // perspective
    // ze = A/(B - (Zs >> B_SHF)
    // TODO: Verify that we want to drop lower bits here! (currently taken over from software
    // renderer)
    //       Maybe we want to use "ze = (A << B_SHF)/((B << B_SHF) - Zs)" instead?
    //       That's equivalent, but keeps the lower bits of Zs.
    out.Write("\tfloat ze = (" I_FOGF ".x * 16777216.0) / float(" I_FOGI ".y - (zCoord >> " I_FOGI
              ".w));\n");
  }
  else
  {
    // orthographic
    // ze = a*Zs    (here, no B_SHF)
    out.Write("\tfloat ze = " I_FOGF ".x * float(zCoord) / 16777216.0;\n");
  }

  // x_adjust = sqrt((x-center)^2 + k^2)/k
  // ze *= x_adjust
  if (uid_data->fog_RangeBaseEnabled)
  {
    out.SetConstantsUsed(C_FOGF, C_FOGF);
    out.Write("\tfloat offset = (2.0 * (rawpos.x / " I_FOGF ".w)) - 1.0 - " I_FOGF ".z;\n"
              "\tfloat floatindex = clamp(9.0 - abs(offset) * 9.0, 0.0, 9.0);\n"
              "\tuint indexlower = uint(floatindex);\n"
              "\tuint indexupper = indexlower + 1u;\n"
              "\tfloat klower = " I_FOGRANGE "[indexlower >> 2u][indexlower & 3u];\n"
              "\tfloat kupper = " I_FOGRANGE "[indexupper >> 2u][indexupper & 3u];\n"
              "\tfloat k = lerp(klower, kupper, frac(floatindex));\n"
              "\tfloat x_adjust = sqrt(offset * offset + k * k) / k;\n"
              "\tze *= x_adjust;\n");
  }

  out.Write("\tfloat fog = clamp(ze - " I_FOGF ".y, 0.0, 1.0);\n");

  if (uid_data->fog_fsel >= FogType::Exp)
  {
    out.Write("{}", tev_fog_funcs_table[u32(uid_data->fog_fsel)]);
  }
  else
  {
    if (uid_data->fog_fsel != FogType::Linear)
      WARN_LOG_FMT(VIDEO, "Unknown Fog Type! {}", uid_data->fog_fsel);
  }

  out.Write("\tint ifog = iround(fog * 256.0);\n");
  out.Write("\tprev.rgb = (prev.rgb * (256 - ifog) + " I_FOGCOLOR ".rgb * ifog) >> 8;\n");
}

static void WriteColor(ShaderCode& out, APIType api_type, const pixel_shader_uid_data* uid_data,
                       bool use_dual_source)
{
  // D3D requires that the shader outputs be uint when writing to a uint render target for logic op.
  if (api_type == APIType::D3D && uid_data->uint_output)
  {
    if (uid_data->rgba6_format)
      out.Write("\tocol0 = uint4(prev & 0xFC);\n");
    else
      out.Write("\tocol0 = uint4(prev);\n");
    return;
  }

  if (uid_data->rgba6_format)
    out.Write("\tocol0.rgb = float3(prev.rgb >> 2) / 63.0;\n");
  else
    out.Write("\tocol0.rgb = float3(prev.rgb) / 255.0;\n");

  // Colors will be blended against the 8-bit alpha from ocol1 and
  // the 6-bit alpha from ocol0 will be written to the framebuffer
  if (uid_data->useDstAlpha)
  {
    out.SetConstantsUsed(C_ALPHA, C_ALPHA);
    out.Write("\tocol0.a = float(" I_ALPHA ".a >> 2) / 63.0;\n");

    // Use dual-source color blending to perform dst alpha in a single pass
    if (use_dual_source)
      out.Write("\tocol1 = float4(0.0, 0.0, 0.0, float(prev.a) / 255.0);\n");
  }
  else
  {
    out.Write("\tocol0.a = float(prev.a >> 2) / 63.0;\n");
    if (use_dual_source)
      out.Write("\tocol1 = float4(0.0, 0.0, 0.0, float(prev.a) / 255.0);\n");
  }
}

static void WriteBlend(ShaderCode& out, const pixel_shader_uid_data* uid_data)
{
  if (uid_data->blend_enable)
  {
    static constexpr std::array<const char*, 8> blend_src_factor{
        "float3(0,0,0);",                      // ZERO
        "float3(1,1,1);",                      // ONE
        "initial_ocol0.rgb;",                  // DSTCLR
        "float3(1,1,1) - initial_ocol0.rgb;",  // INVDSTCLR
        "ocol1.aaa;",                          // SRCALPHA
        "float3(1,1,1) - ocol1.aaa;",          // INVSRCALPHA
        "initial_ocol0.aaa;",                  // DSTALPHA
        "float3(1,1,1) - initial_ocol0.aaa;",  // INVDSTALPHA
    };
    static constexpr std::array<const char*, 8> blend_src_factor_alpha{
        "0.0;",                    // ZERO
        "1.0;",                    // ONE
        "initial_ocol0.a;",        // DSTCLR
        "1.0 - initial_ocol0.a;",  // INVDSTCLR
        "ocol1.a;",                // SRCALPHA
        "1.0 - ocol1.a;",          // INVSRCALPHA
        "initial_ocol0.a;",        // DSTALPHA
        "1.0 - initial_ocol0.a;",  // INVDSTALPHA
    };
    static constexpr std::array<const char*, 8> blend_dst_factor{
        "float3(0,0,0);",                      // ZERO
        "float3(1,1,1);",                      // ONE
        "ocol0.rgb;",                          // SRCCLR
        "float3(1,1,1) - ocol0.rgb;",          // INVSRCCLR
        "ocol1.aaa;",                          // SRCALHA
        "float3(1,1,1) - ocol1.aaa;",          // INVSRCALPHA
        "initial_ocol0.aaa;",                  // DSTALPHA
        "float3(1,1,1) - initial_ocol0.aaa;",  // INVDSTALPHA
    };
    static constexpr std::array<const char*, 8> blend_dst_factor_alpha{
        "0.0;",                    // ZERO
        "1.0;",                    // ONE
        "ocol0.a;",                // SRCCLR
        "1.0 - ocol0.a;",          // INVSRCCLR
        "ocol1.a;",                // SRCALPHA
        "1.0 - ocol1.a;",          // INVSRCALPHA
        "initial_ocol0.a;",        // DSTALPHA
        "1.0 - initial_ocol0.a;",  // INVDSTALPHA
    };
    out.Write("\tfloat4 blend_src;\n");
    out.Write("\tblend_src.rgb = {}\n", blend_src_factor[u32(uid_data->blend_src_factor)]);
    out.Write("\tblend_src.a = {}\n",
              blend_src_factor_alpha[u32(uid_data->blend_src_factor_alpha)]);
    out.Write("\tfloat4 blend_dst;\n");
    out.Write("\tblend_dst.rgb = {}\n", blend_dst_factor[u32(uid_data->blend_dst_factor)]);
    out.Write("\tblend_dst.a = {}\n",
              blend_dst_factor_alpha[u32(uid_data->blend_dst_factor_alpha)]);

    out.Write("\tfloat4 blend_result;\n");
    if (uid_data->blend_subtract)
    {
      out.Write("\tblend_result.rgb = initial_ocol0.rgb * blend_dst.rgb - ocol0.rgb * "
                "blend_src.rgb;\n");
    }
    else
    {
      out.Write(
          "\tblend_result.rgb = initial_ocol0.rgb * blend_dst.rgb + ocol0.rgb * blend_src.rgb;\n");
    }

    if (uid_data->blend_subtract_alpha)
      out.Write("\tblend_result.a = initial_ocol0.a * blend_dst.a - ocol0.a * blend_src.a;\n");
    else
      out.Write("\tblend_result.a = initial_ocol0.a * blend_dst.a + ocol0.a * blend_src.a;\n");
  }
  else
  {
    out.Write("\tfloat4 blend_result = ocol0;\n");
  }

  out.Write("\treal_ocol0 = blend_result;\n");
}
