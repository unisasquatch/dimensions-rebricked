#include "ui/overlay.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_vulkan.h"
#include "gpu/vk/device.h"
#include "gpu/vk/renderer.h"
#include "hle/button_icons.h"
#include "hle/port_settings.h"
#include "loader/game_patches.h"
#include "hle/toypad.h"
#include "hle/toypad_source.h"
#include "hle/toypad_web.h"
#include "imgui.h"
#include "ui/settings_items.h"

namespace ui {
namespace {

std::mutex g_mutex;  // ImGui: events arrive on the main thread, frames are drawn on the GPU thread
std::atomic<bool> g_open{false};
enum class Panel { Picker, Settings };
std::atomic<Panel> g_panel{Panel::Picker};
std::atomic<int64_t> g_closed_at_ms{0};
bool g_context = false, g_vulkan = false;
SDL_Window* g_window = nullptr;
float g_scale = 1.0f;  // font build scale (tallest display / 1080)
float g_ui = 1.0f;     // layout scale this frame (window height / 1080)
ImGuiStyle g_base_style;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void Close() {
    std::fprintf(stderr, "[overlay] %s closed\n", g_panel == Panel::Settings ? "settings" : "picker");
    g_open = false;
    g_closed_at_ms = NowMs();
}

// --- icon atlas (atlas.bin, made from the game's files by button_icons.cpp's BuildFigures) ---
struct Icon { float u0, v0, u1, v1, aspect; };
struct Atlas {
    bool tried = false;
    int generation = 0;  // icons::FiguresGeneration() when it was tried
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE, staging_memory = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    std::vector<std::pair<uint32_t, Icon>> icons;
    const Icon* Find(uint32_t id) const {
        for (const auto& [i, icon] : icons)
            if (i == id) return &icon;
        return nullptr;
    }
} g_atlas;

void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// Loads the atlas and records its upload into `cmd` (outside any rendering).
void LoadAtlas(VkCommandBuffer cmd) {
    g_atlas.tried = true;
    g_atlas.generation = icons::FiguresGeneration();
    g_atlas.icons.clear();
    const std::string path = toypad::Instance().FigureDir() + "/icons/atlas.bin";
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "[overlay] no icon atlas at %s yet (it is made from the game's files at startup)\n", path.c_str());
        return;
    }
    char magic[4];
    uint32_t head[4];
    in.read(magic, 4).read(reinterpret_cast<char*>(head), sizeof head);
    if (!in || std::memcmp(magic, "TPAT", 4) != 0 || head[0] != 1) return;
    const uint32_t w = head[1], h = head[2], count = head[3];
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t id;
        uint16_t r[4];
        in.read(reinterpret_cast<char*>(&id), 4).read(reinterpret_cast<char*>(r), 8);
        g_atlas.icons.push_back({id, {float(r[0]) / w, float(r[1]) / h, float(r[0] + r[2]) / w, float(r[1] + r[3]) / h,
                                      r[3] ? float(r[2]) / r[3] : 1.0f}});
    }
    const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
    vkr::Device& dev = vkr::GetDevice();
    VkDevice d = dev.device();
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    // Any failure leaves the picker with plain buttons instead of portraits (never a half-made texture).
    auto fail = [&](const char* what, VkResult r) {
        std::fprintf(stderr, "[overlay] icon atlas: %s failed (%d)\n", what, int(r));
        return;
    };
    VkResult r = vkCreateBuffer(d, &bci, nullptr, &g_atlas.staging);
    if (r != VK_SUCCESS) return fail("staging buffer", r);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d, g_atlas.staging, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = dev.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if ((r = vkAllocateMemory(d, &mai, nullptr, &g_atlas.staging_memory)) != VK_SUCCESS) return fail("staging memory", r);
    vkBindBufferMemory(d, g_atlas.staging, g_atlas.staging_memory, 0);
    void* map = nullptr;
    vkMapMemory(d, g_atlas.staging_memory, 0, bytes, 0, &map);
    in.read(static_cast<char*>(map), std::streamsize(bytes));
    vkUnmapMemory(d, g_atlas.staging_memory);

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if ((r = vkCreateImage(d, &ici, nullptr, &g_atlas.image)) != VK_SUCCESS) return fail("image", r);
    vkGetImageMemoryRequirements(d, g_atlas.image, &req);
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = dev.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if ((r = vkAllocateMemory(d, &mai, nullptr, &g_atlas.memory)) != VK_SUCCESS) {
        vkDestroyImage(d, g_atlas.image, nullptr);
        g_atlas.image = VK_NULL_HANDLE;
        return fail("image memory", r);
    }
    vkBindImageMemory(d, g_atlas.image, g_atlas.memory, 0);
    ImageBarrier(cmd, g_atlas.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, g_atlas.staging, g_atlas.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    ImageBarrier(cmd, g_atlas.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = g_atlas.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if ((r = vkCreateImageView(d, &vci, nullptr, &g_atlas.view)) != VK_SUCCESS) return fail("image view", r);
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if ((r = vkCreateSampler(d, &sci, nullptr, &g_atlas.sampler)) != VK_SUCCESS) return fail("sampler", r);
    g_atlas.set = ImGui_ImplVulkan_AddTexture(g_atlas.sampler, g_atlas.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    std::fprintf(stderr, "[overlay] %u figure icons loaded\n", count);
}

// --- the picker ---
struct State {
    int zone = 1;          // toypad::Zone: 1 centre, 2 left, 3 right
    int kind = 0;          // 0 characters, 1 vehicles & gadgets
    int sort = 0;          // 0 default, 1 franchise, 2 A to Z
    std::vector<toypad::FigureInfo> figures;
    bool loaded = false;
    bool focus_grid = false;
    bool fresh = false;  // just opened: clear ImGui key state first
} g_state;

const char* ZoneName(int z) { return z == 2 ? "Left" : z == 3 ? "Right" : "Centre"; }
int ZoneLimit(int z) { return z == 1 ? 1 : 3; }
std::string NameOf(uint32_t id) {
    for (const auto& f : g_state.figures)
        if (f.id == id) return f.name;
    return "Figure " + std::to_string(id);
}

bool IconButton(const char* id, uint32_t figure, float size) {
    const Icon* icon = g_atlas.set ? g_atlas.Find(figure) : nullptr;
    if (!icon) return ImGui::Button(id, ImVec2(size, size));
    ImGui::PushID(id);
    const bool pressed = ImGui::ImageButton("##icon", reinterpret_cast<ImTextureID>(g_atlas.set), ImVec2(size, size),
                                            ImVec2(icon->u0, icon->v0), ImVec2(icon->u1, icon->v1));
    ImGui::PopID();
    return pressed;
}

// A zone lit in colour `c`, the way the game draws its Toy Pad: the board's pale grey tinted toward a light
// version of the colour (the same formula as the web page's zoneFill).
ImU32 ZoneFill(const float* c) {
    static constexpr float kUnlit[3] = {220, 223, 227};
    const float m = std::max({c[0], c[1], c[2]});
    float out[3];
    // LEDs look bright even at low values (the game pulses its prompts between about 5 and 22 of 255).
    const float a = 1 - std::exp(-m / 18);
    for (int k = 0; k < 3; ++k) {
        if (m < 0.5f) { out[k] = kUnlit[k]; continue; }
        const float hue = c[k] / m * 255, lit = hue + (255 - hue) * 0.55f;
        out[k] = kUnlit[k] + (lit - kUnlit[k]) * a;
    }
    return IM_COL32(int(out[0]), int(out[1]), int(out[2]), 255);
}

// The Toy Pad as the game shows it: a flat grey board with the round centre pad at the top and L-shaped
// left and right zones, each lit as the game sets that pad's light (toypad.cpp). Pressing a zone chooses where
// the next figure goes; pressing a figure on it lifts it off. Laid out in the web page's 1000 x 510 units.
void DrawBoard(toypad::VirtualToyPad& pad, const std::vector<toypad::VirtualToyPad::SlotInfo>& slots, float unit) {
    const auto lights = pad.Lights();
    static float shown[3][3] = {};  // shown colour per pad, gliding toward the light (flashes pulse like the game's)
    const float glide = std::min(1.0f, ImGui::GetIO().DeltaTime / 0.06f);
    for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) shown[i][k] += (lights[i].rgb[k] - shown[i][k]) * glide;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float h = unit * 4.2f, w = std::min(avail, h * 1000 / 510), s = w / 1000;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const ImVec2 o(start.x + (avail - w) * 0.5f, start.y);
    auto P = [&](float x, float y) { return ImVec2(o.x + x * s, o.y + y * s); };
    auto Rect = [&](float x, float y, float rw, float rh, ImU32 fill, float round = 0) {
        dl->AddRectFilled(P(x, y), P(x + rw, y + rh), fill, round * s);
    };
    auto Frame = [&](float x, float y, float rw, float rh, ImU32 col, float thick, float round = 0) {
        dl->AddRect(P(x, y), P(x + rw, y + rh), col, round * s, 0, thick * s);
    };
    auto Shade = [&](float x, float y, float rw, float rh) {  // highlight fading down the zone (y 120 to 416)
        auto a = [](float yy) { return IM_COL32(255, 255, 255, int(90 * std::clamp(1 - (yy - 120) / 296, 0.0f, 1.0f))); };
        dl->AddRectFilledMultiColor(P(x, y), P(x + rw, y + rh), a(y), a(y), a(y + rh), a(y + rh));
    };
    const ImU32 ink = IM_COL32(52, 55, 62, 255);

    // The board: dark outline, face, front edge, inner frame, the stem below the centre, studs.
    Rect(8, 8, 984, 494, IM_COL32(27, 29, 34, 255), 18);
    dl->AddRectFilledMultiColor(P(24, 24), P(976, 428), IM_COL32(236, 238, 241, 255), IM_COL32(236, 238, 241, 255),
                                IM_COL32(191, 194, 199, 255), IM_COL32(191, 194, 199, 255));
    dl->AddRectFilledMultiColor(P(24, 434), P(976, 490), IM_COL32(238, 240, 242, 255), IM_COL32(238, 240, 242, 255),
                                IM_COL32(169, 173, 179, 255), IM_COL32(169, 173, 179, 255));
    Frame(44, 44, 912, 378, IM_COL32(93, 97, 104, 255), 3);
    Rect(468, 282, 64, 146, IM_COL32(163, 167, 173, 255));
    Frame(468, 282, 64, 146, IM_COL32(74, 78, 85, 255), 3);
    for (float sx : {62.0f, 876.0f}) {
        Rect(sx, 56, 62, 40, IM_COL32(179, 182, 187, 255), 4);
        Frame(sx, 56, 62, 40, ink, 3, 4);
        for (float dx : {17.0f, 45.0f})
            for (float dy : {12.0f, 29.0f}) {
                dl->AddCircleFilled(P(sx + dx, 56 + dy), 7 * s, IM_COL32(210, 212, 216, 255));
                dl->AddCircle(P(sx + dx, 56 + dy), 7 * s, ink, 0, 2.5f * s);
            }
    }

    // The zones. Each side is an L: a tall strip on the outside and a wide block along the front.
    struct Box { float x, y, w, h; };
    const Box kLeft[2] = {{56, 120, 206, 296}, {262, 280, 190, 136}};
    const Box kRight[2] = {{738, 120, 206, 296}, {548, 280, 190, 136}};
    const ImVec2 kCentre(500, 150);
    const float kCentreR = 90;
    const ImU32 select_col = ImGui::GetColorU32(ImGuiCol_ButtonActive);
    for (int z : {2, 3}) {
        const Box* b = z == 2 ? kLeft : kRight;
        for (int i = 0; i < 2; ++i) {
            Rect(b[i].x, b[i].y, b[i].w, b[i].h, ZoneFill(shown[z - 1]));
            Shade(b[i].x, b[i].y, b[i].w, b[i].h);
        }
        // Outline of the L.
        const float x0 = z == 2 ? 56 : 944, xi = z == 2 ? 262 : 738, xr = z == 2 ? 452 : 548;
        const ImVec2 pts[] = {P(x0, 120), P(xi, 120), P(xi, 280), P(xr, 280), P(xr, 416), P(x0, 416)};
        const bool sel = g_state.zone == z;
        dl->AddPolyline(pts, 6, sel ? select_col : ink, ImDrawFlags_Closed, (sel ? 8 : 4) * s);
    }
    {
        const ImVec2 c = P(kCentre.x, kCentre.y);
        dl->AddCircleFilled(c, kCentreR * s, ZoneFill(shown[0]), 64);
        for (float r : {72.0f, 54.0f, 36.0f}) dl->AddCircle(c, r * s, IM_COL32(255, 255, 255, 150), 64, 3 * s);
        const bool sel = g_state.zone == 1;
        dl->AddCircle(c, kCentreR * s, sel ? select_col : ink, 64, (sel ? 8 : 4) * s);
    }
    // The LEGO badge on the front edge.
    Rect(462, 414, 76, 80, IM_COL32(227, 0, 11, 255));
    Frame(462, 414, 76, 80, IM_COL32(22, 23, 27, 255), 3);
    {
        const ImVec2 ts = ImGui::CalcTextSize("LEGO");
        const float scale = std::min(1.0f, 64 * s / ts.x);
        ImFont* font = ImGui::GetFont();
        const float size = ImGui::GetFontSize() * scale;
        const ImVec2 at(P(500, 454).x - ts.x * scale * 0.5f, P(500, 454).y - ts.y * scale * 0.5f);
        dl->AddText(font, size, ImVec2(at.x + 1.5f, at.y + 1.5f), IM_COL32(22, 23, 27, 255), "LEGO");
        dl->AddText(font, size, at, IM_COL32(255, 212, 0, 255), "LEGO");
    }

    // Hit areas and figures. Zones first (overlappable), then the figures on top of them.
    struct Spot { float x, y, size; };
    static const Spot kSpots[4][3] = {{},
                                      {{500, 150, 150}},
                                      {{160, 214, 108}, {160, 346, 108}, {340, 348, 108}},
                                      {{840, 214, 108}, {840, 346, 108}, {660, 348, 108}}};
    auto Hit = [&](const char* id, float x, float y, float rw, float rh, int z) {
        ImGui::SetCursorScreenPos(P(x, y));
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton(id, ImVec2(rw * s, rh * s))) g_state.zone = z;
        if (ImGui::IsItemFocused() || ImGui::IsItemHovered())
            dl->AddRect(P(x, y), P(x + rw, y + rh), IM_COL32(255, 255, 255, 120), 4 * s, 0, 2 * s);
    };
    ImGui::PushID("board");
    Hit("left", kLeft[0].x, kLeft[0].y, kLeft[0].w, kLeft[0].h, 2);
    Hit("left2", kLeft[1].x, kLeft[1].y, kLeft[1].w, kLeft[1].h, 2);
    Hit("centre", kCentre.x - kCentreR, kCentre.y - kCentreR, 2 * kCentreR, 2 * kCentreR, 1);
    Hit("right", kRight[0].x, kRight[0].y, kRight[0].w, kRight[0].h, 3);
    Hit("right2", kRight[1].x, kRight[1].y, kRight[1].w, kRight[1].h, 3);
    static std::pair<uint32_t, double> placed[toypad::VirtualToyPad::kSlots];  // figure id, when it appeared
    for (int i = 0; i < toypad::VirtualToyPad::kSlots; ++i) {  // forget lifted figures, so a re-place drops again
        bool present = false;
        for (const auto& slot : slots) present |= slot.slot == i;
        if (!present) placed[i] = {0, 0};
    }
    // Figures stand on the board itself: no button frame round them.
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255, 255, 255, 60));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(255, 255, 255, 110));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    for (int z : {2, 1, 3}) {
        int n = 0;
        for (const auto& slot : slots) {
            if (int(slot.zone) != z || n >= 3) continue;
            const Spot& sp = kSpots[z][n++];
            // A figure just put down drops onto the pad, then bobs gently (phases staggered by slot).
            auto& [placed_id, placed_at] = placed[slot.slot];
            if (placed_id != slot.id) placed_id = slot.id, placed_at = ImGui::GetTime();
            const float age = float(ImGui::GetTime() - placed_at);
            float lift = 0;
            if (age < 0.3f) lift = 220 * (1 - age / 0.3f) * (1 - age / 0.3f);
            else if (age < 0.5f) lift = 16 * std::sin((age - 0.3f) / 0.2f * 3.14159f);
            else lift = 7 * std::abs(std::sin((age - 0.5f + slot.slot * 0.37f) * 3.14159f / 0.9f));
            dl->AddEllipseFilled(P(sp.x, sp.y + sp.size * 0.46f), ImVec2(sp.size * 0.34f * s, sp.size * 0.09f * s),
                                 IM_COL32(0, 0, 0, 46));
            ImGui::SetCursorScreenPos(P(sp.x - sp.size / 2, sp.y - sp.size / 2 - lift));
            ImGui::PushID(slot.slot);
            if (IconButton("remove", slot.id, sp.size * s)) pad.Remove(slot.slot);
            if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
                ImGui::SetTooltip(slot.physical ? "%s - on your Toy Pad (lift it off there)" : "%s - press to remove",
                                  slot.id ? NameOf(slot.id).c_str() : "Toy Pad figure");
            ImGui::PopID();
        }
        // Count, in the zone's corner.
        int count = 0;
        for (const auto& slot : slots) count += int(slot.zone) == z;
        char label[48];
        std::snprintf(label, sizeof label, "%s %d/%d", ZoneName(z), count, ZoneLimit(z));
        const float size = std::min(ImGui::GetFontSize() * 0.7f, 24 * s);  // small, like the page's labels
        const ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0, label);
        const ImVec2 at = z == 2 ? P(66, 126) : z == 3 ? ImVec2(P(934, 126).x - ts.x, P(0, 126).y)
                                                       : ImVec2(P(500, 0).x - ts.x * 0.5f, P(0, 250).y);
        dl->AddText(ImGui::GetFont(), size, at, IM_COL32(0, 0, 0, 120), label);
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    ImGui::PopID();
    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(ImVec2(avail, 510 * s + 8 * g_ui));
}

