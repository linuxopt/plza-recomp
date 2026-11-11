// Vulkan textures: NVN formats, images, change-detected uploads from guest memory, views and samplers
// (counterpart of the texture half of metal.m and the view helpers of renderer.m).
#include <SDL3/SDL_timer.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "vk.h"
#include "texpack.h"
#include "../astc.h"
#include "../glyphs.h"
#include "../renderer.h"

// ================================================================== formats
// expand_rgb: converted to RGBA8 on upload - 1 RGB8, 2 RGBA4 and 3 RGB5A1 (16-bit, red in the low bits: the
// Vulkan PACK16 formats put it in the high bits, and the alpha bit at the other end)
typedef struct { VkFormat vf; uint8_t expand_rgb, alpha_one; } VkFmt;

static VkFmt fmt_of(uint32_t f) {
    switch (f) {
    case 0x01: return (VkFmt){VK_FORMAT_R8_UNORM};
    case 0x02: return (VkFmt){VK_FORMAT_R8_SNORM};
    case 0x03: return (VkFmt){VK_FORMAT_R8_UINT};
    case 0x04: return (VkFmt){VK_FORMAT_R8_SINT};
    case 0x05: return (VkFmt){VK_FORMAT_R16_SFLOAT};
    case 0x06: return (VkFmt){VK_FORMAT_R16_UNORM};
    case 0x07: return (VkFmt){VK_FORMAT_R16_SNORM};
    case 0x08: return (VkFmt){VK_FORMAT_R16_UINT};
    case 0x09: return (VkFmt){VK_FORMAT_R16_SINT};
    case 0x0A: return (VkFmt){VK_FORMAT_R32_SFLOAT};
    case 0x0B: return (VkFmt){VK_FORMAT_R32_UINT};
    case 0x0C: return (VkFmt){VK_FORMAT_R32_SINT};
    case 0x0D: return (VkFmt){VK_FORMAT_R8G8_UNORM};
    case 0x0E: return (VkFmt){VK_FORMAT_R8G8_SNORM};
    case 0x0F: return (VkFmt){VK_FORMAT_R8G8_UINT};
    case 0x10: return (VkFmt){VK_FORMAT_R8G8_SINT};
    case 0x11: return (VkFmt){VK_FORMAT_R16G16_SFLOAT};
    case 0x12: return (VkFmt){VK_FORMAT_R16G16_UNORM};
    case 0x13: return (VkFmt){VK_FORMAT_R16G16_SNORM};
    case 0x14: return (VkFmt){VK_FORMAT_R16G16_UINT};
    case 0x15: return (VkFmt){VK_FORMAT_R16G16_SINT};
    case 0x16: return (VkFmt){VK_FORMAT_R32G32_SFLOAT};
    case 0x17: return (VkFmt){VK_FORMAT_R32G32_UINT};
    case 0x18: return (VkFmt){VK_FORMAT_R32G32_SINT};
    case 0x19: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM, 1, 1};  // RGB8: expanded on upload
    case 0x25: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM};
    case 0x26: return (VkFmt){VK_FORMAT_R8G8B8A8_SNORM};
    case 0x27: return (VkFmt){VK_FORMAT_R8G8B8A8_UINT};
    case 0x28: return (VkFmt){VK_FORMAT_R8G8B8A8_SINT};
    case 0x29: return (VkFmt){VK_FORMAT_R16G16B16A16_SFLOAT};
    case 0x2A: return (VkFmt){VK_FORMAT_R16G16B16A16_UNORM};
    case 0x2B: return (VkFmt){VK_FORMAT_R16G16B16A16_SNORM};
    case 0x2C: return (VkFmt){VK_FORMAT_R16G16B16A16_UINT};
    case 0x2D: return (VkFmt){VK_FORMAT_R16G16B16A16_SINT};
    case 0x2E: return (VkFmt){VK_FORMAT_R32G32B32A32_SFLOAT};
    case 0x2F: return (VkFmt){VK_FORMAT_R32G32B32A32_UINT};
    case 0x30: return (VkFmt){VK_FORMAT_R32G32B32A32_SINT};
    case 0x31: return (VkFmt){VK_FORMAT_S8_UINT};
    case 0x32: return (VkFmt){VK_FORMAT_D16_UNORM};
    case 0x33: return (VkFmt){VK_FORMAT_D32_SFLOAT};  // DEPTH24 (as on Metal: no 24-bit depth there)
    case 0x34: return (VkFmt){VK_FORMAT_D32_SFLOAT};
    case 0x35: return (VkFmt){VK_FORMAT_D32_SFLOAT_S8_UINT};
    case 0x36: return (VkFmt){VK_FORMAT_D32_SFLOAT_S8_UINT};
    case 0x37: return (VkFmt){VK_FORMAT_R8G8B8A8_SRGB, 0, 1};
    case 0x38: return (VkFmt){VK_FORMAT_R8G8B8A8_SRGB};
    case 0x39: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM, 2};      // RGBA4 (Metal ABGR4Unorm): expanded on upload
    case 0x3A: return (VkFmt){VK_FORMAT_R5G6B5_UNORM_PACK16};     // Metal B5G6R5Unorm
    case 0x3B: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM, 3};      // RGB5A1 (Metal A1BGR5Unorm): expanded on upload
    case 0x3C: return (VkFmt){VK_FORMAT_R5G6B5_UNORM_PACK16};
    case 0x3D: return (VkFmt){VK_FORMAT_A2B10G10R10_UNORM_PACK32};
    case 0x3E: return (VkFmt){VK_FORMAT_A2B10G10R10_UINT_PACK32};
    case 0x3F: return (VkFmt){VK_FORMAT_B10G11R11_UFLOAT_PACK32};
    case 0x40: return (VkFmt){VK_FORMAT_E5B9G9R9_UFLOAT_PACK32};
    case 0x41: case 0x42: return (VkFmt){VK_FORMAT_BC1_RGBA_UNORM_BLOCK};
    case 0x43: return (VkFmt){VK_FORMAT_BC2_UNORM_BLOCK};
    case 0x44: return (VkFmt){VK_FORMAT_BC3_UNORM_BLOCK};
    case 0x45: case 0x46: return (VkFmt){VK_FORMAT_BC1_RGBA_SRGB_BLOCK};
    case 0x47: return (VkFmt){VK_FORMAT_BC2_SRGB_BLOCK};
    case 0x48: return (VkFmt){VK_FORMAT_BC3_SRGB_BLOCK};
    case 0x49: return (VkFmt){VK_FORMAT_BC4_UNORM_BLOCK};
    case 0x4A: return (VkFmt){VK_FORMAT_BC4_SNORM_BLOCK};
    case 0x4B: return (VkFmt){ENV("LARECOMP_BC5_SNORM_TEST") ? VK_FORMAT_BC5_SNORM_BLOCK : VK_FORMAT_BC5_UNORM_BLOCK};
    // 0x4C = signed RGTC2; LARECOMP_BC5_UNORM=1 decodes it unsigned (see metal.m / notes/PLAN.md)
    case 0x4C: return (VkFmt){ENV("LARECOMP_BC5_UNORM") ? VK_FORMAT_BC5_UNORM_BLOCK : VK_FORMAT_BC5_SNORM_BLOCK};
    case 0x4D: return (VkFmt){VK_FORMAT_BC7_UNORM_BLOCK};
    case 0x4E: return (VkFmt){VK_FORMAT_BC7_SRGB_BLOCK};
    case 0x4F: return (VkFmt){VK_FORMAT_BC6H_SFLOAT_BLOCK};
    case 0x50: return (VkFmt){VK_FORMAT_BC6H_UFLOAT_BLOCK};
    case 0x6D: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM, 0, 1};
    case 0x71: return (VkFmt){VK_FORMAT_R16G16B16A16_SFLOAT, 0, 1};
    case 0x76: return (VkFmt){VK_FORMAT_R32G32B32A32_SFLOAT, 0, 1};
    case 0x79: return (VkFmt){VK_FORMAT_ASTC_4x4_UNORM_BLOCK};
    case 0x7A: return (VkFmt){VK_FORMAT_ASTC_5x4_UNORM_BLOCK};
    case 0x7B: return (VkFmt){VK_FORMAT_ASTC_5x5_UNORM_BLOCK};
    case 0x7C: return (VkFmt){VK_FORMAT_ASTC_6x5_UNORM_BLOCK};
    case 0x7D: return (VkFmt){VK_FORMAT_ASTC_6x6_UNORM_BLOCK};
    case 0x7E: return (VkFmt){VK_FORMAT_ASTC_8x5_UNORM_BLOCK};
    case 0x7F: return (VkFmt){VK_FORMAT_ASTC_8x6_UNORM_BLOCK};
    case 0x80: return (VkFmt){VK_FORMAT_ASTC_8x8_UNORM_BLOCK};
    case 0x81: return (VkFmt){VK_FORMAT_ASTC_10x5_UNORM_BLOCK};
    case 0x82: return (VkFmt){VK_FORMAT_ASTC_10x6_UNORM_BLOCK};
    case 0x83: return (VkFmt){VK_FORMAT_ASTC_10x8_UNORM_BLOCK};
    case 0x84: return (VkFmt){VK_FORMAT_ASTC_10x10_UNORM_BLOCK};
    case 0x85: return (VkFmt){VK_FORMAT_ASTC_12x10_UNORM_BLOCK};
    case 0x86: return (VkFmt){VK_FORMAT_ASTC_12x12_UNORM_BLOCK};
    case 0x87: return (VkFmt){VK_FORMAT_ASTC_4x4_SRGB_BLOCK};
    case 0x88: return (VkFmt){VK_FORMAT_ASTC_5x4_SRGB_BLOCK};
    case 0x89: return (VkFmt){VK_FORMAT_ASTC_5x5_SRGB_BLOCK};
    case 0x8A: return (VkFmt){VK_FORMAT_ASTC_6x5_SRGB_BLOCK};
    case 0x8B: return (VkFmt){VK_FORMAT_ASTC_6x6_SRGB_BLOCK};
    case 0x8C: return (VkFmt){VK_FORMAT_ASTC_8x5_SRGB_BLOCK};
    case 0x8D: return (VkFmt){VK_FORMAT_ASTC_8x6_SRGB_BLOCK};
    case 0x8E: return (VkFmt){VK_FORMAT_ASTC_8x8_SRGB_BLOCK};
    case 0x8F: return (VkFmt){VK_FORMAT_ASTC_10x5_SRGB_BLOCK};
    case 0x90: return (VkFmt){VK_FORMAT_ASTC_10x6_SRGB_BLOCK};
    case 0x91: return (VkFmt){VK_FORMAT_ASTC_10x8_SRGB_BLOCK};
    case 0x92: return (VkFmt){VK_FORMAT_ASTC_10x10_SRGB_BLOCK};
    case 0x93: return (VkFmt){VK_FORMAT_ASTC_12x10_SRGB_BLOCK};
    case 0x94: return (VkFmt){VK_FORMAT_ASTC_12x12_SRGB_BLOCK};
    case 0x95: case 0x96: case 0x97: return (VkFmt){VK_FORMAT_R5G5B5A1_UNORM_PACK16};
    case 0x98: return (VkFmt){VK_FORMAT_B8G8R8A8_UNORM, 0, 1};
    case 0x99: return (VkFmt){VK_FORMAT_B8G8R8A8_UNORM};
    case 0x9A: return (VkFmt){VK_FORMAT_B8G8R8A8_SRGB, 0, 1};
    case 0x9B: return (VkFmt){VK_FORMAT_B8G8R8A8_SRGB};
    default: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM};
    }
}

