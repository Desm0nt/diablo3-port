// [d3-perf][nonadreno] GPU self-test of the exact optimisations D3 turns on by default (jni_android.c DM_GFX_D3PERF; switches in
// xbg_game.cpp "NVN d3-perf"). They were verified only on Adreno, and each one can blank the 3D scene on a driver that handles
// it differently: D3 culls objects by the samples-passed reports (zeros hide the world), and a depth clear that does not happen
// or a NaN that is not saturated to 0 hides or blackens what is drawn. So on every other GPU the engine runs this once during
// initialization, before the game's first frame: one command buffer, one submission, one fence wait (timeout = every check
// fails), every Vulkan result checked; then each optimisation whose check fails is turned off.
//
//  report_inpass  A 16x16 colour + depth pass draws an occluder over the left half, then two occlusion-query "boxes" (colour
//                 writes off, depth writes off, as D3's visibility boxes): the in-pass way of DM_GFX_REPORT_INPASS (query ended
//                 inside the pass, a bottom-of-pipe timestamp written inside it, the pass goes on with more draws) against the
//                 former way (the query's pass ended, the timestamp outside, a new LOAD pass). The results are copied with
//                 vkCmdCopyQueryPoolResults as the engine does (with and without WAIT_BIT) and read back with
//                 vkGetQueryPoolResults. PASS: in-pass > 0 and equal to the former way (128 and 256 with precise queries).
//  lazy_clear     A twin render pass with loadOp / stencilLoadOp CLEAR, as lazy_twin() makes it (GENERAL layouts, the engine's
//                 subpass dependencies), on attachments first filled with garbage (blue, a depth that fails every draw, stencil
//                 0x55); then a white rectangle over the left half (depth 0.5) and a green one over the top half behind it
//                 (stencil EQUAL 0 where the format has stencil). Done for depth 1.0 + LESS_OR_EQUAL on D24S8 (or D32S8) and for
//                 depth 0.0 + GREATER_OR_EQUAL (reverse Z) on D32_SFLOAT, since the engine does not know which convention a game
//                 uses. PASS: every pixel as expected (white left, green top right, the clear colour bottom right). The former
//                 path (LOAD pass + vkCmdClearAttachments) runs as a reference for the log.
//  nan_sat        xbg_selftest.comp evaluates FClamp(x, 0, 1) on NaN, +-inf, -0, 0.5, 2, -3, 1: as NClamp (what
//  f2i_notrunc    DM_GFX_NAN_MINMAX makes of it) and kept as FClamp (DM_GFX_NAN_SAT); ConvertFToS / ConvertFToU of
//  ldc_canon      NClamp(Trunc(x)) with and without the Trunc (DM_GFX_F2I_NOTRUNC); the LDC vec4 indexes ((x + 8) >> 2) >> 2
//                 and their canonical x >> 4 (DM_GFX_LDC_CANON). The variants come from the engine's own rewrite functions.
//                 PASS: the rewritten module writes the same bits as the original (+0 and -0 count as equal for the clamp).
//                 LDC_CANON is exact integer arithmetic by construction; the check still proves the rewritten module runs.
#include "xbg_selftest.h"
#include "xbg_selftest_spv.h"
#include "xbg_spirv_ldc.h"
#include "xbg_spirv_peep.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>

namespace {
constexpr uint32_t W = 16, H = 16;
// Host-visible buffer: query copies (without / with WAIT_BIT), 4 colour readbacks, 4 compute data blocks
constexpr VkDeviceSize OFF_QNOWAIT = 0, OFF_QWAIT = 64, OFF_IMG = 256, IMG_BYTES = W * H * 4, OFF_CS = 4608, CS_STRIDE = 512, BUF_BYTES = 8192;
// The data block of xbg_selftest.comp (std430 offsets)
constexpr uint32_t D_SAT_IN = 0, D_F2I_IN = 32, D_BONE = 64, D_TABLE = 80, D_SAT_OUT = 336, D_F2I_OUT = 368, D_F2U_OUT = 400, D_LDC_OUT = 432, D_BYTES = 448;
constexpr uint32_t SENTINEL = 0xdeadbeefu;
// R8G8B8A8_UNORM texels read as little-endian words
constexpr uint32_t RED = 0xff0000ffu, WHITE = 0xffffffffu, GREEN = 0xff00ff00u, BLUE = 0xffff0000u, BLACK = 0xff000000u;

struct Ctx {
    VkDevice dev = VK_NULL_HANDLE; VkPhysicalDeviceMemoryProperties mem{};
    std::vector<std::function<void()>> undo; bool leak = false; char *err = nullptr; size_t err_bytes = 0;
    ~Ctx() { if (!leak) for (size_t i = undo.size(); i-- > 0;) undo[i](); }   // after a GPU timeout everything is leaked, never freed in use
    bool ok(VkResult r, const char *what) { if (r == VK_SUCCESS) return true; if (!*err) snprintf(err, err_bytes, "%s=%d", what, int(r)); return false; }
    bool fail(const char *what) { if (!*err) snprintf(err, err_bytes, "%s", what); return false; }
    uint32_t type(uint32_t bits, VkMemoryPropertyFlags need, VkMemoryPropertyFlags want) const {
        for (int pass = 0; pass < 2; pass++) for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
            const VkMemoryPropertyFlags f = mem.memoryTypes[i].propertyFlags;
            if ((bits & (1u << i)) && (f & need) == need && (pass || (f & want) == want)) return i;
        }
        return UINT32_MAX;
    }
    bool memory(const VkMemoryRequirements &r, VkMemoryPropertyFlags need, VkMemoryPropertyFlags want, VkDeviceMemory &m, VkMemoryPropertyFlags *got = nullptr) {
        const uint32_t t = type(r.memoryTypeBits, need, want); if (t == UINT32_MAX) return fail("no memory type");
        VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; a.allocationSize = r.size; a.memoryTypeIndex = t;
        if (!ok(vkAllocateMemory(dev, &a, nullptr, &m), "vkAllocateMemory")) return false;
        VkDevice d = dev; undo.push_back([d, m] { vkFreeMemory(d, m, nullptr); });
        if (got) *got = mem.memoryTypes[t].propertyFlags;
        return true;
    }
};