void DrawPicker() {
    if (!g_state.loaded) {
        g_state.figures = toypad::FigureList();
        g_state.loaded = true;
    }
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 screen = io.DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, screen.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(screen.x * 0.86f, screen.y * 0.86f), ImGuiCond_Always);
    ImGui::Begin("Toy Pad", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);

    auto& pad = toypad::Instance();
    const auto slots = pad.Slots();
    const float icon_small = 64 * g_ui;

    DrawBoard(pad, slots, icon_small);
    if (const toypad::Source src = toypad::CurrentSource(); src != toypad::Source::kThisPc)
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1),
                           src == toypad::Source::kPhysical
                               ? "Toy Pad source: the physical Toy Pad (Options > Gameplay). Figures can't be placed from here."
                               : "Toy Pad source: the LegoToypad app (Options > Gameplay). Figures placed here join its figures.");

    // Characters / vehicles (L1 / R1), sort order.
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1) || ImGui::IsKeyPressed(ImGuiKey_GamepadR1)) g_state.kind ^= 1;
    if (ImGui::RadioButton("Characters", g_state.kind == 0)) g_state.kind = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("Vehicles & gadgets", g_state.kind == 1)) g_state.kind = 1;
    ImGui::SameLine(0, 40 * g_ui);
    ImGui::TextUnformatted("Sort:");
    static const char* kSorts[] = {"Default", "Franchise", "A to Z"};
    for (int i = 0; i < 3; ++i) {
        ImGui::SameLine();
        if (ImGui::RadioButton(kSorts[i], g_state.sort == i)) g_state.sort = i;
    }
    ImGui::SameLine(0, 40 * g_ui);
    ImGui::TextDisabled("L1/R1 switch list   Circle closes");

    // The figures.
    std::vector<const toypad::FigureInfo*> shown;
    for (const auto& f : g_state.figures)
        if ((f.id >= 1000) == (g_state.kind == 1)) shown.push_back(&f);
    if (g_state.sort == 2)
        std::stable_sort(shown.begin(), shown.end(), [](auto* a, auto* b) { return a->name < b->name; });
    else if (g_state.sort == 1)
        std::stable_sort(shown.begin(), shown.end(), [](auto* a, auto* b) {
            const std::string fa = a->franchise.empty() ? "~" : a->franchise, fb = b->franchise.empty() ? "~" : b->franchise;
            return fa != fb ? fa < fb : a->id < b->id;
        });
    ImGui::BeginChild("grid", ImVec2(0, 0), ImGuiChildFlags_Borders);
    const float cell = 112 * g_ui, icon = 88 * g_ui;
    const int columns = std::max(1, int(ImGui::GetContentRegionAvail().x / (cell + ImGui::GetStyle().ItemSpacing.x)));
    std::string group;
    int col = 0;
    for (const auto* f : shown) {
        if (g_state.sort == 1 && f->franchise != group) {
            group = f->franchise;
            if (col) ImGui::NewLine();
            col = 0;
            ImGui::SeparatorText(group.empty() ? "Other" : group.c_str());
        }
        if (col) ImGui::SameLine();
        ImGui::PushID(int(f->id));
        ImGui::BeginGroup();
        const float pad_x = (cell - icon) * 0.5f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad_x);
        if (IconButton("fig", f->id, icon)) {
            int count = 0;
            for (const auto& s : pad.Slots()) count += int(s.zone) == g_state.zone;
            if (count < ZoneLimit(g_state.zone)) pad.Place(toypad::Zone(g_state.zone), f->id);
        }
        if (g_state.focus_grid && col == 0 && f == shown.front()) {
            ImGui::SetItemDefaultFocus();
            ImGui::SetKeyboardFocusHere(-1);
            g_state.focus_grid = false;
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cell - pad_x);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() - pad_x);
        ImGui::TextWrapped("%s", f->name.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        ImGui::PopID();
        col = (col + 1) % columns;
    }
    ImGui::EndChild();

    // Circle (or Escape) closes, unless a popup is open; Options closes too.
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false))
        Close();
    ImGui::End();
}