VkFormat vk_format(uint32_t f) { return fmt_of(f).vf; }
bool vk_format_alpha_one(uint32_t f) { return fmt_of(f).alpha_one; }
bool renderer_format_alpha_one(uint32_t f) { return fmt_of(f).alpha_one; }

static VkImageAspectFlags aspect_of(uint32_t f) {
    if (f == 0x31) return VK_IMAGE_ASPECT_STENCIL_BIT;
    if (f == 0x35 || f == 0x36) return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (renderer_format_is_depth(f)) return VK_IMAGE_ASPECT_DEPTH_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

static bool format_supports(VkFormat f, VkFormatFeatureFlags want) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    return (p.optimalTilingFeatures & want) == want;
}

// ================================================================== output resolution / per-frame clears
// Same rules as metal.m (see the comments there).
static float g_rs_x = 1, g_rs_y = 1;
void renderer_get_scale(float *sx, float *sy) { *sx = g_rs_x; *sy = g_rs_y; }
void renderer_set_scale(float sx, float sy) { g_rs_x = sx; g_rs_y = sy; }
uint64_t renderer_present_count(void);

bool g_tex_no_scale;  // (hle/za_fixes.c: Legends Z-A renders unscaled)
static bool tex_scalable(const NvnTexture *t) {
    if (ENV("LARECOMP_NO_SCALE") || g_tex_no_scale) return false;
    if (t->target != 1 || t->samples > 1 || !(t->flags & 8) || t->width < 32 || t->height < 16) return false;
    float a = (float)t->width / (float)t->height;
    return a > 1.74f && a < 1.81f;
}

bool renderer_texture_first_write(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    uint64_t total_frames = renderer_present_count();
    if (!b || !b->presented || (b->sx == b->sy) || b->touched_frame == total_frames + 1) return false;
    b->touched_frame = total_frames + 1;
    return true;
}

static uint32_t g_level_gen = 1;

bool renderer_texture_frame_fresh(NvnTexture *t) {
    static int on = -1;
    if (on < 0) on = ENV("LARECOMP_NO_FRAME_CLEAR") == NULL;
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    uint64_t total_frames = renderer_present_count();
    if (!on || !b || !b->scalable || b->frame_cleared == total_frames + 1) return false;
    b->frame_cleared = total_frames + 1;
    return true;
}

bool renderer_texture_level_fresh(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    static int keep = -1;
    if (keep < 0) keep = ENV("LARECOMP_KEEP_TARGETS") != NULL;
    if (!b || keep || b->level_gen == g_level_gen) return false;
    if (!b->scalable) { b->level_gen = g_level_gen; return false; }
    bool fresh = b->level_gen != 0;
    b->level_gen = g_level_gen;
    return fresh;
}

void renderer_texture_mark_presented(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (b) b->presented = true;
}

bool renderer_texture_presented(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    return b && b->presented;
}

void renderer_texture_scale(NvnTexture *t, float *sx, float *sy) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (b && b->scalable) { *sx = b->sx; *sy = b->sy; }
    else { *sx = 1; *sy = 1; }
}

// ================================================================== images
void vk_image_init_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .image = img,
                               .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

// Images destroyed by the guest are kept until the submissions that may use them have completed.
typedef struct { VkImage img; VmaAllocation mem; VkImageView view; VkPipeline pipe; uint64_t seq; } Dead;
static Dead *dead;
static int ndead, cap_dead;
static void views_forget(TexBackend *b);

void vk_collect_dead(void) {
    uint64_t done = vk_completed_seq();
    int k = 0;
    for (int i = 0; i < ndead; i++) {
        if (dead[i].seq > done) { dead[k++] = dead[i]; continue; }
        if (dead[i].view) vkDestroyImageView(g_vk.dev, dead[i].view, NULL);
        else if (dead[i].pipe) vkDestroyPipeline(g_vk.dev, dead[i].pipe, NULL);
        else vmaDestroyImage(g_vk.vma, dead[i].img, dead[i].mem);
    }
    ndead = k;
}