bool has_stencil(VkFormat f) { return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT; }
const char *fmt_name(VkFormat f) {
    switch (f) { case VK_FORMAT_D24_UNORM_S8_UINT: return "d24s8"; case VK_FORMAT_D32_SFLOAT_S8_UINT: return "d32s8"; case VK_FORMAT_D32_SFLOAT: return "d32"; default: return "?"; }
}

bool make_image(Ctx &c, VkFormat f, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImage &img, VkImageView &view) {
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ic.imageType = VK_IMAGE_TYPE_2D; ic.format = f; ic.extent = {W, H, 1};
    ic.mipLevels = 1; ic.arrayLayers = 1; ic.samples = VK_SAMPLE_COUNT_1_BIT; ic.tiling = VK_IMAGE_TILING_OPTIMAL; ic.usage = usage;
    ic.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!c.ok(vkCreateImage(c.dev, &ic, nullptr, &img), "vkCreateImage")) return false;
    { VkDevice d = c.dev; VkImage i = img; c.undo.push_back([d, i] { vkDestroyImage(d, i, nullptr); }); }
    VkMemoryRequirements r; vkGetImageMemoryRequirements(c.dev, img, &r); VkDeviceMemory m;
    if (!c.memory(r, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m) || !c.ok(vkBindImageMemory(c.dev, img, m, 0), "vkBindImageMemory")) return false;
    VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vc.image = img; vc.viewType = VK_IMAGE_VIEW_TYPE_2D; vc.format = f;
    vc.subresourceRange = {aspect, 0, 1, 0, 1};
    if (!c.ok(vkCreateImageView(c.dev, &vc, nullptr, &view), "vkCreateImageView")) return false;
    { VkDevice d = c.dev; VkImageView v = view; c.undo.push_back([d, v] { vkDestroyImageView(d, v, nullptr); }); }
    return true;
}