// --- Display Settings ---
struct SettingsState {
    bool focus = false;  // just opened: put the cursor on the first row
    int hovered = 0;     // the row whose help line is shown
    int page = 0;        // 0 Display Settings, 1 Gameplay
} g_settings;

// One row: name on the left, "< value >" on the right. Left / Right (D-pad, stick, arrow keys) or Cross / Enter
// change it while it has the cursor; returns -1, 0 or +1.
int SettingRow(int index, const char* name, const std::string& value) {
    ImGui::PushID(index);
    const float w = ImGui::GetContentRegionAvail().x;
    if (g_settings.focus && index == 0) {
        ImGui::SetKeyboardFocusHere();
        g_settings.focus = false;
    }
    const bool activated = ImGui::Selectable("##row", false, ImGuiSelectableFlags_None, ImVec2(w, 0));
    const bool focused = ImGui::IsItemFocused();
    if (focused) g_settings.hovered = index;
    ImGui::SameLine(ImGui::GetStyle().WindowPadding.x + 12 * g_ui);
    ImGui::TextUnformatted(name);
    const std::string shown = "<  " + value + "  >";
    ImGui::SameLine(w - ImGui::CalcTextSize(shown.c_str()).x);
    ImGui::TextColored(focused ? ImVec4(1.0f, 0.85f, 0.2f, 1) : ImVec4(0.75f, 0.85f, 1.0f, 1), "%s", shown.c_str());
    ImGui::PopID();
    int step = activated ? 1 : 0;
    if (focused) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft))
            step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight))
            step = 1;
    }
    return step;
}