static void create_image(NvnTexture *t, TexBackend *b) {
    VkFmt mf = fmt_of(t->format);
    b->sx = b->scalable ? g_rs_x : 1;
    b->sy = b->scalable ? g_rs_y : 1;
    uint32_t tw = t->width ? t->width : 1, th = t->height ? t->height : 1;
    if (b->scalable) {
        tw = (uint32_t)lroundf(tw * b->sx); th = (uint32_t)lroundf(th * b->sy);
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
    }
    VkImageType type = VK_IMAGE_TYPE_2D;
    uint32_t layers = 1, depth = 1;
    switch (t->target) {
    case 0: type = VK_IMAGE_TYPE_1D; th = 1; break;
    case 2: type = VK_IMAGE_TYPE_3D; depth = t->depth ? t->depth : 1; break;
    case 3: type = VK_IMAGE_TYPE_1D; layers = t->height ? t->height : 1; th = 1; break;
    case 4: case 6: layers = t->depth ? t->depth : 1; break;
    case 8: layers = 6; break;
    case 9: layers = t->depth ? t->depth : 6; break;
    default: break;
    }
    uint32_t levels = t->levels ? t->levels : 1;
    {   // never more levels than the size allows (scaled targets, odd guest values)
        uint32_t m = 1, sz = tw > th ? tw : th;
        if (depth > sz) sz = depth;
        while (sz > 1) { sz >>= 1; m++; }
        if (levels > m) levels = m;
    }
    uint32_t samples = t->samples > 1 ? t->samples : 1;
    if (samples > 1) levels = 1;
    VkFormat vf = mf.vf;
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    b->astc_bw = b->astc_bh = 0;
    if (!format_supports(vf, need)) {
        bool astc = vf >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && vf <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
        const FormatInfo *fi = t->layout.fmt;
        if (astc && fi && fi->bpb == 16) {  // desktop GPUs: decoded on the CPU into an RGBA8 image of the same colour space
            b->astc_bw = fi->bw;
            b->astc_bh = fi->bh;
            b->astc_srgb = t->format >= 0x87;
            vf = b->astc_srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        } else {
            static int nlog;
            if (nlog++ < 20) LOG("[vk] format %d (nvn %#x) not supported: RGBA8 instead\n", vf, t->format);
            vf = VK_FORMAT_R8G8B8A8_UNORM;
        }
    }
    bool depthfmt = renderer_format_is_depth(t->format);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (depthfmt) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    else if (format_supports(vf, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    // NVN_TEXTURE_FLAGS_IMAGE: shaders write it as an image (SUST / SURED: Z-A's exposure and luminance passes)
    b->storage = !depthfmt && (t->flags & 4) && format_supports(vf, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
    if (b->storage) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    VkImageCreateFlags flags = 0;
    if (!depthfmt) flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;  // format views (NVN texture views, raw sRGB views)
    if (type == VK_IMAGE_TYPE_2D && layers >= 6 && tw == th && samples == 1) flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    // 3D render targets (Z-A's tonemapping LUT, one slice per instance): rendered through a 2D-array view of the slices
    if (type == VK_IMAGE_TYPE_3D && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = flags, .imageType = type, .format = vf,
                            .extent = {tw, th, depth}, .mipLevels = levels, .arrayLayers = layers,
                            .samples = (VkSampleCountFlagBits)samples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkResult r = vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL);
    if (r != VK_SUCCESS) {
        LOG("[vk] image creation failed (%d): nvn fmt %#x %ux%ux%u target %u\n", r, t->format, t->width, t->height, t->depth, t->target);
        ci.format = vf = VK_FORMAT_R8G8B8A8_UNORM;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.flags &= ~VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL));
        depthfmt = false;
    }
    if (ci.usage != usage) b->storage = false;  // (the fallback image above)
    b->usage = ci.usage;
    b->format = vf;
    b->width = tw; b->height = th; b->depth = depth; b->layers = layers; b->levels = levels; b->samples = samples;
    b->aspect = depthfmt ? aspect_of(t->format) : VK_IMAGE_ASPECT_COLOR_BIT;
    b->base_type = type == VK_IMAGE_TYPE_1D ? (layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D)
                 : type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                 : t->target == 8 ? VK_IMAGE_VIEW_TYPE_CUBE : t->target == 9 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
                 : layers > 1 || t->target == 4 || t->target == 6 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vk_image_init_layout(vk_upload_cmd(), b->image, b->aspect);
    if (depthfmt) {
        // New depth / stencil images start zeroed (as fresh memory reads on Apple GPUs). The game builds its depth
        // pyramid with depth-only copies, so the stencil of those images was never written: whatever the memory
        // held, different per image and per run, then failed or passed the depth-of-field passes' stencil test in
        // patches (flicker that came and went between runs, every other frame, below 1x internal resolution).
        VkCommandBuffer cb = vk_upload_cmd();
        VkClearDepthStencilValue zero = {0.0f, 0};
        VkImageSubresourceRange rg = {b->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearDepthStencilImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(cb, &di);
    }
}

static void defer(Dead d) {
    if (ndead == cap_dead) { cap_dead = cap_dead ? cap_dead * 2 : 256; dead = realloc(dead, cap_dead * sizeof *dead); }
    d.seq = vk_pending_seq();
    dead[ndead++] = d;
}

void vk_defer_image(VkImage img, VmaAllocation mem) { defer((Dead){.img = img, .mem = mem}); }
void vk_defer_view(VkImageView v) { defer((Dead){.view = v}); }
void vk_defer_pipeline(VkPipeline p) { defer((Dead){.pipe = p}); }  // (render thread: exec.c's optimized pipelines replace the fast-linked)

void vk_dl_note_upload(void *img, uint32_t w, uint32_t h, uint32_t lv);  // (exec.c: LARECOMP_VK_DEVLOST_TRACE)

static uint32_t level_w(const TexBackend *b, uint32_t lv) { uint32_t v = b->width >> lv; return v ? v : 1; }
static uint32_t level_h(const TexBackend *b, uint32_t lv) { uint32_t v = b->height >> lv; return v ? v : 1; }

static void destroy_image(TexBackend *b) {
    views_forget(b);
    if (b->image || b->mem) defer((Dead){.img = b->image, .mem = b->mem});  // (never the same allocation twice)
    b->image = VK_NULL_HANDLE;
    b->mem = VK_NULL_HANDLE;
}

// Live game textures: an in-place reboot (vk_textures_reset) destroys them all.
static NvnTexture **g_live;
static int g_nlive, g_cap_live;
static void live_add(NvnTexture *t, TexBackend *b) {
    if (g_nlive == g_cap_live) { g_cap_live = g_cap_live ? g_cap_live * 2 : 1024; g_live = realloc(g_live, g_cap_live * sizeof *g_live); }
    g_live[g_nlive++] = t;
    b->reg = g_nlive;
}
static void live_remove(TexBackend *b) {
    if (!b->reg) return;
    NvnTexture *last = g_live[--g_nlive];
    g_live[b->reg - 1] = last;
    if (last->backend) ((TexBackend *)last->backend)->reg = b->reg;
    b->reg = 0;
}

uint64_t prof_tb_new, prof_tb_dead_ns, prof_tb_create_ns;  // (new texture backends: dead-texture collection, image creation)
static TexBackend *tex_backend(NvnTexture *t) {
    TexBackend *b = t->backend;
    if (b) {
        if (b->scalable && (b->sx != g_rs_x || b->sy != g_rs_y)) {  // output resolution changed: reallocate
            destroy_image(b);
            create_image(t, b);
            if (t->width >= 900 && getenv("LARECOMP_LOG_RT"))
                LOG("[rt-tex] rescaled %ux%u fmt %#x -> %ux%u\n", t->width, t->height, t->format, b->width, b->height);
        }
        return b;
    }
    uint64_t t0 = SDL_GetTicksNS();
    vk_collect_dead();
    uint64_t t1 = SDL_GetTicksNS();
    b = calloc(1, sizeof *b);
    b->scalable = tex_scalable(t);
    create_image(t, b);
    if (t->width >= 900 && getenv("LARECOMP_LOG_RT"))  // (research: which big textures follow the render scale)
        LOG("[rt-tex] %ux%u fmt %#x target %u flags %#x -> %ux%u%s\n", t->width, t->height, t->format, t->target, t->flags,
            b->width, b->height, b->scalable ? " scaled" : "");
    t->backend = b;
    live_add(t, b);
    prof_tb_new++, prof_tb_dead_ns += t1 - t0, prof_tb_create_ns += SDL_GetTicksNS() - t1;  // (exec.c [spike])
    return b;
}

// A texture the game fills from memory (CopyBufferToTexture) holds full-size data, so it can't live at the scaled
// output resolution: Let's Go's movie planes (R8 1920x1080 / RG8 960x540, 16:9 render-target flags) were scaled to
// the window and the full-size copies overran them (device lost). Such a texture stays at its own size from then on.
void vk_tex_unscale(NvnTexture *t) {
    TexBackend *b = tex_backend(t);
    if (!b->scalable) return;
    b->scalable = false;
    if (b->sx != 1 || b->sy != 1) {
        LOG("[vk] texture %ux%u fmt %#x at %llx flags %#x stride %u filled from memory: kept at full size\n", t->width,
            t->height, t->format, (unsigned long long)t->addr, t->flags, (unsigned)t->stride);
        destroy_image(b);
        create_image(t, b);
    }
}

void backend_texture_destroyed(NvnTexture *t) {
    TexBackend *b = t->backend;
    if (b) {
        live_remove(b);
        destroy_image(b);
        free(b->pending_rgba);
        free(b);
    }
    t->backend = NULL;
}

// Reboot (device.c backend_reset; GPU idle): every game texture and its views go.
void vk_textures_reset(void) {
    int n = g_nlive;
    while (g_nlive > 0) backend_texture_destroyed(g_live[g_nlive - 1]);
    vk_collect_dead();
    LOG("[vk] reboot: %d textures released\n", n);
}

// ================================================================== uploads (same change detection as metal.m)
static uint64_t full_hash_budget, full_hash_budget_frame;

// Four independent lanes (one serial multiply chain ran at ~1.5 bytes/cycle; dynamic textures are hashed every
// frame). Values are only compared within a run.
uint64_t prof_up_sample_ns, prof_up_hash_ns, prof_up_swz_ns, prof_up_astc_ns, prof_up_new, prof_up_trans_ns, prof_up_copy_ns, prof_up_cmd_ns, prof_up_glyph_ns, prof_up_expand_ns;  // (LARECOMP_VK_PROF: upload_texture by part)
static uint64_t hash_full(const uint8_t *p, uint64_t n) {
    uint64_t h0 = 0x9E3779B97F4A7C15ull ^ n, h1 = 0xC2B2AE3D27D4EB4Full, h2 = 0x165667B19E3779F9ull, h3 = 0x27D4EB2F165667C5ull;
    uint64_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t v[4];
        memcpy(v, p + i, 32);
        h0 = (h0 ^ v[0]) * 0x100000001B3ull; h0 ^= h0 >> 29;
        h1 = (h1 ^ v[1]) * 0x100000001B3ull; h1 ^= h1 >> 29;
        h2 = (h2 ^ v[2]) * 0x100000001B3ull; h2 ^= h2 >> 29;
        h3 = (h3 ^ v[3]) * 0x100000001B3ull; h3 ^= h3 >> 29;
    }
    uint64_t h = h0 ^ (h1 * 0x9E3779B97F4A7C15ull) ^ (h2 * 0xC2B2AE3D27D4EB4Full) ^ (h3 * 0x165667B19E3779F9ull);
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h ^ (h >> 31);
}

uint64_t vk_tex_content_hash(const uint8_t *p, uint64_t n) { return hash_full(p, n); }  // (research: hle/bntxscan.c)

// The quick check of a texture the CPU may have changed: 128 chunks of 32 bytes spread over it (each costs one cache
// miss: 4096 single bytes made this a sixth of the render thread; the full hash every 120 frames is the backstop)
static uint64_t fnv_sample(const uint8_t *p, uint64_t n) {
    if (n <= 128 * 32) return hash_full(p, n);
    uint64_t h = 1469598103934665603ull ^ n, step = (n - 32) / 127;
    for (uint64_t i = 0; i < 128; i++) {
        uint64_t v[4];
        memcpy(v, p + i * step, 32);
        h = (h ^ v[0] ^ (v[1] << 1) ^ (v[2] << 2) ^ (v[3] << 3)) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    return h;
}

static pthread_mutex_t pending_lock = PTHREAD_MUTEX_INITIALIZER;

static void upload_pending(TexBackend *b) {
    pthread_mutex_lock(&pending_lock);
    void *px = b->pending_rgba;
    uint32_t w = b->pending_w, h = b->pending_h, stride = b->pending_stride;
    b->pending_rgba = NULL;
    pthread_mutex_unlock(&pending_lock);
    if (!px) return;
    if (w > b->width) w = b->width;
    if (h > b->height) h = b->height;
    VkSpan s = vk_transient_copy(px, (uint64_t)stride * h, 16);
    free(px);
    VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = stride / 4,
                            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    VkCommandBuffer cb = vk_upload_cmd();
    vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
}

uint64_t prof_tex_checks, prof_tex_samples, prof_tex_full, prof_tex_full_bytes, prof_tex_dyn;
// Texture packs (texpack.c): the texture's image becomes the pack's - a .dds's BC7 / BC5 / BC4 levels as they are, or a
// PNG's RGBA8 (sRGB when the game's texture was) with its mips blitted down the chain. False: nothing changed.
// Room for a replacement of `need` bytes: the GPU's memory (all heaps - an APU's images land in whichever has room) used
// with it stays a reserve (15%, at least 768 MB) below the driver's budget (VK_EXT_memory_budget; VMA estimates without).
// The PS5 under Linux has 512 MB VRAM + 7 GB GTT and Z-A's guest heap takes 6 GB of it: its 6.5 GB AI pack filled the
// rest, the compositor's buffers faulted and the GPU hung (the user's, 2026-11-09). Over the budget the game's own
// texture stays; LARECOMP_TEXPACK_NOBUDGET=1 ignores it.
// On Linux with one amdgpu card the kernel's own counters are used (sysfs mem_info_{vram,gtt}_{used,total}): RADV's
// budget there said 12.3 GB used of a 2.5 GB host heap on the PS5, and GTT is what runs out (the dma-buf guest heap,
// nvn/vulkan/gpuheap.c, lives in it).
#ifdef __linux__
static bool amdgpu_free(uint64_t *free_b, uint64_t *total_b) {
    static int card = -2;  // -1: none (or several)
    char p[96];
    if (card == -2) {
        card = -1;
        for (int i = 0, found = 0; i < 16; i++) {
            snprintf(p, sizeof p, "/sys/class/drm/card%d/device/mem_info_gtt_total", i);
            struct stat st;
            if (!stat(p, &st)) { if (found++) { card = -1; break; } card = i; }
        }
    }
    if (card < 0) return false;
    static const char *k[4] = {"vram_total", "vram_used", "gtt_total", "gtt_used"};
    uint64_t v[4];
    for (int i = 0; i < 4; i++) {
        snprintf(p, sizeof p, "/sys/class/drm/card%d/device/mem_info_%s", card, k[i]);
        FILE *f = fopen(p, "r");
        unsigned long long x = 0;
        int ok = f && fscanf(f, "%llu", &x) == 1;
        if (f) fclose(f);
        if (!ok) return false;
        v[i] = x;
    }
    *total_b = v[0] + v[2];
    *free_b = (v[0] > v[1] ? v[0] - v[1] : 0) + (v[2] > v[3] ? v[2] - v[3] : 0);
    return true;
}
#endif
static bool texpack_room(uint64_t need) {
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_TEXPACK_NOBUDGET") != NULL;
    if (off) return true;
#ifdef __linux__
    {
        static uint64_t free_b, total_b, at, left;  // (sysfs read every 100 ms; what was taken since, counted here)
        static int have = -1;
        uint64_t now = SDL_GetTicks();
        if (have < 0 || now - at >= 100) have = amdgpu_free(&free_b, &total_b), at = now, left = free_b;
        if (have) {
            uint64_t reserve = total_b * 10 / 100 > (768ull << 20) ? total_b * 10 / 100 : (768ull << 20);
            extern uint64_t gpuheap_unbacked(void);  // (and room for the guest heap's chunks still to come, up to 1 GB)
            uint64_t heap = gpuheap_unbacked();
            reserve += heap < (1024ull << 20) ? heap : (1024ull << 20);
            static int shown;
            if (!shown++) LOG("[texpack] GPU memory (amdgpu): %llu of %llu MB free\n", (unsigned long long)(free_b >> 20),
                              (unsigned long long)(total_b >> 20));
            bool ok = need + reserve <= left;
            if (ok) left -= need;
            static int warned;
            if (!ok && !warned++)
                LOG("[texpack] GPU memory budget reached (%llu MB free of %llu): the game's textures stay from here\n",
                    (unsigned long long)(left >> 20), (unsigned long long)(total_b >> 20));
            return ok;
        }
    }
#endif
    VmaBudget bud[VK_MAX_MEMORY_HEAPS];
    vmaGetHeapBudgets(g_vk.vma, bud);
    const VkPhysicalDeviceMemoryProperties *mp;
    vmaGetMemoryProperties(g_vk.vma, &mp);
    uint64_t use = 0, budget = 0;
    for (uint32_t i = 0; i < mp->memoryHeapCount; i++) use += bud[i].usage, budget += bud[i].budget;
    static int shown;
    if (!shown++) {
        LOG("[texpack] GPU memory (VK_EXT_memory_budget %s):", g_vk.mem_budget ? "on" : "off");
        for (uint32_t i = 0; i < mp->memoryHeapCount; i++)
            LOG(" heap %u %s size %llu MB, used %llu MB (VMA's %llu), budget %llu MB;", i,
                mp->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? "device" : "host",
                (unsigned long long)(mp->memoryHeaps[i].size >> 20), (unsigned long long)(bud[i].usage >> 20),
                (unsigned long long)(bud[i].statistics.allocationBytes >> 20), (unsigned long long)(bud[i].budget >> 20));
        LOG("\n");
    }
    uint64_t reserve = budget * 15 / 100 > (768ull << 20) ? budget * 15 / 100 : (768ull << 20);
    bool ok = use + need + reserve <= budget;
    static int warned;
    if (!ok && !warned++)
        LOG("[texpack] GPU memory budget reached (%llu of %llu MB used): the game's textures stay from here\n",
            (unsigned long long)(use >> 20), (unsigned long long)(budget >> 20));
    return ok;
}

static bool texpack_apply(TexBackend *b, const TexLoaded *r) {
    VkFormat old = b->format, vf = r->fmt;
    if (r->rgba) {
        bool srgb = old == VK_FORMAT_BC7_SRGB_BLOCK || old == VK_FORMAT_BC1_RGBA_SRGB_BLOCK || old == VK_FORMAT_BC1_RGB_SRGB_BLOCK ||
                    old == VK_FORMAT_BC2_SRGB_BLOCK || old == VK_FORMAT_BC3_SRGB_BLOCK || old == VK_FORMAT_R8G8B8A8_SRGB ||
                    old == VK_FORMAT_B8G8R8A8_SRGB;
        vf = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    }
    uint64_t need = 0;
    for (uint32_t lv = 0; lv < r->levels && lv < TEXPACK_MAX_LEVELS; lv++) need += r->level_size[lv];
    if (r->rgba) need = need * 4 / 3;  // (a PNG's: its levels made here)
    if (!texpack_room(need)) return false;
    uint32_t w = r->w, h = r->h, levels = r->levels;
    if (r->rgba) for (uint32_t s = w > h ? w : h; s > 1; s >>= 1) levels++;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                            .imageType = VK_IMAGE_TYPE_2D, .format = vf, .extent = {w, h, 1}, .mipLevels = levels, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage img;
    VmaAllocation mem;
    if (!format_supports(vf, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT) ||
        vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL) != VK_SUCCESS)
        return false;
    destroy_image(b);
    b->image = img, b->mem = mem;
    b->format = vf, b->usage = ci.usage, b->storage = false;
    b->width = w, b->height = h, b->depth = 1, b->layers = 1, b->levels = levels, b->samples = 1;
    b->aspect = VK_IMAGE_ASPECT_COLOR_BIT, b->base_type = VK_IMAGE_VIEW_TYPE_2D;
    b->astc_bw = b->astc_bh = 0;
    b->texpack = true;
    VkCommandBuffer cb = vk_upload_cmd();
    vk_image_init_layout(cb, img, VK_IMAGE_ASPECT_COLOR_BIT);
    for (uint32_t lv = 0; lv < r->levels; lv++) {
        VkSpan s = vk_transient_copy(r->data + r->level_off[lv], r->level_size[lv], 16);
        VkBufferImageCopy rg = {.bufferOffset = s.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, 0, 1},
                                .imageExtent = {level_w(b, lv), level_h(b, lv), 1}};
        vkCmdCopyBufferToImage(cb, s.buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    }
    for (uint32_t lv = r->levels; lv < levels; lv++) {  // (a PNG's: each level from the one above, once that one is written)
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                               .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                               .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(cb, &di);
        int32_t sw = (int32_t)level_w(b, lv - 1), sh = (int32_t)level_h(b, lv - 1), dw = (int32_t)level_w(b, lv), dh = (int32_t)level_h(b, lv);
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv - 1, 0, 1}, .srcOffsets = {{0, 0, 0}, {sw, sh, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, 0, 1}, .dstOffsets = {{0, 0, 0}, {dw, dh, 1}}};
        vkCmdBlitImage(cb, img, VK_IMAGE_LAYOUT_GENERAL, img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
    }
    vk_full_barrier(cb);
    return true;
}

static void upload_texture(NvnTexture *t, TexBackend *b) {
    // (its replacement read by now? the game's own until then.) At most LARECOMP_TEXPACK_BUDGET_MB (8) of replacements
    // go in a frame, at least one: each is a new image and all its levels copied on this thread - many done at once
    // (after a load, or running into a new place) held single frames for 300+ ms with a 2x pack
    static uint64_t tp_frame, tp_bytes, tp_budget;
    if (!tp_budget) tp_budget = (getenv("LARECOMP_TEXPACK_BUDGET_MB") ? strtoull(getenv("LARECOMP_TEXPACK_BUDGET_MB"), NULL, 10) : 8) << 20;
    if (b->texpack_wait) {
        uint64_t fr = vk_frame_counter();
        if (fr != tp_frame) tp_frame = fr, tp_bytes = 0;
        TexLoaded *r;
        if (tp_bytes < tp_budget && texpack_take(b->texpack_key, &r)) {
            b->texpack_wait = false;
            if (r) for (uint32_t lv = 0; lv < r->levels && lv < TEXPACK_MAX_LEVELS; lv++) tp_bytes += r->level_size[lv];
            bool ok = r && texpack_apply(b, r);
            texpack_free(r);
            if (ok) return;
        }
    }
    if (!t->addr || t->target == 10 || b->texpack) return;
    uint64_t frame = vk_frame_counter();
    if (b->glyph && b->glyph_gen != glyphs_generation()) b->uploaded = false;  // (another controller's prompts)
    if (b->uploaded && b->checked_frame == frame) return;
    if (b->uploaded && !b->dynamic && ((frame + ((uintptr_t)b >> 6)) & 7)) return;
    const FormatInfo *fi = t->layout.fmt;
    if (!fi || !fi->bpb) return;
    VkFmt mf = fmt_of(t->format);
    uint64_t total = tegra_total_size(&t->layout);
    b->checked_frame = frame;
    prof_tex_checks++;
    if (t->flags & 0x10) b->dynamic = true;
    if (full_hash_budget_frame != frame) { full_hash_budget_frame = frame; full_hash_budget = 1 << 20; }
    const uint8_t *mem = (const uint8_t *)(uintptr_t)t->addr;
    static int always_full = -1;
    if (always_full < 0) always_full = ENV("LARECOMP_TEX_FULLHASH") ? 1 : 0;
    bool full = b->dynamic || !b->uploaded || always_full;
    // (the backstop: 1 MB a frame, or one texture up to 4 MB - reading more evicted the game's data from the shared L3)
    if (!full && frame - b->full_frame >= 120 && (full_hash_budget >= total || (full_hash_budget == 1 << 20 && total <= 4 << 20))) {
        full = true;
        full_hash_budget -= total < full_hash_budget ? total : full_hash_budget;
    }
    extern uint64_t prof_up_ns;
    bool pr = prof_up_ns != ~0ull;
    uint64_t pt = pr ? SDL_GetTicksNS() : 0;
    if (!full) {
        prof_tex_samples++;
        uint64_t sh = fnv_sample(mem, total);
        if (pr) prof_up_sample_ns += SDL_GetTicksNS() - pt;
        if (sh == b->sample_hash) return;
        if (pr) pt = SDL_GetTicksNS();
    }
    prof_tex_full++; prof_tex_full_bytes += total; if (b->dynamic) prof_tex_dyn++;
    if (!b->uploaded) prof_up_new++;
    uint64_t h = hash_full(mem, total);
    b->full_frame = frame;
    b->sample_hash = fnv_sample(mem, total);
    if (pr) prof_up_hash_ns += SDL_GetTicksNS() - pt;
    if (b->uploaded && h == b->data_hash) return;
    if (b->uploaded && h != b->data_hash) b->dynamic = true;
    b->data_hash = h;
    b->uploaded = true;
    if (b->format != mf.vf && !b->astc_bw) return;  // unsupported format: nothing sensible to copy
    uint64_t ct = pr ? SDL_GetTicksNS() : 0;
    VkCommandBuffer cb = vk_upload_cmd();
    if (pr) prof_up_cmd_ns += SDL_GetTicksNS() - ct;
    uint32_t layers = t->layout.layers ? t->layout.layers : 1;
    if (layers > b->layers && b->depth == 1) layers = b->layers;
    uint64_t layer_size = tegra_layer_size(&t->layout);
    bool linear = (t->flags & 0x10) != 0;
    uint32_t levels = t->layout.levels < b->levels ? t->layout.levels : b->levels;
    for (uint32_t layer = 0; layer < layers; layer++) {
        for (uint32_t lv = 0; lv < levels; lv++) {
            LevelLayout l = tegra_level(&t->layout, lv);
            if (b->astc_bw) {  // blocks -> linear block rows -> RGBA8 texels
                const uint8_t *src = (const uint8_t *)(uintptr_t)t->addr + layer * layer_size + l.offset;
                uint8_t *blocks = malloc((uint64_t)l.wb * l.hb * 16);
                if (linear) for (uint32_t y = 0; y < l.hb; y++) memcpy(blocks + (uint64_t)y * l.wb * 16, src + (uint64_t)y * t->stride, l.wb * 16);
                else tegra_deswizzle(src, blocks, l.wb, l.hb, 1, 16, l.block_height_log2, l.block_depth_log2);
                VkSpan s = vk_transient((uint64_t)l.w * l.h * 4, 16);
                uint64_t at = pr ? SDL_GetTicksNS() : 0;
                astc_decode_ldr(blocks, l.w, l.h, b->astc_bw, b->astc_bh, b->astc_srgb, s.ptr);
                if (pr) prof_up_astc_ns += SDL_GetTicksNS() - at;
                free(blocks);
                VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = l.w, .bufferImageHeight = l.h,
                                        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, layer, 1},
                                        .imageExtent = {l.w < level_w(b, lv) ? l.w : level_w(b, lv), l.h < level_h(b, lv) ? l.h : level_h(b, lv), 1}};
                vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
                continue;
            }
            uint32_t row = l.wb * fi->bpb;
            uint64_t sz = (uint64_t)row * l.hb * l.d;
            bool expand = (mf.expand_rgb == 1 && fi->bpb == 3) || (mf.expand_rgb >= 2 && fi->bpb == 2);
            uint32_t out_row = expand ? l.wb * 4 : row;
            uint64_t tt = pr ? SDL_GetTicksNS() : 0;
            VkSpan s = vk_transient((uint64_t)out_row * l.hb * l.d, 16);
            if (pr) prof_up_trans_ns += SDL_GetTicksNS() - tt;
            // (a possible button icon is put together in normal memory: the staging buffer is write-combined, and
            // reading it back to hash the icon - glyphs_patch below - cost ~0.1-0.3 ms per small texture: up to 3.7 ms
            // of a frame where the game brought in a batch of small BC4 / BC5 textures)
            bool glyph_cand = lv == 0 && layer == 0 && l.d == 1 && l.w <= 128 && l.h <= 128 && (t->format == 0x49 || t->format == 0x4B);
            // (and a texture a pack may replace: its key is a hash of these blocks - read back from the write-combined
            // staging buffer, every new texture's top level while a pack was on, a load held one frame for 6-20 s)
            bool tp_cand = lv == 0 && layer == 0 && !b->texpack_seen && !expand && !linear && !b->dynamic && !b->scalable && l.d == 1 &&
                           layers == 1 && t->target == 1 && b->samples == 1 && !(t->flags & 0x1b) && texpack_active();
            uint8_t *lin = expand || glyph_cand || tp_cand ? malloc(sz) : s.ptr;  // (built in normal memory, copied after)
            const uint8_t *src = (const uint8_t *)(uintptr_t)t->addr + layer * layer_size + l.offset;
            if (linear) {
                for (uint32_t y = 0; y < l.hb; y++) memcpy(lin + (uint64_t)y * row, src + (uint64_t)y * t->stride, row);
            } else {
                uint64_t st = pr ? SDL_GetTicksNS() : 0;
                tegra_deswizzle(src, lin, l.wb, l.hb, l.d, fi->bpb, l.block_height_log2, l.block_depth_log2);
                if (pr) prof_up_swz_ns += SDL_GetTicksNS() - st;
            }
            // a texture pack's replacement (texpack.c): plain 2D textures from the game's files, on their first upload
            if (lv == 0 && layer == 0 && !b->texpack_seen && getenv("LARECOMP_TEXPACK_WHY")) {
                static int nwhy;
                if (nwhy++ < 60)
                    LOG("[texpack] why %ux%u fmt %#x: expand %d linear %d dynamic %d scalable %d d %u layers %u target %u samples %u flags %#x\n",
                        l.w, l.h, t->format, expand, linear, b->dynamic, b->scalable, l.d, layers, t->target, b->samples, t->flags);
            }
            if (lv == 0 && layer == 0 && !b->texpack_seen && !expand && !linear && !b->dynamic && !b->scalable && l.d == 1 &&
                layers == 1 && t->target == 1 && b->samples == 1 && !(t->flags & 0x1b) && texpack_active()) {
                // (flags: display 1, video 2, compressible 8 - render targets -, linear 0x10 are never the game's files; the
                // image flag 4 is on Arceus's material textures too)
                b->texpack_seen = true;
                uint64_t key = hash_full(lin, (uint64_t)row * l.hb) | 1;
                static int klog = -1;  // (LARECOMP_TEXPACK_KEYS=<n>: the first n keys looked up, to match against an export)
                if (klog < 0) klog = getenv("LARECOMP_TEXPACK_KEYS") ? atoi(getenv("LARECOMP_TEXPACK_KEYS")) : 0;
                if (klog > 0) klog--, LOG("[texpack] key %016llx %ux%u fmt %#x\n", (unsigned long long)key, l.w, l.h, t->format);
                if (texpack_has(key)) texpack_request(key), b->texpack_key = key, b->texpack_wait = true;
            }
            static int dump_fmt = -2;  // LARECOMP_DUMP_TEX_FMT=<nvn format>: level 0 of those textures, untiled -> build/tex/
            if (dump_fmt == -2) dump_fmt = ENV("LARECOMP_DUMP_TEX_FMT") ? (int)strtol(ENV("LARECOMP_DUMP_TEX_FMT"), NULL, 0) : -1;
            if ((int)t->format == dump_fmt && lv == 0 && layer == 0) {
                char path[160];
                mkdir("build/tex", 0755);
                snprintf(path, sizeof path, "build/tex/%llx_%ux%u_f%x_row%u.bin", (unsigned long long)t->addr, l.w, l.h, t->format, row);
                FILE *f = fopen(path, "wb");
                if (f) { fwrite(lin, 1, (size_t)row * l.hb, f); fclose(f); }
            }
            // LARECOMP_DUMP_TEX_ALL=<max size>: level 0 of every texture up to that width and height, once per content
            // -> build/tex/all/<hash>_<w>x<h>_f<format>_row<bytes>.bin (deswizzled blocks; <hash> as runtime/nvn/glyphs.c keys them)
            static int dump_all = -1;
            if (dump_all < 0) dump_all = ENV("LARECOMP_DUMP_TEX_ALL") ? atoi(ENV("LARECOMP_DUMP_TEX_ALL")) : 0;
            if (dump_all && lv == 0 && layer == 0 && l.w <= (uint32_t)dump_all && l.h <= (uint32_t)dump_all && l.d == 1) {
                static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
                static uint64_t seen[65536];
                static int nseen;
                uint64_t hsh = hash_full(lin, (uint64_t)row * l.hb) | 1;
                pthread_mutex_lock(&mu);
                uint32_t k = (uint32_t)hsh & 65535;
                while (seen[k] && seen[k] != hsh) k = (k + 1) & 65535;
                bool fresh = !seen[k] && nseen < 60000;
                if (fresh) seen[k] = hsh, nseen++;
                pthread_mutex_unlock(&mu);
                if (fresh) {
                    char path[200];
                    mkdir("build/tex", 0755);
                    mkdir("build/tex/all", 0755);
                    snprintf(path, sizeof path, "build/tex/all/%016llx_%ux%u_f%x_row%u.bin", (unsigned long long)hsh, l.w, l.h,
                             t->format, row);
                    FILE *f = fopen(path, "wb");
                    if (f) { fwrite(lin, 1, (size_t)row * l.hb, f); fclose(f); }
                }
            }
            // a button icon: its symbol for the controller in use (BC4 / BC5: lin is the upload buffer itself)
            if (glyph_cand) {
                uint64_t gt = pr ? SDL_GetTicksNS() : 0;
                int g = glyphs_patch(hash_full(lin, (uint64_t)row * l.hb) | 1, t->format, l.w, l.h, lin);
                if (g) b->glyph = true, b->glyph_gen = glyphs_generation();
                if (pr) prof_up_glyph_ns += SDL_GetTicksNS() - gt;
            }
            uint64_t et = pr && expand ? SDL_GetTicksNS() : 0;
            if (expand) {  // RGB8 / RGBA4 / RGB5A1 -> RGBA8
                uint8_t *o = s.ptr;
                uint64_t n = (uint64_t)l.wb * l.hb * l.d;
                if (mf.expand_rgb == 1)
                    for (uint64_t i = 0; i < n; i++) { o[i * 4] = lin[i * 3]; o[i * 4 + 1] = lin[i * 3 + 1]; o[i * 4 + 2] = lin[i * 3 + 2]; o[i * 4 + 3] = 255; }
                else
                    for (uint64_t i = 0; i < n; i++) {
                        uint32_t v = lin[i * 2] | (uint32_t)lin[i * 2 + 1] << 8;
                        if (mf.expand_rgb == 2) {  // R 0-3, G 4-7, B 8-11, A 12-15
                            for (int c = 0; c < 4; c++) o[i * 4 + c] = (uint8_t)(((v >> (4 * c)) & 15) * 17);
                        } else {                   // R 0-4, G 5-9, B 10-14, A 15 (the Pokémon icons in Let's Go)
                            for (int c = 0; c < 3; c++) { uint32_t x = (v >> (5 * c)) & 31; o[i * 4 + c] = (uint8_t)(x << 3 | x >> 2); }
                            o[i * 4 + 3] = v >> 15 ? 255 : 0;
                        }
                    }
                free(lin);
            }
            if (et) prof_up_expand_ns += SDL_GetTicksNS() - et;
            if ((glyph_cand || tp_cand) && !expand) memcpy(s.ptr, lin, sz), free(lin);
            uint32_t w = l.w, hh = l.h, d = l.d;
            // the copy stays inside the image's level: a texture allocated at the internal resolution (16:9, render
            // target sized) can still be filled from the CPU - the save slots' screenshots on the title screen. Its
            // full-size data overran the scaled image (only the width was clamped): GPU writes past the allocation,
            // a device loss below 1x after a seed switch.
            if (w > level_w(b, lv)) w = level_w(b, lv);
            if (hh > level_h(b, lv)) hh = level_h(b, lv);
            VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = l.wb * fi->bw, .bufferImageHeight = l.hb * fi->bh,
                                    .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect, lv,
                                                         b->depth > 1 ? 0 : layer, 1},
                                    .imageExtent = {w, b->base_type == VK_IMAGE_VIEW_TYPE_1D || b->base_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? 1 : hh,
                                                    b->depth > 1 ? d : 1}};
            if (b->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) continue;  // packed D/S: no CPU layout
            uint64_t kt = pr ? SDL_GetTicksNS() : 0;
            vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
            if (pr) prof_up_copy_ns += SDL_GetTicksNS() - kt;
        }
    }
}