// The engine's render pass for one colour target + depth (create_pass), or its lazy-clear twin (lazy_twin: CLEAR load ops)
bool make_pass(Ctx &c, VkFormat depth, bool clear, VkRenderPass &rp) {
    VkAttachmentDescription a[2]{};
    for (int k = 0; k < 2; k++) {
        a[k].format = k ? depth : VK_FORMAT_R8G8B8A8_UNORM; a[k].samples = VK_SAMPLE_COUNT_1_BIT;
        a[k].loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD; a[k].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a[k].stencilLoadOp = clear && k == 1 && has_stencil(depth) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        a[k].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE; a[k].initialLayout = a[k].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_GENERAL}, ds{1, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount = 1; sub.pColorAttachments = &color; sub.pDepthStencilAttachment = &ds;
    VkSubpassDependency dep[2]{};
    dep[0] = {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
              VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0};
    dep[1] = {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0};
    VkRenderPassCreateInfo rc{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rc.attachmentCount = 2; rc.pAttachments = a; rc.subpassCount = 1; rc.pSubpasses = &sub; rc.dependencyCount = 2; rc.pDependencies = dep;
    if (!c.ok(vkCreateRenderPass(c.dev, &rc, nullptr, &rp), "vkCreateRenderPass")) return false;
    VkDevice d = c.dev; c.undo.push_back([d, rp] { vkDestroyRenderPass(d, rp, nullptr); });
    return true;
}

struct Target { VkImage color{}, depth{}; VkImageView cv{}, dv{}; VkFramebuffer fb{}; VkFormat df{}; };
bool make_target(Ctx &c, VkFormat df, VkRenderPass rp, Target &t) {
    t.df = df;
    if (!make_image(c, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, t.color, t.cv)) return false;
    if (!make_image(c, df, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil(df) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0), t.depth, t.dv)) return false;
    VkImageView v[2] = {t.cv, t.dv};
    VkFramebufferCreateInfo fc{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fc.renderPass = rp; fc.attachmentCount = 2; fc.pAttachments = v; fc.width = W; fc.height = H; fc.layers = 1;
    if (!c.ok(vkCreateFramebuffer(c.dev, &fc, nullptr, &t.fb), "vkCreateFramebuffer")) return false;
    VkDevice d = c.dev; VkFramebuffer f = t.fb; c.undo.push_back([d, f] { vkDestroyFramebuffer(d, f, nullptr); });
    return true;
}

bool make_module(Ctx &c, const uint32_t *w, size_t n, VkShaderModule &m) {
    VkShaderModuleCreateInfo sc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sc.codeSize = n * 4; sc.pCode = w;
    if (!c.ok(vkCreateShaderModule(c.dev, &sc, nullptr, &m), "vkCreateShaderModule")) return false;
    VkDevice d = c.dev; c.undo.push_back([d, m] { vkDestroyShaderModule(d, m, nullptr); });
    return true;
}

struct Push { float rect[4]; float color[4]; float z; };
struct Gfx { VkShaderModule vs{}, fs{}; VkPipelineLayout layout{}; };
bool make_pipe(Ctx &c, const Gfx &g, VkRenderPass rp, VkCompareOp cmp, bool depth_write, bool stencil_eq0, bool color, VkPipeline &p) {
    VkPipelineShaderStageCreateInfo st[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = g.vs; st[0].pName = "main"; st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = g.fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0, 0, float(W), float(H), 0, 1}; VkRect2D sc{{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vs{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vs.viewportCount = 1; vs.pViewports = &vp; vs.scissorCount = 1; vs.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = depth_write; ds.depthCompareOp = cmp;
    if (stencil_eq0) { ds.stencilTestEnable = VK_TRUE; ds.front = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 0xff, 0, 0}; ds.back = ds.front; }
    VkPipelineColorBlendAttachmentState ba{}; ba.colorWriteMask = color ? 0xfu : 0u;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkGraphicsPipelineCreateInfo pc{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; pc.stageCount = 2; pc.pStages = st; pc.pVertexInputState = &vi; pc.pInputAssemblyState = &ia;
    pc.pViewportState = &vs; pc.pRasterizationState = &rs; pc.pMultisampleState = &ms; pc.pDepthStencilState = &ds; pc.pColorBlendState = &cb; pc.layout = g.layout; pc.renderPass = rp;
    if (!c.ok(vkCreateGraphicsPipelines(c.dev, VK_NULL_HANDLE, 1, &pc, nullptr, &p), "vkCreateGraphicsPipelines")) return false;
    VkDevice d = c.dev; c.undo.push_back([d, p] { vkDestroyPipeline(d, p, nullptr); });
    return true;
}

void draw(VkCommandBuffer cmd, VkPipelineLayout layout, VkPipeline p, float x0, float y0, float x1, float y1, uint32_t rgba, float z) {
    Push q{{x0, y0, x1, y1}, {float(rgba & 255) / 255.f, float(rgba >> 8 & 255) / 255.f, float(rgba >> 16 & 255) / 255.f, float(rgba >> 24) / 255.f}, z};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof q, &q);
    vkCmdDraw(cmd, 6, 1, 0, 0);
}
void full_barrier(VkCommandBuffer cmd) {   // as the engine's full_barrier between operations
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
}
void begin_pass(VkCommandBuffer cmd, VkRenderPass rp, VkFramebuffer fb, const VkClearValue *values) {
    VkRenderPassBeginInfo b{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; b.renderPass = rp; b.framebuffer = fb; b.renderArea.extent = {W, H};
    b.clearValueCount = values ? 2 : 0; b.pClearValues = values;
    vkCmdBeginRenderPass(cmd, &b, VK_SUBPASS_CONTENTS_INLINE);
}
VkClearValue color_value(uint32_t rgba) { VkClearValue v{}; for (int k = 0; k < 4; k++) v.color.float32[k] = float(rgba >> (8 * k) & 255) / 255.f; return v; }
// The engine's former clear path (begin with LOAD, then vkCmdClearAttachments over the whole framebuffer)
void clear_attachments(VkCommandBuffer cmd, uint32_t rgba, float depth, uint32_t stencil, bool with_stencil) {
    VkClearAttachment a[2]{}; a[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; a[0].colorAttachment = 0; a[0].clearValue = color_value(rgba);
    a[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | (with_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0); a[1].clearValue.depthStencil = {depth, stencil};
    VkClearRect r{{{0, 0}, {W, H}}, 0, 1}; vkCmdClearAttachments(cmd, 2, a, 1, &r);
}

uint32_t expected_lazy(uint32_t x, uint32_t y) { return x < W / 2 ? WHITE : y < H / 2 ? GREEN : RED; }
// +0 and -0 count as equal (both write 0 to a UNORM target); NaN equals NaN whatever its payload
bool same_sat(uint32_t a, uint32_t b) {
    auto nan = [](uint32_t v) { return (v & 0x7f800000u) == 0x7f800000u && (v & 0x7fffffu); };
    return a == b || (!(a & 0x7fffffffu) && !(b & 0x7fffffffu)) || (nan(a) && nan(b));
}

bool run(Ctx &c, const XbgSelftestIn &in, XbgSelftestOut &out) {
    c.dev = in.device; vkGetPhysicalDeviceMemoryProperties(in.physical, &c.mem);
    auto attach = [&](VkFormat f) { VkFormatProperties p{}; vkGetPhysicalDeviceFormatProperties(in.physical, f, &p); return (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0; };
    const VkFormat ds = attach(VK_FORMAT_D24_UNORM_S8_UINT) ? VK_FORMAT_D24_UNORM_S8_UINT : attach(VK_FORMAT_D32_SFLOAT_S8_UINT) ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_UNDEFINED;
    if (ds == VK_FORMAT_UNDEFINED) return c.fail("no depth-stencil attachment format");
    const VkFormat d32 = attach(VK_FORMAT_D32_SFLOAT) ? VK_FORMAT_D32_SFLOAT : ds;
    if (!in.nan_swap) return c.fail("no NaN swap function");

    // Host-visible buffer
    VkBuffer buf; uint8_t *map = nullptr; VkDeviceMemory bm; VkMemoryPropertyFlags bflags = 0;
    {
        VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bc.size = BUF_BYTES; bc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!c.ok(vkCreateBuffer(c.dev, &bc, nullptr, &buf), "vkCreateBuffer")) return false;
        { VkDevice d = c.dev; c.undo.push_back([d, buf] { vkDestroyBuffer(d, buf, nullptr); }); }
        VkMemoryRequirements r; vkGetBufferMemoryRequirements(c.dev, buf, &r);
        if (!c.memory(r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, bm, &bflags) || !c.ok(vkBindBufferMemory(c.dev, buf, bm, 0), "vkBindBufferMemory")) return false;
        void *p = nullptr; if (!c.ok(vkMapMemory(c.dev, bm, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory")) return false;
        map = static_cast<uint8_t *>(p); { VkDevice d = c.dev; c.undo.push_back([d, bm] { vkUnmapMemory(d, bm); }); }
    }
    const bool coherent = (bflags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    memset(map, 0, BUF_BYTES); memset(map + OFF_QNOWAIT, 0xff, 64); memset(map + OFF_QWAIT, 0xff, 64);   // a query copy that writes nothing leaves ~0
    {
        const uint32_t sat_in[8] = {0x7fc00000u, 0x7f800000u, 0xff800000u, 0x80000000u, 0x3f000000u, 0x40000000u, 0xc0400000u, 0x3f800000u};   // NaN +inf -inf -0 0.5 2 -3 1
        const float f2i_in[8] = {1.5f, -1.5f, 2.999f, -2.999f, 0.75f, -0.75f, 100.f, 0.f}; const uint32_t f2i_nan = 0x7fc00000u;
        for (int v = 0; v < 4; v++) {
            uint8_t *b = map + OFF_CS + v * CS_STRIDE;
            memcpy(b + D_SAT_IN, sat_in, 32); memcpy(b + D_F2I_IN, f2i_in, 32); memcpy(b + D_F2I_IN + 28, &f2i_nan, 4);
            for (uint32_t k = 0; k < 4; k++) memcpy(b + D_BONE + 4 * k, &k, 4);
            for (uint32_t k = 0; k < 64; k++) { float t = float(k) * 1.25f + 0.5f; memcpy(b + D_TABLE + 4 * k, &t, 4); }
            for (uint32_t o = D_SAT_OUT; o < D_BYTES; o += 4) memcpy(b + o, &SENTINEL, 4);
        }
    }
    if (!coherent) { VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; r.memory = bm; r.size = VK_WHOLE_SIZE; if (!c.ok(vkFlushMappedMemoryRanges(c.dev, 1, &r), "vkFlushMappedMemoryRanges")) return false; }

    // Compute: the original module (after the engine's NaN swap) and one variant per rewrite
    std::vector<uint32_t> orig(xbg_selftest_cs, xbg_selftest_cs + sizeof(xbg_selftest_cs) / 4), sat = orig, f2i, ldc;
    size_t kept = 0; in.nan_swap(orig, false, nullptr); in.nan_swap(sat, true, &kept);
    size_t merges = 0;
    const size_t f2i_n = xbg_spirv_f2i_notrunc(orig.data(), orig.size(), f2i), ldc_n = xbg_spirv_ldc_canon(orig.data(), orig.size(), ldc, &merges);
    if (!f2i_n) f2i = orig;
    if (!ldc_n) ldc = orig;
    VkPipeline cp[4]; VkPipelineLayout cl; VkDescriptorSet set;
    {
        VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo lc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; lc.bindingCount = 1; lc.pBindings = &b; VkDescriptorSetLayout sl;
        if (!c.ok(vkCreateDescriptorSetLayout(c.dev, &lc, nullptr, &sl), "vkCreateDescriptorSetLayout")) return false;
        { VkDevice d = c.dev; c.undo.push_back([d, sl] { vkDestroyDescriptorSetLayout(d, sl, nullptr); }); }
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount = 1; pl.pSetLayouts = &sl;
        if (!c.ok(vkCreatePipelineLayout(c.dev, &pl, nullptr, &cl), "vkCreatePipelineLayout")) return false;
        { VkDevice d = c.dev; c.undo.push_back([d, cl] { vkDestroyPipelineLayout(d, cl, nullptr); }); }
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1}; VkDescriptorPoolCreateInfo pc{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pc.maxSets = 1; pc.poolSizeCount = 1; pc.pPoolSizes = &ps; VkDescriptorPool pool;
        if (!c.ok(vkCreateDescriptorPool(c.dev, &pc, nullptr, &pool), "vkCreateDescriptorPool")) return false;
        { VkDevice d = c.dev; c.undo.push_back([d, pool] { vkDestroyDescriptorPool(d, pool, nullptr); }); }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool = pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &sl;
        if (!c.ok(vkAllocateDescriptorSets(c.dev, &ai, &set), "vkAllocateDescriptorSets")) return false;
        VkDescriptorBufferInfo bi{buf, OFF_CS, D_BYTES}; VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet = set; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC; w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(c.dev, 1, &w, 0, nullptr);
        const std::vector<uint32_t> *mods[4] = {&orig, &sat, &f2i, &ldc};
        for (int v = 0; v < 4; v++) {
            VkShaderModule m; if (!make_module(c, mods[v]->data(), mods[v]->size(), m)) return false;
            VkComputePipelineCreateInfo cc{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; cc.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; cc.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cc.stage.module = m; cc.stage.pName = "main"; cc.layout = cl;
            if (!c.ok(vkCreateComputePipelines(c.dev, VK_NULL_HANDLE, 1, &cc, nullptr, &cp[v]), "vkCreateComputePipelines")) return false;
            VkDevice d = c.dev; VkPipeline p = cp[v]; c.undo.push_back([d, p] { vkDestroyPipeline(d, p, nullptr); });
        }
    }

    // Graphics: passes, pipelines, targets
    Gfx g;
    if (!make_module(c, xbg_selftest_vs, sizeof(xbg_selftest_vs) / 4, g.vs) || !make_module(c, xbg_selftest_fs, sizeof(xbg_selftest_fs) / 4, g.fs)) return false;
    {
        VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
        if (!c.ok(vkCreatePipelineLayout(c.dev, &pl, nullptr, &g.layout), "vkCreatePipelineLayout")) return false;
        VkDevice d = c.dev; VkPipelineLayout l = g.layout; c.undo.push_back([d, l] { vkDestroyPipelineLayout(d, l, nullptr); });
    }
    VkRenderPass ds_load, ds_clear, z_load, z_clear;
    if (!make_pass(c, ds, false, ds_load) || !make_pass(c, ds, true, ds_clear)) return false;
    if (d32 == ds) { z_load = ds_load; z_clear = ds_clear; }
    else if (!make_pass(c, d32, false, z_load) || !make_pass(c, d32, true, z_clear)) return false;
    // pipelines are made with the LOAD pass; the CLEAR twin differs only in load ops, so it is compatible (as in the engine)
    VkPipeline p_occ, p_box, pa0, pb0, pa1, pb1;
    if (!make_pipe(c, g, ds_load, VK_COMPARE_OP_LESS_OR_EQUAL, true, false, true, p_occ) ||
        !make_pipe(c, g, ds_load, VK_COMPARE_OP_LESS_OR_EQUAL, false, false, false, p_box) ||
        !make_pipe(c, g, ds_load, VK_COMPARE_OP_LESS_OR_EQUAL, true, true, true, pb0) ||
        !make_pipe(c, g, z_load, VK_COMPARE_OP_GREATER_OR_EQUAL, true, false, true, pa1) ||
        !make_pipe(c, g, z_load, VK_COMPARE_OP_GREATER_OR_EQUAL, true, has_stencil(d32), true, pb1)) return false;
    pa0 = p_occ;
    Target rt, lz[2], fl[2];
    if (!make_target(c, ds, ds_load, rt) || !make_target(c, ds, ds_load, lz[0]) || !make_target(c, ds, ds_load, fl[0]) ||
        !make_target(c, d32, z_load, lz[1]) || !make_target(c, d32, z_load, fl[1])) return false;

    // Queries
    VkQueryPool occ, ts = VK_NULL_HANDLE;
    {
        VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; q.queryType = VK_QUERY_TYPE_OCCLUSION; q.queryCount = 4;
        if (!c.ok(vkCreateQueryPool(c.dev, &q, nullptr, &occ), "vkCreateQueryPool")) return false;
        { VkDevice d = c.dev; c.undo.push_back([d, occ] { vkDestroyQueryPool(d, occ, nullptr); }); }
        if (in.timestamp_bits) {
            q.queryType = VK_QUERY_TYPE_TIMESTAMP; if (!c.ok(vkCreateQueryPool(c.dev, &q, nullptr, &ts), "vkCreateQueryPool")) return false;
            VkDevice d = c.dev; VkQueryPool t = ts; c.undo.push_back([d, t] { vkDestroyQueryPool(d, t, nullptr); });
        }
    }

    // Command buffer
    VkCommandPool pool; VkCommandBuffer cmd; VkFence fence;
    {
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pc.queueFamilyIndex = in.queue_family;
        if (!c.ok(vkCreateCommandPool(c.dev, &pc, nullptr, &pool), "vkCreateCommandPool")) return false;
        { VkDevice d = c.dev; c.undo.push_back([d, pool] { vkDestroyCommandPool(d, pool, nullptr); }); }
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        if (!c.ok(vkAllocateCommandBuffers(c.dev, &ai, &cmd), "vkAllocateCommandBuffers")) return false;
        VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (!c.ok(vkCreateFence(c.dev, &fc, nullptr, &fence), "vkCreateFence")) return false;
        VkDevice d = c.dev; c.undo.push_back([d, fence] { vkDestroyFence(d, fence, nullptr); });
    }
    VkCommandBufferBeginInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; cb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!c.ok(vkBeginCommandBuffer(cmd, &cb), "vkBeginCommandBuffer")) return false;
    {   // every image to GENERAL, the engine's only layout for attachments
        std::vector<VkImageMemoryBarrier> b;
        for (const Target *t : {&rt, &lz[0], &fl[0], &lz[1], &fl[1]}) for (int k = 0; k < 2; k++) {
            VkImageMemoryBarrier m{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; m.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            m.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; m.newLayout = VK_IMAGE_LAYOUT_GENERAL; m.srcQueueFamilyIndex = m.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            m.image = k ? t->depth : t->color;
            m.subresourceRange = {k ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil(t->df) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)) : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1};
            b.push_back(m);
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, uint32_t(b.size()), b.data());
    }
    // compute variants: orig, nan_sat, f2i_notrunc, ldc_canon, each on its own data block
    for (uint32_t v = 0; v < 4; v++) {
        const uint32_t off = uint32_t(v * CS_STRIDE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cp[v]); vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cl, 0, 1, &set, 1, &off);
        vkCmdDispatch(cmd, 1, 1, 1);
    }
    full_barrier(cmd);
    // report_inpass. Queries 0 / 1: in-pass (DM_GFX_REPORT_INPASS=1); 2 / 3: the former way (the report ends the pass).
    const VkQueryControlFlags qf = in.precise_occlusion ? VK_QUERY_CONTROL_PRECISE_BIT : 0;
    const VkClearValue rclear[2] = {color_value(BLACK), [] { VkClearValue v{}; v.depthStencil = {1.f, 0}; return v; }()};
    vkCmdResetQueryPool(cmd, occ, 0, 4); if (ts) vkCmdResetQueryPool(cmd, ts, 0, 4);
    begin_pass(cmd, ds_clear, rt.fb, rclear);
    draw(cmd, g.layout, p_occ, -1, -1, 0, 1, WHITE, 0.5f);                      // occluder: left half at 0.5
    vkCmdBeginQuery(cmd, occ, 0, qf); draw(cmd, g.layout, p_box, -1, -1, 1, 1, 0, 0.75f); vkCmdEndQuery(cmd, occ, 0);   // behind it: right half passes
    if (ts) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ts, 0);
    vkCmdBeginQuery(cmd, occ, 1, qf); draw(cmd, g.layout, p_box, -1, -1, 1, 1, 0, 0.25f); vkCmdEndQuery(cmd, occ, 1);   // in front: all of it passes
    if (ts) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ts, 1);
    draw(cmd, g.layout, p_occ, -1, -1, 1, 0, GREEN, 0.9f);                      // the pass goes on after the reports, as in D3
    vkCmdEndRenderPass(cmd); full_barrier(cmd);
    begin_pass(cmd, ds_clear, rt.fb, rclear);
    draw(cmd, g.layout, p_occ, -1, -1, 0, 1, WHITE, 0.5f);
    vkCmdBeginQuery(cmd, occ, 2, qf); draw(cmd, g.layout, p_box, -1, -1, 1, 1, 0, 0.75f); vkCmdEndQuery(cmd, occ, 2);
    vkCmdEndRenderPass(cmd); full_barrier(cmd); if (ts) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ts, 2); full_barrier(cmd);
    begin_pass(cmd, ds_load, rt.fb, nullptr);
    vkCmdBeginQuery(cmd, occ, 3, qf); draw(cmd, g.layout, p_box, -1, -1, 1, 1, 0, 0.25f); vkCmdEndQuery(cmd, occ, 3);
    vkCmdEndRenderPass(cmd);
    // as submit_slots: copied right after the pass end (no WAIT_BIT on Adreno), and with WAIT_BIT
    vkCmdCopyQueryPoolResults(cmd, occ, 0, 4, buf, OFF_QNOWAIT, 8, VK_QUERY_RESULT_64_BIT);
    vkCmdCopyQueryPoolResults(cmd, occ, 0, 4, buf, OFF_QWAIT, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    full_barrier(cmd); if (ts) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ts, 3);
    // lazy_clear: variant 0 depth 1.0 / LESS_OR_EQUAL on ds, variant 1 depth 0.0 / GREATER_OR_EQUAL (reverse Z) on d32
    for (int v = 0; v < 2; v++) {
        const VkRenderPass load = v ? z_load : ds_load, clear = v ? z_clear : ds_clear; const VkPipeline pa = v ? pa1 : pa0, pb = v ? pb1 : pb0;
        const float cd = v ? 0.f : 1.f, garbage = v ? 1.f : 0.f, zb = v ? 0.25f : 0.75f; const bool st = has_stencil(lz[v].df);
        for (const Target *t : {&lz[v], &fl[v]}) { begin_pass(cmd, load, t->fb, nullptr); clear_attachments(cmd, BLUE, garbage, 0x55, st); vkCmdEndRenderPass(cmd); }
        full_barrier(cmd);
        VkClearValue cv[2] = {color_value(RED), {}}; cv[1].depthStencil = {cd, 0};
        begin_pass(cmd, clear, lz[v].fb, cv);                                    // DM_GFX_LAZY_CLEAR: clears as load ops
        draw(cmd, g.layout, pa, -1, -1, 0, 1, WHITE, 0.5f); draw(cmd, g.layout, pb, -1, -1, 1, 0, GREEN, zb); vkCmdEndRenderPass(cmd);
        begin_pass(cmd, load, fl[v].fb, nullptr); clear_attachments(cmd, RED, cd, 0, st);   // the former recording
        draw(cmd, g.layout, pa, -1, -1, 0, 1, WHITE, 0.5f); draw(cmd, g.layout, pb, -1, -1, 1, 0, GREEN, zb); vkCmdEndRenderPass(cmd);
        full_barrier(cmd);
    }
    {
        const VkImage src[4] = {lz[0].color, fl[0].color, lz[1].color, fl[1].color};
        for (int k = 0; k < 4; k++) { VkBufferImageCopy r{}; r.bufferOffset = OFF_IMG + k * IMG_BYTES; r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; r.imageExtent = {W, H, 1};
            vkCmdCopyImageToBuffer(cmd, src[k], VK_IMAGE_LAYOUT_GENERAL, buf, 1, &r); }
        VkMemoryBarrier hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; hb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr);
    }
    if (!c.ok(vkEndCommandBuffer(cmd), "vkEndCommandBuffer")) return false;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    if (!c.ok(vkQueueSubmit(in.queue, 1, &si, fence), "vkQueueSubmit")) return false;
    const VkResult wr = vkWaitForFences(c.dev, 1, &fence, VK_TRUE, in.timeout_ns);
    if (wr == VK_TIMEOUT) { c.leak = true; return c.fail("GPU did not finish in time (resources leaked)"); }
    if (!c.ok(wr, "vkWaitForFences")) { c.leak = wr != VK_ERROR_DEVICE_LOST; return false; }
    if (!coherent) { VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE}; r.memory = bm; r.size = VK_WHOLE_SIZE; if (!c.ok(vkInvalidateMappedMemoryRanges(c.dev, 1, &r), "vkInvalidateMappedMemoryRanges")) return false; }
    out.ran = true;

    // report_inpass
    {
        uint64_t nowait[4], wait[4], truth[4] = {};
        memcpy(nowait, map + OFF_QNOWAIT, 32); memcpy(wait, map + OFF_QWAIT, 32);
        const VkResult qr = vkGetQueryPoolResults(c.dev, occ, 0, 4, sizeof truth, truth, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        const bool same = in.precise_occlusion ? wait[0] == wait[2] && wait[1] == wait[3] : wait[2] > 0 && wait[3] > 0;
        const bool pass = qr == VK_SUCCESS && wait[0] > 0 && wait[1] > 0 && wait[0] != ~0ull && wait[1] != ~0ull && same && !memcmp(wait, truth, sizeof truth);
        out.pass[XBG_ST_REPORT_INPASS] = pass;
        snprintf(out.note[XBG_ST_REPORT_INPASS], sizeof out.note[0], "in-pass %lld/%lld, reference %lld/%lld%s%s", (long long)wait[0], (long long)wait[1], (long long)wait[2], (long long)wait[3],
                 in.precise_occlusion ? "" : ", imprecise", qr == VK_SUCCESS && !memcmp(wait, truth, sizeof truth) ? "" : ", copy != vkGetQueryPoolResults");
        out.copy_nowait_ok = !memcmp(nowait, truth, sizeof truth);
        snprintf(out.query_note, sizeof out.query_note, "query copy without WAIT %s (%lld/%lld/%lld/%lld)", out.copy_nowait_ok ? "matches" : "DIFFERS",
                 (long long)nowait[0], (long long)nowait[1], (long long)nowait[2], (long long)nowait[3]);
    }
    // lazy_clear
    {
        bool pass = true; char *n = out.note[XBG_ST_LAZY_CLEAR]; size_t at = 0;
        for (int v = 0; v < 2; v++) {
            uint32_t bad[2] = {}, quad[4] = {};
            for (int which = 0; which < 2; which++) {
                const uint8_t *img = map + OFF_IMG + (2 * v + which) * IMG_BYTES;
                for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) {
                    uint32_t p; memcpy(&p, img + 4 * (y * W + x), 4); bad[which] += p != expected_lazy(x, y);
                    if (!which && (x == W / 4 || x == 3 * W / 4) && (y == H / 4 || y == 3 * H / 4)) quad[(y > H / 2) * 2 + (x > W / 2)] = p;
                }
            }
            pass = pass && !bad[0];
            if (at < sizeof out.note[0]) at += snprintf(n + at, sizeof out.note[0] - at, "%s%s clear %.0f: %s", v ? "; " : "", fmt_name(v ? lz[1].df : lz[0].df), v ? 0.0 : 1.0, bad[0] ? "" : "ok");
            if (bad[0] && at < sizeof out.note[0]) at += snprintf(n + at, sizeof out.note[0] - at, "%u px wrong TL %08x TR %08x BL %08x BR %08x", bad[0], quad[0], quad[1], quad[2], quad[3]);
            if (bad[1] && at < sizeof out.note[0]) at += snprintf(n + at, sizeof out.note[0] - at, " (reference also %u px wrong)", bad[1]);
        }
        out.pass[XBG_ST_LAZY_CLEAR] = pass;
    }
    // shader rewrites: same bits as the original module
    {
        auto word = [&](int v, uint32_t off, int k) { uint32_t x; memcpy(&x, map + OFF_CS + v * CS_STRIDE + off + 4 * k, 4); return x; };
        bool orig_ok = true; for (uint32_t o = D_SAT_OUT; o < D_BYTES; o += 4) orig_ok = orig_ok && word(0, o, 0) != SENTINEL;
        // nan_sat
        { int bad = -1; bool ran = true; for (int k = 0; k < 8; k++) { ran = ran && word(1, D_SAT_OUT, k) != SENTINEL; if (bad < 0 && !same_sat(word(0, D_SAT_OUT, k), word(1, D_SAT_OUT, k))) bad = k; }
          static const char *names[8] = {"NaN", "+inf", "-inf", "-0", "0.5", "2", "-3", "1"};
          out.pass[XBG_ST_NAN_SAT] = orig_ok && ran && kept > 0 && bad < 0;
          if (!kept) snprintf(out.note[XBG_ST_NAN_SAT], sizeof out.note[0], "no saturate kept by the rewrite");
          else if (bad >= 0) snprintf(out.note[XBG_ST_NAN_SAT], sizeof out.note[0], "x=%s: NClamp %08x FClamp %08x", names[bad], word(0, D_SAT_OUT, bad), word(1, D_SAT_OUT, bad));
          else if (!orig_ok || !ran) snprintf(out.note[XBG_ST_NAN_SAT], sizeof out.note[0], "compute output missing");
          else snprintf(out.note[XBG_ST_NAN_SAT], sizeof out.note[0], "NaN -> %08x, %zu kept", word(1, D_SAT_OUT, 0), kept); }
        // f2i_notrunc
        { int bad = -1; bool ran = true; for (int k = 0; k < 16; k++) { const uint32_t o = k < 8 ? D_F2I_OUT : D_F2U_OUT; ran = ran && word(2, o, k & 7) != SENTINEL; if (bad < 0 && word(0, o, k & 7) != word(2, o, k & 7)) bad = k; }
          out.pass[XBG_ST_F2I_NOTRUNC] = orig_ok && ran && f2i_n > 0 && bad < 0;
          if (!f2i_n) snprintf(out.note[XBG_ST_F2I_NOTRUNC], sizeof out.note[0], "pattern not rewritten");
          else if (bad >= 0) snprintf(out.note[XBG_ST_F2I_NOTRUNC], sizeof out.note[0], "%s input %d: %08x vs %08x", bad < 8 ? "f2i" : "f2u", bad & 7, word(0, bad < 8 ? D_F2I_OUT : D_F2U_OUT, bad & 7), word(2, bad < 8 ? D_F2I_OUT : D_F2U_OUT, bad & 7));
          else if (!orig_ok || !ran) snprintf(out.note[XBG_ST_F2I_NOTRUNC], sizeof out.note[0], "compute output missing");
          else snprintf(out.note[XBG_ST_F2I_NOTRUNC], sizeof out.note[0], "%zu sites", f2i_n); }
        // ldc_canon
        { int bad = -1; bool ran = true; for (int k = 0; k < 4; k++) { ran = ran && word(3, D_LDC_OUT, k) != SENTINEL; if (bad < 0 && word(0, D_LDC_OUT, k) != word(3, D_LDC_OUT, k)) bad = k; }
          out.pass[XBG_ST_LDC_CANON] = orig_ok && ran && ldc_n > 0 && bad < 0;
          if (!ldc_n) snprintf(out.note[XBG_ST_LDC_CANON], sizeof out.note[0], "pattern not rewritten");
          else if (bad >= 0) snprintf(out.note[XBG_ST_LDC_CANON], sizeof out.note[0], "bone %d: %08x vs %08x", bad, word(0, D_LDC_OUT, bad), word(3, D_LDC_OUT, bad));
          else if (!orig_ok || !ran) snprintf(out.note[XBG_ST_LDC_CANON], sizeof out.note[0], "compute output missing");
          else snprintf(out.note[XBG_ST_LDC_CANON], sizeof out.note[0], "%zu shifts, exact by construction", ldc_n); }
    }
    return true;
}
} // namespace

const char *xbg_selftest_name(int check) {
    static const char *n[XBG_ST_COUNT] = {"report_inpass", "lazy_clear", "nan_sat", "ldc_canon", "f2i_notrunc"};
    return check >= 0 && check < XBG_ST_COUNT ? n[check] : "?";
}

uint32_t xbg_selftest_parse(const char *list) {
    uint32_t mask = 0; if (!list) return 0;
    for (const char *p = list; *p;) {
        while (*p == ',' || *p == ' ' || *p == ';' || *p == '"') p++;
        const char *e = p; while (*e && *e != ',' && *e != ' ' && *e != ';' && *e != '"') e++;
        const size_t n = size_t(e - p);
        if (n == 3 && !strncmp(p, "all", 3)) mask = (1u << XBG_ST_COUNT) - 1;
        else for (int k = 0; k < XBG_ST_COUNT; k++) if (n == strlen(xbg_selftest_name(k)) && !strncmp(p, xbg_selftest_name(k), n)) mask |= 1u << k;
        p = e;
    }
    return mask;
}

void xbg_selftest_run(const XbgSelftestIn &in, XbgSelftestOut &out) {
    const auto t0 = std::chrono::steady_clock::now();
    out = XbgSelftestOut{};
    {
        Ctx c; c.err = out.error; c.err_bytes = sizeof out.error;
        if (!run(c, in, out)) {   // the test itself failed: every check counts as FAIL
            for (int k = 0; k < XBG_ST_COUNT; k++) { out.pass[k] = 0; snprintf(out.note[k], sizeof out.note[0], "test failed: %s", out.error); }
        }
    }
    for (int k = 0; k < XBG_ST_COUNT; k++) if (in.force_fail >> k & 1) { out.pass[k] = 0; snprintf(out.note[k], sizeof out.note[0], "forced (DM_GFX_SELFTEST_FORCE_FAIL)"); }
    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