// The settings list (Display Settings or Gameplay, ui/settings_items.h).
void DrawSettings() {
    const auto page = g_settings.page == 1 ? settings::Page::Gameplay : settings::Page::Display;
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 screen = io.DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(screen.x * 0.5f, screen.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(screen.x * 0.9f, 900 * g_ui), 0), ImGuiCond_Always);
    ImGui::Begin(settings::PageTitle(page), nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
    const auto& items = settings::Items(page);
    for (int i = 0; i < int(items.size()); ++i)
        if (const int step = SettingRow(i, items[size_t(i)].name, items[size_t(i)].value()))
            settings::Change(page, i, step);
    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", items[size_t(std::clamp(g_settings.hovered, 0, int(items.size()) - 1))].help);
    ImGui::PopTextWrapPos();
    ImGui::TextDisabled("Left / Right: change    Circle / Esc: back");
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false))
        Close();
    ImGui::End();
}

void InitVulkan(VkFormat format) {
    vkr::Device& dev = vkr::GetDevice();
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpci.maxSets = 16;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    vkCreateDescriptorPool(dev.device(), &dpci, nullptr, &pool);
    static VkFormat color_format;
    color_format = format;
    ImGui_ImplVulkan_InitInfo info{};
    info.Instance = dev.instance();
    info.PhysicalDevice = dev.physical();
    info.Device = dev.device();
    info.QueueFamily = dev.queue_family();
    info.Queue = dev.queue();
    info.DescriptorPool = pool;
    info.MinImageCount = 2;
    info.ImageCount = 3;
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.UseDynamicRendering = true;
    info.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format;
    g_vulkan = ImGui_ImplVulkan_Init(&info);
}

}  // namespace