// Host writes into guest memory that textures may sit on (Let's Go's movie decoder fills NV12 buffers the game
// wraps in R8 / RG8 textures): any thread records the range; a texture over it is CPU-owned again at its next use,
// so upload_texture's hash check picks the new data up (a GPU clear had made it GPU-owned: a green movie).
#define DIRTY_RING 4096  // (with the render thread a list can run a few frames after the writes made before it were stamped)
static struct { uint64_t lo, hi, seq; } g_dirty[DIRTY_RING];
static _Atomic uint64_t g_dirty_seq;
static pthread_mutex_t dirty_lock = PTHREAD_MUTEX_INITIALIZER;

void backend_pool_written(uint64_t addr, uint64_t size) {
    if (!size) return;
    pthread_mutex_lock(&dirty_lock);
    uint64_t s = g_dirty_seq + 1;
    g_dirty[s % DIRTY_RING].lo = addr;
    g_dirty[s % DIRTY_RING].hi = addr + size;
    g_dirty[s % DIRTY_RING].seq = s;
    g_dirty_seq = s;
    pthread_mutex_unlock(&dirty_lock);
}

// The render thread (renderthread.c) stamps each list with the write count when the game submitted it: the list sees the
// writes made before that and not the later ones (0: no list, everything so far).
static uint64_t g_dirty_limit;
uint64_t backend_dirty_seq(void) { return g_dirty_seq; }
void backend_set_dirty_limit(uint64_t seq) { g_dirty_limit = seq; }

static void dirty_check(NvnTexture *t, TexBackend *b) {
    uint64_t now = g_dirty_limit ? g_dirty_limit : g_dirty_seq;
    if (b->dirty_seq == now) return;
    if (!b->dirty_seq) { b->dirty_seq = now; return; }  // (new texture: uploaded from memory anyway)
    uint64_t lo = t->addr, hi = t->addr + tegra_total_size(&t->layout);
    pthread_mutex_lock(&dirty_lock);
    for (uint64_t s = now; s > b->dirty_seq && s + DIRTY_RING > now; s--) {
        int i = (int)(s % DIRTY_RING);
        if (g_dirty[i].seq == s && g_dirty[i].lo < hi && lo < g_dirty[i].hi) {
            if (b->scalable) {  // host data is full size (see vk_tex_unscale)
                extern void vk_tex_unscale(NvnTexture *t);
                pthread_mutex_unlock(&dirty_lock);
                vk_tex_unscale(t);
                pthread_mutex_lock(&dirty_lock);
            }
            if (b->gpu_owned && (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {  // (research: a rendered depth texture taken back from memory)
                static int n;
                if (n++ < 20) LOG("[tex] depth texture %p %ux%u (%u layers, guest %llx-%llx) re-read from memory: write %llx-%llx\n", (void *)t,
                                  t->width, t->height, b->layers, (unsigned long long)lo, (unsigned long long)hi,
                                  (unsigned long long)g_dirty[i].lo, (unsigned long long)g_dirty[i].hi);
            }
            b->gpu_owned = false;
            b->dynamic = true;
            b->checked_frame = ~0ull;
            break;
        }
    }
    pthread_mutex_unlock(&dirty_lock);
    b->dirty_seq = now;
}

TexBackend *vk_tex(NvnTexture *t, bool for_render) {
    TexBackend *b = tex_backend(t);
    if (t->addr && !for_render) dirty_check(t, b);
    if (b->pending_rgba) upload_pending(b);
    if (for_render) b->gpu_owned = true;
    else if (!b->gpu_owned) {
        extern uint64_t prof_up_ns;
        uint64_t t0 = prof_up_ns != ~0ull ? SDL_GetTicksNS() : 0;
        upload_texture(t, b);
        if (prof_up_ns != ~0ull) prof_up_ns += SDL_GetTicksNS() - t0;
    }
    return b;
}

void renderer_mark_gpu_owned(NvnTexture *t) { tex_backend(t)->gpu_owned = true; }
bool renderer_is_gpu_owned(NvnTexture *t) { return tex_backend(t)->gpu_owned; }
// Host-produced texel data (decoded video, any thread): applied on the render thread at the texture's next use.
void renderer_upload_rgba(NvnTexture *t, const void *rgba, uint32_t w, uint32_t h, uint32_t stride) {
    TexBackend *b = t->backend;
    if (!b) return;  // the game binds the texture before decoding into it; first frames may arrive earlier
    void *copy = malloc((size_t)stride * h);
    memcpy(copy, rgba, (size_t)stride * h);
    pthread_mutex_lock(&pending_lock);
    free(b->pending_rgba);
    b->pending_rgba = copy;
    b->pending_w = w; b->pending_h = h; b->pending_stride = stride;
    b->gpu_owned = true;
    pthread_mutex_unlock(&pending_lock);
}

// ================================================================== views
#define VIEW_TAB 16384
typedef struct { TexBackend *b; uint64_t k1, k2; VkImageView view; } ViewEnt;
static ViewEnt view_tab[VIEW_TAB];

// Views may still be referenced by submitted or pending work: destroyed once that has completed.
void vk_views_forget(TexBackend *b) { views_forget(b); }
uint32_t vk_view_gen;  // bumped whenever a cached view may be destroyed (exec.c's per-frame texture memo)
static void views_forget(TexBackend *b) {
    vk_view_gen++;
    for (int i = 0; i < VIEW_TAB; i++)
        if (view_tab[i].b == b) { defer((Dead){.view = view_tab[i].view}); view_tab[i].b = NULL; view_tab[i].view = VK_NULL_HANDLE; }
}

static bool g_view_storage;  // vk_storage_view: the view being made is the storage one (its own cache key and usage)
VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv, uint32_t ly0,
                          uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect) {
    uint64_t k1 = (uint64_t)type | (uint64_t)fmt << 8 | (uint64_t)lv0 << 40 | (uint64_t)nlv << 48 | (uint64_t)aspect << 56 |
                  (uint64_t)g_view_storage << 63;
    uint64_t k2 = (uint64_t)ly0 | (uint64_t)nly << 16 | (uint64_t)sw.r << 32 | (uint64_t)sw.g << 40 | (uint64_t)sw.b << 48 | (uint64_t)sw.a << 56;
    uint64_t h = ((uint64_t)(uintptr_t)b * 0x9E3779B97F4A7C15ull) ^ (k1 * 0xC2B2AE3D27D4EB4Full) ^ (k2 * 0x165667B19E3779F9ull);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (VIEW_TAB - 1);
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (VIEW_TAB - 1)) {
        ViewEnt *e = &view_tab[slot];
        if (e->b == b && e->k1 == k1 && e->k2 == k2) return e->view;
        if (!e->b) break;
    }
    extern uint64_t prof_views_created;
    prof_views_created++;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = b->image, .viewType = type,
                                .format = fmt, .components = sw, .subresourceRange = {aspect, lv0, nlv, ly0, nly}};
    // A storage image's other views (sampled, render target, sRGB / swizzled) leave storage out: their formats may not
    // support it. Its storage view leaves the rest out.
    VkImageViewUsageCreateInfo vu = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                     .usage = g_view_storage ? VK_IMAGE_USAGE_STORAGE_BIT : b->usage & ~VK_IMAGE_USAGE_STORAGE_BIT};
    if (b->storage) vi.pNext = &vu;
    VkImageView v;
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &v) != VK_SUCCESS) {
        static int nfail;
        if (nfail++ < 20) LOG("[vk] view failed: type %d fmt %d (image fmt %d) levels %u+%u layers %u+%u\n", type, fmt, b->format, lv0, nlv, ly0, nly);
        return VK_NULL_HANDLE;
    }
    ViewEnt *e = &view_tab[slot];
    if (e->b) { defer((Dead){.view = e->view}); vk_view_gen++; }  // evict (16 probes taken)
    *e = (ViewEnt){b, k1, k2, v};
    return v;
}