void Init(SDL_Window* window) {
    std::lock_guard lock(g_mutex);
    g_window = window;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard |
                      ImGuiConfigFlags_NoMouseCursorChange;
    // Size text for the display (1080p = 1x): the window may be fullscreen 4K.
    int count = 0;
    float tallest = 1080;
    if (SDL_DisplayID* ids = SDL_GetDisplays(&count)) {
        for (int i = 0; i < count; ++i)
            if (const SDL_DisplayMode* m = SDL_GetDesktopDisplayMode(ids[i])) tallest = std::max(tallest, float(m->h));
        SDL_free(ids);
    }
    g_scale = tallest / 1080.0f;
    ImFontConfig fc;
    fc.SizePixels = 26 * g_scale;
    const char* font = "C:\\Windows\\Fonts\\segoeuib.ttf";
    if (std::ifstream(font).good()) io.Fonts->AddFontFromFileTTF(font, 26 * g_scale);
    else io.Fonts->AddFontDefault(&fc);
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 14;
    style.ChildRounding = 10;
    style.FrameRounding = 10;
    style.WindowPadding = ImVec2(18, 18);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.07f, 0.14f, 0.94f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.12f, 0.22f, 0.9f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.15f, 0.17f, 0.32f, 1);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.45f, 0.70f, 1);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.31f, 0.76f, 1.0f, 1);
    style.Colors[ImGuiCol_NavHighlight] = ImVec4(1.0f, 0.85f, 0.2f, 1);
    g_base_style = style;
    ImGui_ImplSDL3_InitForVulkan(window);
    g_ui = 0;  // set on the first frame
    g_context = true;
}