static VkImageViewType view_type_for(int textype) {
    switch (textype) {
    case TEXTYPE_1D: return VK_IMAGE_VIEW_TYPE_1D;
    case TEXTYPE_3D: return VK_IMAGE_VIEW_TYPE_3D;
    case TEXTYPE_CUBE: return VK_IMAGE_VIEW_TYPE_CUBE;
    case TEXTYPE_2D_ARRAY: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case TEXTYPE_CUBE_ARRAY: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    case TEXTYPE_1D_ARRAY: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}

static const VkComponentMapping IDENTITY = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

// render: attachment view (one level, one layer). Otherwise a sampled view of type want_textype (TEXTYPE_*),
// with the NVN component swizzle, converting between compatible types the way renderer.m does.
VkImageView vk_view(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render, int want_textype) {
    TexBackend *b = vk_tex(t, render);
    if (!has_view) v = NULL;
    VkFormat fmt = b->format;
    if (v && (v->setmask & 4) && !(b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
        VkFormat vf = fmt_of(v->format).vf;
        if (vk_format(t->format) == b->format) fmt = vf;  // (fallback images keep their own format)
    }
    uint32_t lv0 = v && (v->setmask & 1) ? v->base_level : 0;
    uint32_t nlv = v && (v->setmask & 1) && v->num_levels ? v->num_levels : b->levels - (lv0 < b->levels ? lv0 : 0);
    uint32_t ly0 = v && (v->setmask & 2) ? v->min_layer : 0;
    uint32_t nly = v && (v->setmask & 2) && v->num_layers ? v->num_layers : b->layers - (ly0 < b->layers ? ly0 : 0);
    if (lv0 >= b->levels) lv0 = b->levels - 1;
    if (lv0 + nlv > b->levels) nlv = b->levels - lv0;
    if (ly0 >= b->layers) ly0 = b->layers - 1;
    if (ly0 + nly > b->layers) nly = b->layers - ly0;
    if (render) {
        if (b->base_type == VK_IMAGE_VIEW_TYPE_3D) {  // every slice of the level (a layered pass: gl_Layer picks one)
            uint32_t slices = b->depth >> lv0 ? b->depth >> lv0 : 1;
            uint32_t z0 = v && (v->setmask & 2) ? v->min_layer : 0, nz = v && (v->setmask & 2) && v->num_layers ? v->num_layers : slices;
            if (z0 >= slices) z0 = 0;
            if (z0 + nz > slices) nz = slices - z0;
            return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D_ARRAY, fmt, lv0, 1, z0, nz, IDENTITY, b->aspect);
        }
        return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, fmt, lv0, 1, ly0, 1, IDENTITY, b->aspect);
    }
    VkImageAspectFlags aspect = b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect;
    // NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL (the view's, else the texture's): the stencil (Z-A's lighting reads its
    // material bits from the scene's stencil; taken as depth, the bits of a float made stripes on every surface)
    uint32_t dsm = v && (v->setmask & 16) ? v->ds_mode : t->ds_mode;
    if (dsm == 1 && (b->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
    VkComponentMapping sw = IDENTITY;
    if (!(b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) && b->samples == 1) {
        const uint8_t *s = v && (v->setmask & 8) ? v->swizzle : t->swizzle;
        uint8_t c[4];
        for (int i = 0; i < 4; i++) c[i] = s[i] <= 5 ? s[i] : (uint8_t)(2 + i);
        if (c[0] || c[1] || c[2] || c[3]) {  // never set: identity
            if (fmt_of(t->format).alpha_one)
                for (int i = 0; i < 4; i++) if (c[i] == 5) c[i] = 1;
            // NVN swizzle 0 zero, 1 one, 2..5 r,g,b,a == VkComponentSwizzle - 1
            sw = (VkComponentMapping){(VkComponentSwizzle)(c[0] + 1), (VkComponentSwizzle)(c[1] + 1), (VkComponentSwizzle)(c[2] + 1),
                                      (VkComponentSwizzle)(c[3] + 1)};
        } else if (fmt_of(t->format).alpha_one) {
            sw.a = VK_COMPONENT_SWIZZLE_ONE;
        }
    }
    VkImageViewType want = view_type_for(want_textype);
    VkImageViewType have = b->base_type;
    if (b->samples > 1) return vk_dummy_view(want_textype);  // multisampled images can't be sampled as 2D
    if (want == have) {
        if (have == VK_IMAGE_VIEW_TYPE_CUBE) nly = 6;
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, nly, sw, aspect);
    }
    if (want == VK_IMAGE_VIEW_TYPE_2D && (have == VK_IMAGE_VIEW_TYPE_2D_ARRAY || have == VK_IMAGE_VIEW_TYPE_CUBE))
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, 1, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_2D_ARRAY && (have == VK_IMAGE_VIEW_TYPE_2D || have == VK_IMAGE_VIEW_TYPE_CUBE))
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, have == VK_IMAGE_VIEW_TYPE_CUBE ? nly : 1, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_CUBE && have == VK_IMAGE_VIEW_TYPE_2D_ARRAY && b->layers >= 6 && b->width == b->height)
        return vk_image_view(b, want, fmt, lv0, nlv, ly0 + 6 <= b->layers ? ly0 : 0, 6, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_1D_ARRAY && have == VK_IMAGE_VIEW_TYPE_1D)
        return vk_image_view(b, want, fmt, lv0, nlv, 0, 1, sw, aspect);
    static int nlog;
    if (nlog++ < 30) LOG("[tex] type mismatch: want %d have %d (%ux%ux%u target %u fmt %#x layers %u)\n", want, have, t->width,
                         t->height, t->depth, t->target, t->format, b->layers);
    return vk_dummy_view(want_textype);
}

// The storage view of an NVN image (SUST / SURED): one level, identity swizzle, the image's own format.
VkImageView vk_storage_view(NvnTexture *t, const NvnTextureView *v, bool has_view, int sudim) {
    TexBackend *b = vk_tex(t, false);
    if (!b || !b->storage || b->samples > 1) return VK_NULL_HANDLE;
    if (!has_view) v = NULL;
    uint32_t lv = v && (v->setmask & 1) ? v->base_level : 0;
    uint32_t ly0 = v && (v->setmask & 2) ? v->min_layer : 0;
    uint32_t nly = v && (v->setmask & 2) && v->num_layers ? v->num_layers : b->layers - (ly0 < b->layers ? ly0 : 0);
    if (lv >= b->levels) lv = b->levels - 1;
    if (ly0 >= b->layers) ly0 = b->layers - 1;
    if (ly0 + nly > b->layers) nly = b->layers - ly0;
    VkImageViewType type;
    switch (sudim) {
    case SUDIM_1D: type = VK_IMAGE_VIEW_TYPE_1D; nly = 1; break;
    case SUDIM_1D_ARRAY: type = VK_IMAGE_VIEW_TYPE_1D_ARRAY; break;
    case SUDIM_2D: type = VK_IMAGE_VIEW_TYPE_2D; nly = 1; break;
    case SUDIM_3D: type = VK_IMAGE_VIEW_TYPE_3D; ly0 = 0; nly = 1; break;
    default: type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
    }
    if ((type == VK_IMAGE_VIEW_TYPE_3D) != (b->base_type == VK_IMAGE_VIEW_TYPE_3D)) return VK_NULL_HANDLE;
    g_view_storage = true;
    VkImageView view = vk_image_view(b, type, b->format, lv, 1, ly0, nly, IDENTITY, VK_IMAGE_ASPECT_COLOR_BIT);
    g_view_storage = false;
    return view;
}