bool ProcessEvent(const SDL_Event& e) {
    if (!g_open || !g_context) return false;
    std::lock_guard lock(g_mutex);
    ImGui_ImplSDL3_ProcessEvent(&e);
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT:
        case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_WHEEL:
            return true;
        default:
            return false;
    }
}

void OpenGameplaySettings() {
    OpenSettings();
    g_settings.page = 1;
}

void OpenSettings() {
    std::fprintf(stderr, "[overlay] settings opened\n");
    g_settings.focus = true;
    g_settings.hovered = 0;
    g_settings.page = 0;
    g_state.fresh = true;
    g_panel = Panel::Settings;
    g_open = true;
}

void OpenPicker() {
    std::fprintf(stderr, "[overlay] picker opened\n");
    g_panel = Panel::Picker;
    g_state.focus_grid = true;
    g_state.fresh = true;
    g_open = true;
}
bool PickerOpen() { return g_open; }
bool BlocksGameInput() { return g_open || NowMs() - g_closed_at_ms.load() < 400; }

void Record(VkCommandBuffer cmd, VkImageView view, VkExtent2D extent, VkFormat format) {
    std::lock_guard lock(g_mutex);
    if (!g_context) return;
    if (!g_vulkan) InitVulkan(format);
    if (!g_vulkan) return;
    // The figure files may be made after the picker first opened (a fresh install's first minute): when they are,
    // take the new list, and the atlas if there wasn't one.
    if (const int gen = icons::FiguresGeneration(); gen != g_atlas.generation) {
        g_state.loaded = false;
        if (g_atlas.image == VK_NULL_HANDLE) g_atlas.tried = false;
        else g_atlas.generation = gen;
    }
    if (!g_atlas.tried && g_panel == Panel::Picker) LoadAtlas(cmd);
    // Keys released while the picker was closed never reached ImGui (it only gets events while open):
    // start each opening with nothing held, or a held Escape/Circle would close it again at once.
    if (g_state.fresh) {
        ImGui::GetIO().ClearInputKeys();
        g_state.fresh = false;
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::GetIO().DisplaySize = ImVec2(float(extent.width), float(extent.height));
    // Size everything for this window (1080p = 1x); the font was built for the largest display.
    const float ui = std::max(0.5f, float(extent.height) / 1080.0f);
    if (ui != g_ui) {
        g_ui = ui;
        ImGui::GetStyle() = g_base_style;
        ImGui::GetStyle().ScaleAllSizes(g_ui);
        ImGui::GetIO().FontGlobalScale = g_ui / g_scale;
    }
    ImGui::NewFrame();
    if (g_panel == Panel::Settings) DrawSettings();
    else DrawPicker();
    ImGui::Render();
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cmd, &ri);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    vkCmdEndRendering(cmd);
}

}  // namespace ui