// 1x1 black textures for unbound slots, per type
VkImageView vk_dummy_view(int textype) {
    static TexBackend dummies[10];
    static VkImageView views[10];
    if (textype < 0 || textype >= 10) textype = TEXTYPE_2D;
    if (views[textype]) return views[textype];
    TexBackend *b = &dummies[textype];
    VkImageViewType vt = view_type_for(textype);
    bool cube = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                            .imageType = vt == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D
                                       : vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = cube ? 6 : 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL));
    VkCommandBuffer cb = vk_upload_cmd();
    vk_image_init_layout(cb, b->image, VK_IMAGE_ASPECT_COLOR_BIT);
    VkClearColorValue zero = {{0, 0, 0, 0}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, cube ? 6 : 1};
    vkCmdClearColorImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    b->format = VK_FORMAT_R8G8B8A8_UNORM;
    b->levels = 1;
    b->layers = cube ? 6 : 1;
    views[textype] = vk_image_view(b, vt, b->format, 0, 1, 0, cube ? 6 : 1, IDENTITY, VK_IMAGE_ASPECT_COLOR_BIT);
    return views[textype];
}

// 1x1 storage images for image bindings with nothing usable bound: R32_UINT (atomics) or RGBA16F (stores), per SUDIM_*
VkImageView vk_dummy_storage_view(int sudim, bool atomic) {
    static TexBackend dummies[2][6];
    static VkImageView views[2][6];
    if (sudim < 0 || sudim >= 6 || sudim == SUDIM_1D_BUFFER) sudim = SUDIM_2D;
    if (views[atomic][sudim]) return views[atomic][sudim];
    TexBackend *b = &dummies[atomic][sudim];
    VkFormat f = atomic ? VK_FORMAT_R32_UINT : VK_FORMAT_R16G16B16A16_SFLOAT;
    VkImageViewType vt = sudim == SUDIM_1D ? VK_IMAGE_VIEW_TYPE_1D : sudim == SUDIM_1D_ARRAY ? VK_IMAGE_VIEW_TYPE_1D_ARRAY
                       : sudim == SUDIM_2D ? VK_IMAGE_VIEW_TYPE_2D : sudim == SUDIM_3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = vt == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D
                                       : vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D,
                            .format = f, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_STORAGE_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL));
    vk_image_init_layout(vk_upload_cmd(), b->image, VK_IMAGE_ASPECT_COLOR_BIT);
    b->format = f;
    b->levels = b->layers = 1;
    views[atomic][sudim] = vk_image_view(b, vt, f, 0, 1, 0, 1, IDENTITY, VK_IMAGE_ASPECT_COLOR_BIT);
    return views[atomic][sudim];
}

// ================================================================== samplers
static VkCompareOp compare_op(uint32_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

// A sampler with depth compare used by a texture instruction without .DC (Legends Z-A's G-buffer shaders read the
// cascaded shadow map's raw depth and compare in code): Maxwell compares only for .DC instructions, Vulkan leaves the
// result of a compare sampler on a non-shadow image type undefined (NVIDIA: 0/1 - no cast shadows). Those get the same
// sampler without compare.
static VkSampler sampler_create(NvnSamplerDesc *d, bool cmp);
VkSampler vk_sampler(NvnSamplerDesc *d) { return d->backend ? (VkSampler)d->backend : sampler_create(d, true); }
VkSampler vk_sampler_for(NvnSamplerDesc *d, bool shadow) {
    if (shadow || !d->compare_mode || ENV("LARECOMP_KEEP_COMPARE")) return vk_sampler(d);
    return d->backend_nocmp ? (VkSampler)d->backend_nocmp : sampler_create(d, false);
}
static VkSampler sampler_create(NvnSamplerDesc *d, bool cmp) {
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = d->mag_filter == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    uint32_t mf = d->min_filter;
    si.minFilter = (mf == 1 || mf == 3 || mf == 5) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = mf == 4 || mf == 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSamplerAddressMode modes[3];
    bool border = false;
    for (int i = 0; i < 3; i++) {
        switch (d->wrap[i]) {
        case 0: case 7: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;
        case 1: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT; break;
        case 2: case 3: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; break;
        case 4: case 5: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; border = true; break;
        case 6: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
        default: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    }
    si.addressModeU = modes[0]; si.addressModeV = modes[1]; si.addressModeW = modes[2];
    si.minLod = d->lod_min;
    si.maxLod = d->lod_max > 1000 ? VK_LOD_CLAMP_NONE : d->lod_max;
    if (mf <= 1) si.maxLod = si.minLod + 0.25f;  // not mipmapped: stay on the base level
    if (si.maxLod < si.minLod) si.maxLod = si.minLod;
    si.mipLodBias = d->lod_bias;
    if (d->max_aniso > 1) { si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = fminf(d->max_aniso, 16); }
    // Anisotropic filtering: the game's own by default (it asks for little or none, sized for the Switch). Raising it
    // (LARECOMP_ANISO=<n>, mipmapped, linearly filtered colour samplers) keeps paths and grass sharp at grazing angles but
    // also keeps tiled detail sharp out to the horizon: Arceus's sea then shows its wave texture's tiles as a checkerboard
    // (Eden and the Switch blur it), so it is not the default.
    static int aniso = -1;
    if (aniso < 0) aniso = ENV("LARECOMP_ANISO") ? atoi(ENV("LARECOMP_ANISO")) : 0;
    if (aniso > 1 && mf >= 2 && si.minFilter == VK_FILTER_LINEAR && !d->compare_mode) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = fmaxf(si.maxAnisotropy, fminf((float)aniso, 16));
    }
    static float lodmin = -1;
    if (lodmin < 0) lodmin = ENV("LARECOMP_LOD_MIN") ? (float)atof(ENV("LARECOMP_LOD_MIN")) : 0;
    if (lodmin > 0 && mf > 1) si.minLod = fmaxf(si.minLod, lodmin);
    static int stest = -1;
    if (stest < 0) stest = ENV("LARECOMP_SAMPLER_TEST") != NULL;
    if (stest && mf > 1) { si.minLod = 0; si.maxLod = VK_LOD_CLAMP_NONE; si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = 16; }
    if (d->compare_mode && cmp) {
        si.compareEnable = VK_TRUE;
        si.compareOp = ENV("LARECOMP_NO_SHADOW") ? VK_COMPARE_OP_ALWAYS : compare_op(d->compare_func);
    }
    VkSamplerCustomBorderColorCreateInfoEXT cb = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    if (border) {
        const float *c = d->border;
        if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 0) si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        else if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        else if (c[0] == 1 && c[1] == 1 && c[2] == 1 && c[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else if (g_vk.custom_border) {
            memcpy(cb.customBorderColor.float32, c, 16);
            si.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            si.pNext = &cb;
        } else si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    // The reduction filter (nvnSamplerBuilderSetReductionFilter: average, min, max - the same values as Vulkan's): a
    // min / max sampler returns the smallest / largest of the texels it filters instead of their average. Z-A's prop
    // culling reads its depth pyramid with one; averaged, near and far texels blended into depths nothing on screen
    // has, and props in plain view tested as hidden behind them (pop-in). LARECOMP_NO_MINMAX=1: averaged (research).
    VkSamplerReductionModeCreateInfo rm = {.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
    static int nominmax = -1;
    if (nominmax < 0) nominmax = ENV("LARECOMP_NO_MINMAX") != NULL;
    if ((d->reduction == 1 || d->reduction == 2) && g_vk.minmax && !nominmax && !si.compareEnable) {
        rm.reductionMode = d->reduction == 1 ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX;
        rm.pNext = si.pNext;
        si.pNext = &rm;
        si.anisotropyEnable = VK_FALSE;  // (not with a reduction)
        static int logged;
        if (!logged++) LOG("[vk] min / max reduction samplers on (first: %s)\n", d->reduction == 1 ? "min" : "max");
    }
    VkSampler s;
    VK_CHECK(vkCreateSampler(g_vk.dev, &si, NULL, &s));
    if (cmp) d->backend = (void *)s;
    else d->backend_nocmp = (void *)s;
    return s;
}

VkSampler vk_default_sampler(bool linear) {
    static VkSampler s[2];
    if (!s[linear]) {
        VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                  .magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                  .minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                  .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f};
        VK_CHECK(vkCreateSampler(g_vk.dev, &si, NULL, &s[linear]));
    }
    return s[linear];
}
