// The port's settings inside the game's own Options menu.
//
// The PS4 Options page (FUN_01cbb3a0 sets it up, FUN_01cbb920 handles Cross) has three PC-only rows that
// it hides on PS4: Options_Video (+0xe8), Options_Effects (+0xf0) and Options_Controls (+0xf8). Their
// Cross handlers open PC pages that don't exist in the PS4 frontend. The Video row is reused as "Display
// Settings" and the Effects row as "Gameplay"; each opens one of the port's settings lists (ui::OpenSettings /
// ui::OpenGameplaySettings, drawn over the game):
// - setup: the three "hide row" calls become one call to OptionsInit, which shows and labels those two rows
//   and keeps the Controls row hidden;
// - Cross on the Video / Effects row: the body becomes a call to OptionsSelect(page, 0 / 1).
// Labels are set the way the game sets "Music: On": the row's innermost child text element
// (FUN_01b74cf0(e, 0) until none) gets UTF-8 text through FUN_01b816a0.
#include "hle/options_menu.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include "host/memory.h"
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "hle/hle.h"
#include "hle/videoout.h"
#include "ui/overlay.h"
#include "ui/settings_items.h"

namespace port {
namespace {

uint8_t* g_base = nullptr;

// --- the in-game (pause) Options page ---
// An older text-menu page: FUN_02956930 lays out its rows every frame, each with FUN_01b51d20(menu, text,
// alpha); the menu counts rows at +0x9c and keeps its cursor at +0x0a (short). The row-add function is
// hooked: just before the page's last row (Back, added from 0x1957594) the hook adds "Display Settings" and
// "Gameplay".
using AddRowFn = SYSV void (*)(uint64_t menu, const char* text, uint32_t alpha);
constexpr uint64_t kAddRow = 0xb51d20;
constexpr size_t kAddRowPrologue = 13;  // push rbp; mov rbp, rsp; push r15..r12; push rbx
constexpr uint64_t kBackRowReturn = 0x1957599;
AddRowFn g_add_row_original = nullptr;
const char g_ingame_label[] = "Display Settings";
const char g_ingame_gameplay_label[] = "Gameplay";
std::atomic<uint64_t> g_row_selected_flip{0};       // flip count when Display Settings was last laid out highlighted
std::atomic<uint64_t> g_gameplay_selected_flip{0};  // the same for Gameplay (the row after it)
std::atomic<uint64_t> g_menu{0}, g_menu_flip{0};  // the page's menu and when it was last laid out
std::atomic<int> g_row_index{-1};                 // our first row's index (the game's Back keeps its own: this one)
constexpr int kInGameRows = 2;                     // Display Settings, Gameplay

// --- the port's settings as the game's own rows ---
// Choosing Display Settings or Gameplay turns the Options page itself into that settings list: on the pause
// page the game's rows are left out and the settings (plus Back) are laid out in their place, with the game's
// help line under them; on the title page the page's own row widgets are relabelled (and any left over
// hidden), then put back on the way out. Up / Down move the game's cursor as usual; Left / Right / Cross
// change the highlighted setting and Circle (or Back) returns, all handled in MenuPadInput and kept from the
// game.
enum NativeWhere : int { kNone = 0, kPause = 1, kTitle = 2 };
std::atomic<int> g_native{kNone};
std::atomic<int> g_native_page{0};  // ui::settings::Page
ui::settings::Page NativePage() { return ui::settings::Page(g_native_page.load()); }

// The pause page's layout (FUN_02956930) runs from here to just past its Back row; its help line for the
// highlighted row is FUN_029576b0(scale, text, menu), called with the float at kHelpScale.
constexpr uint64_t kLayoutStart = 0x1956930, kLayoutEnd = 0x19575a0;
using HelpFn = SYSV void (*)(float scale, const char* text, uint64_t menu);
constexpr uint64_t kHelp = 0x19576b0, kHelpScale = 0x24f53a0;
constexpr size_t kHelpPrologue = 14;  // push rbp; mov rbp, rsp; push r14; push rbx; sub rsp, 0x30; mov rbx, rsi
HelpFn g_help_original = nullptr;
bool g_our_help = false;  // our own help call is under way (the hook lets it through)
std::atomic<int> g_pause_return_cursor{0};
const char g_back_label[] = "Back";

bool InLayout(uint64_t ret) {
    const uint64_t off = ret - reinterpret_cast<uint64_t>(g_base);
    return off >= kLayoutStart && off < kLayoutEnd;
}

// --- Brightness and Motion Blur, moved into Display Settings ---
// The pause page's rows "Brightness: %d" and "Motion Blur: On/Off" come from the game's options
// (FUN_029946a0(DAT_04174480) -> pointer to them): brightness is the int at +0x10 (0-100, applied by
// FUN_01696700(value, options +0x14)), motion blur the byte at +4 (also bit 0x2000 of the render flags at
// *DAT_03fec908 + 0x2a30). The layout leaves both rows out (their labels are the game's texts 0xbb and 0xd1); the
// page's update (FUN_02955670), which finds its rows by counting the cursor (+0x40) through them in order, sees
// the cursor moved past the two missing rows.
constexpr uint64_t kOptionsOwner = 0x3174480, kGetOptions = 0x19946a0, kApplyBrightness = 0x696700,
                   kRenderGlobals = 0x2fec908, kTextTable = 0x3172370, kTextCount = 0x3172380;
using GetOptionsFn = SYSV uint64_t* (*)(uint64_t owner);
using ApplyBrightnessFn = SYSV void (*)(int value, uint32_t extra);

uint8_t* GameOptions() {
    if (!g_base) return nullptr;
    const uint64_t owner = *reinterpret_cast<const uint64_t*>(g_base + kOptionsOwner);
    if (!owner) return nullptr;
    const uint64_t* p = reinterpret_cast<GetOptionsFn>(g_base + kGetOptions)(owner);
    return p && *p ? reinterpret_cast<uint8_t*>(*p) : nullptr;
}

// The game's text number `index` (as the layout reads it), or null.
const char* GameText(uint32_t index) {
    if (!g_base || *reinterpret_cast<const uint32_t*>(g_base + kTextCount) <= index) return nullptr;
    const uint64_t table = *reinterpret_cast<const uint64_t*>(g_base + kTextTable);
    return table ? *reinterpret_cast<const char* const*>(table + 8ull * index) : nullptr;
}

// "<label>: ..." for one of the moved rows.
bool IsMovedRow(const char* text) {
    if (!text) return false;
    for (uint32_t index : {0xbbu, 0xd1u})
        if (const char* label = GameText(index); label && *label) {
            const size_t n = std::strlen(label);
            if (std::strncmp(text, label, n) == 0 && text[n] == ':') return true;
        }
    return false;
}

std::atomic<int> g_moved_at{-1};          // the visual index the moved rows had in the last layout, -1 none
std::atomic<uint64_t> g_moved_flip{0};    // when that layout ran
std::atomic<int> g_moved_n{0};            // how many rows were left out there (they are next to each other)
int g_moved_pending = -1, g_moved_pending_n = 0;  // during a layout pass

// The page's update with the cursor as the game counts its rows: past the moved rows it is that many further on.
using UpdateFn = SYSV void (*)(uint64_t page);
constexpr uint64_t kUpdate = 0x1955670;
constexpr size_t kUpdatePrologue = 13;  // push rbp; mov rbp, rsp; push r15..r12; push rbx
UpdateFn g_update_original = nullptr;

SYSV void UpdateHook(uint64_t page) {
    auto* cursor = reinterpret_cast<int32_t*>(page + 0x40);
    const int at = g_moved_at.load(), n = g_moved_n.load();
    const bool map = g_native.load() == kNone && at >= 0 && n > 0 && g_moved_flip.load() + 2 >= hle::VideoOutFlipCount() &&
                     *cursor >= at;
    if (map) *cursor += n;
    g_update_original(page);
    if (map && *cursor >= at + n) *cursor -= n;
}

}  // namespace

int GameBrightness() {
    const uint8_t* o = GameOptions();
    return o ? *reinterpret_cast<const int32_t*>(o + 0x10) : -1;
}

void ChangeGameBrightness(int step) {
    uint8_t* o = GameOptions();
    if (!o || !step) return;
    const int v = std::clamp(*reinterpret_cast<int32_t*>(o + 0x10) + step * 5, 0, 100);
    *reinterpret_cast<int32_t*>(o + 0x10) = v;
    reinterpret_cast<ApplyBrightnessFn>(g_base + kApplyBrightness)(v, *reinterpret_cast<uint32_t*>(o + 0x14));
}

int GameMotionBlur() {
    const uint8_t* o = GameOptions();
    return o ? (o[4] ? 1 : 0) : -1;
}

void ToggleGameMotionBlur() {
    uint8_t* o = GameOptions();
    if (!o) return;
    o[4] = o[4] ? 0 : 1;
    if (const uint64_t globals = *reinterpret_cast<const uint64_t*>(g_base + kRenderGlobals)) {
        auto* flags = reinterpret_cast<uint64_t*>(globals + 0x2a30);
        *flags = o[4] ? (*flags | 0x2000) : (*flags & ~uint64_t(0x2000));
    }
}

namespace {

// The pause page in native mode: the settings rows, Back, and the highlighted setting's help line.
void NativePauseRows(uint64_t menu, uint32_t alpha) {
    const auto page = NativePage();
    const auto& items = ui::settings::Items(page);
    const float y = *reinterpret_cast<float*>(menu + 0xa8);  // the help line is placed from the first row
    for (int i = 0; i < int(items.size()); ++i) g_add_row_original(menu, ui::settings::Label(page, i).c_str(), alpha);
    g_add_row_original(menu, g_back_label, alpha);
    const int cursor = *reinterpret_cast<const int16_t*>(menu + 0x0a);
    if (g_help_original && cursor >= 0 && cursor < int(items.size())) {
        float scale;
        std::memcpy(&scale, g_base + kHelpScale, 4);
        const float saved = *reinterpret_cast<float*>(menu + 0xa8);
        *reinterpret_cast<float*>(menu + 0xa8) = y;
        g_our_help = true;
        g_help_original(scale, items[size_t(cursor)].short_help, menu);
        g_our_help = false;
        *reinterpret_cast<float*>(menu + 0xa8) = saved;
    }
}

// The game's help lines for its own rows stay out while the page shows the port's settings.
SYSV void HelpHook(float scale, const char* text, uint64_t menu) {
    if (g_native.load() == kPause && !g_our_help && InLayout(reinterpret_cast<uint64_t>(__builtin_return_address(0))))
        return;
    g_help_original(scale, text, menu);
}

SYSV void AddRowHook(uint64_t menu, const char* text, uint32_t alpha) {
    const bool in_layout = InLayout(reinterpret_cast<uint64_t>(__builtin_return_address(0)));
    if (g_native.load() == kPause && in_layout) {
        g_menu = menu;
        g_menu_flip = hle::VideoOutFlipCount() + 1;
        if (*reinterpret_cast<const int*>(menu + 0x9c) == 0) NativePauseRows(menu, alpha);  // once per layout
        return;
    }
    if (in_layout) {
        const int index = *reinterpret_cast<const int*>(menu + 0x9c);
        if (index == 0) {  // a new layout pass
            g_moved_at = g_moved_pending;
            g_moved_n = g_moved_pending_n;
            g_moved_pending = -1;
            g_moved_pending_n = 0;
        }
        if (g_update_original && IsMovedRow(text)) {  // Brightness / Motion Blur: in Display Settings now
            if (g_moved_pending < 0) g_moved_pending = index;
            ++g_moved_pending_n;
            g_moved_at = g_moved_pending;
            g_moved_n = std::max(g_moved_n.load(), g_moved_pending_n);
            g_moved_flip = hle::VideoOutFlipCount() + 1;
            static bool logged = false;
            if (!logged) std::fprintf(stderr, "[options] Brightness and Motion Blur rows moved to Display Settings\n");
            logged = true;
            return;
        }
    }
    if (reinterpret_cast<uint64_t>(__builtin_return_address(0)) == reinterpret_cast<uint64_t>(g_base) + kBackRowReturn) {
        const int index = *reinterpret_cast<const int*>(menu + 0x9c);  // the row about to be added
        const int cursor = *reinterpret_cast<const int16_t*>(menu + 0x0a);
        if (cursor == index) g_row_selected_flip = hle::VideoOutFlipCount() + 1;
        if (cursor == index + 1) g_gameplay_selected_flip = hle::VideoOutFlipCount() + 1;
        g_menu = menu;
        g_row_index = index;
        g_menu_flip = hle::VideoOutFlipCount() + 1;
        g_add_row_original(menu, g_ingame_label, alpha);
        g_add_row_original(menu, g_ingame_gameplay_label, alpha);
    }
    g_add_row_original(menu, text, alpha);
}

// Game functions (image offsets).
using ChildFn = SYSV uint64_t (*)(uint64_t element, int index);
using SetTextFn = SYSV void (*)(uint64_t element, const char* utf8);
constexpr uint64_t kChild = 0xb74cf0, kSetText = 0xb816a0;

// The reused rows (Options_Video +0xe8, Options_Effects +0xf0), their text keys, and the PC row that stays
// hidden. The text element keeps a pointer to the string it was given (FUN_01b816a0 stores the text lookup's
// result, which for a non-key string is the string itself), so the labels live here for good.
struct ReusedRow { uint32_t offset; const char* key; const char* label; };
const ReusedRow kRows[] = {
    {0xe8, "PC_SETTINGS_VIDEO", "Display Settings"},
    {0xf0, "PC_SETTINGS_EFFECTS", "Gameplay"},
};
constexpr uint32_t kHiddenRows[] = {0xf8};  // Options_Controls

// Text lookup (FUN_014f1d60: key -> display text) hook: the row's key gives the port's label, so every
// refresh of the page shows it. Everything else goes to the original (via a trampoline).
using LookupFn = SYSV const char* (*)(const char* key, int arg);
constexpr uint64_t kLookup = 0x4f1d60;
constexpr size_t kLookupPrologue = 14;  // push rbp ... mov r14, rsi: position-independent
LookupFn g_lookup_original = nullptr;

SYSV const char* LookupHook(const char* key, int arg) {
    static const bool log_keys = std::getenv("LDPORT_LOG_TEXT_KEYS") != nullptr;  // finding a row's text key
    if (log_keys && key)
        std::fprintf(stderr, "[text] %s (from %llx)\n", key,
                     (unsigned long long)(reinterpret_cast<uint64_t>(__builtin_return_address(0)) - reinterpret_cast<uint64_t>(g_base)));
    if (key)
        for (const ReusedRow& r : kRows)
            if (std::strcmp(key, r.key) == 0) return r.label;
    return g_lookup_original(key, arg);
}

// Text setter hook (FUN_01b816a0(element, key or text)): remembers what the game last gave each element, so a
// row the settings list borrowed gets exactly that back (looking up the shown text again doesn't work: the
// lookup turns most plain text into " "). Our own calls go through untouched.
constexpr size_t kSetTextPrologue = 13;  // push rbp; mov rbp, rsp; push r14; push rbx; mov rbx, rsi; mov r14, rdi
SetTextFn g_set_text_original = nullptr;
bool g_our_set_text = false;
std::unordered_map<uint64_t, const char*> g_game_text;  // element -> copy of its last text (kept for good)

SYSV void SetTextHook(uint64_t element, const char* text) {
    if (!g_our_set_text && text) {
        const size_t n = std::strlen(text) + 1;
        char* copy = new char[n];
        std::memcpy(copy, text, n);
        g_game_text[element] = copy;  // the old copy stays: the element may still point at it
    }
    g_set_text_original(element, text);
}

void SetRowText(uint64_t row, const char* label) {
    if (!row || !g_base) return;
    auto child = reinterpret_cast<ChildFn>(g_base + kChild);
    auto set_text = [](uint64_t e, const char* t) {
        g_our_set_text = true;
        (g_set_text_original ? g_set_text_original : reinterpret_cast<SetTextFn>(g_base + kSetText))(e, t);
        g_our_set_text = false;
    };
    // Row -> child 0 -> child 0 is the label, as for the game's own rows (FUN_01cbced0).
    uint64_t e = child(row, 0);
    if (!e) return;
    if (const uint64_t c = child(e, 0)) e = c;
    set_text(e, label);
}

SYSV void OptionsInit(uint64_t page) {
    for (uint32_t off : kHiddenRows)
        if (const uint64_t r = *reinterpret_cast<uint64_t*>(page + off)) *reinterpret_cast<uint8_t*>(r + 0x229) = 0;
    for (const ReusedRow& row : kRows)
        if (const uint64_t r = *reinterpret_cast<uint64_t*>(page + row.offset)) {
            *reinterpret_cast<uint8_t*>(r + 0x229) = 1;  // visible
            SetRowText(r, row.label);
        }
}

// --- the title Options page in native mode ---
// Its rows are list items set up by FUN_01cbb3a0; +0x229 on a row is "shown". The ones a settings list can
// borrow, in screen order: Music (+0xd8), Sound Effects (+0xe0), Subtitles (+0xd0), the PC Controls row (+0xf8,
// normally hidden; it sits above Display Settings), Display Settings (+0xe8), Gameplay (+0xf0), Show Game Controls
// (+0x108) and Credits (+0x100: an empty slot above Add-on Content on PS4, used for an 8th setting). (+0x110 is
// the hidden Safe Zone row.) The focused row is FUN_01b7fef0(page->+0x68).
constexpr uint32_t kTitleRows[] = {0xd8, 0xe0, 0xd0, 0xf8, 0xe8, 0xf0, 0x108, 0x100};
constexpr size_t kTitleRowCount = sizeof kTitleRows / sizeof kTitleRows[0];
using FocusFn = SYSV uint64_t (*)(uint64_t container);
constexpr uint64_t kFocus = 0xb7fef0;
uint64_t g_title_page = 0;
struct TitleRow {
    uint64_t row = 0;
    bool was_shown = false;
    int item = -1;        // the setting it shows now, -1 none (hidden while the list is up)
    uint8_t draw_flags = 0;  // its +0x22d before the list borrowed it
    std::string text[2];  // our labels, alternating (the element keeps a pointer to the last one)
    int text_slot = 0;
};
TitleRow g_title_rows[kTitleRowCount];

uint64_t LabelElement(uint64_t row) {
    auto child = reinterpret_cast<ChildFn>(g_base + kChild);
    uint64_t e = child(row, 0);
    if (e) {
        if (const uint64_t c = child(e, 0)) e = c;
    }
    return e;
}

uint64_t TitleFocused() {
    if (!g_title_page) return 0;
    return reinterpret_cast<FocusFn>(g_base + kFocus)(*reinterpret_cast<uint64_t*>(g_title_page + 0x68));
}

void RelabelTitle() {
    const auto page = NativePage();
    for (TitleRow& r : g_title_rows) {
        if (r.item < 0) continue;
        r.text_slot ^= 1;
        r.text[r.text_slot] = ui::settings::Label(page, r.item);
        SetRowText(r.row, r.text[r.text_slot].c_str());
    }
}

void EnterTitle(uint64_t page, ui::settings::Page which) {
    const int n = int(ui::settings::Items(which).size());
    g_title_page = page;
    g_native_page = int(which);
    const uint64_t focused = TitleFocused();
    // The rows on screen, in order, and which one has the cursor.
    std::vector<size_t> shown;
    int focus_pos = 0;
    for (size_t i = 0; i < kTitleRowCount; ++i) {
        TitleRow& r = g_title_rows[i];
        r.row = *reinterpret_cast<uint64_t*>(page + kTitleRows[i]);
        r.item = -1;
        r.was_shown = r.row && *reinterpret_cast<uint8_t*>(r.row + 0x229);
        if (r.was_shown) {
            if (r.row == focused) focus_pos = int(shown.size());
            shown.push_back(i);
        }
    }
    // Rows for the settings: the slots from the top in screen order (the hidden Controls row and the empty
    // Credits slot come back for them), so the list reads in order; the cursor stays on its row.
    (void)focus_pos;
    std::vector<size_t> use;
    for (size_t i = 0; i < kTitleRowCount && int(use.size()) < n; ++i)
        if (g_title_rows[i].row) use.push_back(i);
    int item = 0;
    for (size_t i : use) g_title_rows[i].item = item++;
    for (TitleRow& r : g_title_rows) {
        if (!r.row) continue;
        *reinterpret_cast<uint8_t*>(r.row + 0x229) = r.item >= 0 ? 1 : 0;
        // +0x22d bit 0x02: drawn. Every row has it but Credits (0x08 on PS4), which a list can borrow.
        auto* flags = reinterpret_cast<uint8_t*>(r.row + 0x22d);
        r.draw_flags = *flags;
        if (r.item >= 0) *flags |= 0x02;
    }
    if (std::getenv("LDPORT_LOG_TITLE_ROWS"))  // finding why a row isn't drawn
        for (size_t i = 0; i < kTitleRowCount; ++i) {
            const auto* p = reinterpret_cast<const uint8_t*>(g_title_rows[i].row);
            if (!p) continue;
            std::fprintf(stderr, "[options] row +%#x:", kTitleRows[i]);
            for (int k = 0x1e0; k < 0x240; ++k) std::fprintf(stderr, "%s%02x", k % 16 ? "" : " ", p[k]);
            std::fprintf(stderr, "\n");
        }
    g_native = kTitle;
    RelabelTitle();
    std::fprintf(stderr, "[options] %s (title page, %d rows)\n", ui::settings::PageTitle(which), n);
}

void ExitTitle() {
    g_native = kNone;
    for (TitleRow& r : g_title_rows) {
        if (!r.row) continue;
        *reinterpret_cast<uint8_t*>(r.row + 0x229) = r.was_shown ? 1 : 0;
        *reinterpret_cast<uint8_t*>(r.row + 0x22d) = r.draw_flags;
        if (r.item >= 0) {
            // What the game last gave the label (a text key or its own text), else our row names.
            const uint64_t e = LabelElement(r.row);
            const auto it = g_game_text.find(e);
            const char* text = it != g_game_text.end() ? it->second : nullptr;
            for (const ReusedRow& reused : kRows)
                if (r.row == *reinterpret_cast<uint64_t*>(g_title_page + reused.offset) && !text) text = reused.label;
            // Rows the page labels by text key through another path (FUN_01b86f..), which the setter hook doesn't
            // see: their keys (the setter takes a key as well as text).
            static constexpr struct { uint32_t offset; const char* key; } kKeys[] = {
                {0x108, "SHOW_CONTROLS"}, {0x100, "CREDITS"}, {0xf8, "PC_CONTROL_CONFIG"}};
            for (const auto& k : kKeys)
                if (!text && r.row == *reinterpret_cast<uint64_t*>(g_title_page + k.offset)) text = k.key;
            if (std::getenv("LDPORT_LOG_TITLE_ROWS"))
                std::fprintf(stderr, "[options] restore row %llx element %llx: %s\n", (unsigned long long)r.row,
                             (unsigned long long)e, text ? text : "(unknown)");
            if (text) SetRowText(r.row, text);
        }
        r.item = -1;
    }
    std::fprintf(stderr, "[options] back to the title Options page\n");
}

// Cross on the Display Settings / Gameplay row of the title Options page.
SYSV void OptionsSelect(uint64_t page, int row) {
    EnterTitle(page, row == 1 ? ui::settings::Page::Gameplay : ui::settings::Page::Display);
}

// Writes `bytes` at `offset` if the bytes there start with `expect`.
bool Write(uint64_t offset, const std::vector<uint8_t>& expect, const std::vector<uint8_t>& bytes, size_t span,
           const char* what) {
    uint8_t* at = g_base + offset;
    if (std::memcmp(at, expect.data(), expect.size()) != 0) {
        std::fprintf(stderr, "[patch] %s: bytes differ at %llx, not applied\n", what, (unsigned long long)offset);
        return false;
    }
    std::memset(at, 0x90, span);  // NOP the whole replaced body
    std::memcpy(at, bytes.data(), bytes.size());
    std::fprintf(stderr, "[patch] %s\n", what);
    return true;
}

void Put64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
void Put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}

}  // namespace

// Pause menu (FUN_02959190 draws it each frame, FUN_02959880 acts on the chosen row): its "Show Game
// Controls" row becomes "Toy Pad", which opens the port's figure picker. The Options page keeps its own
// Show Game Controls row.
const char g_toypad_label[] = "Toy Pad";
SYSV void PauseToyPad() {
    std::fprintf(stderr, "[options] pause menu: Toy Pad\n");
    ui::OpenPicker();
}

void InstallPauseMenu() {
    // Label: LEA RAX,[DAT_0412ca38]; MOV R12,[RAX] (10 bytes) -> movabs r12, "Toy Pad".
    {
        std::vector<uint8_t> b = {0x49, 0xbc};
        Put64(b, reinterpret_cast<uint64_t>(g_toypad_label));
        Write(0x19593aa, {0x48, 0x8d, 0x05, 0x87, 0x36, 0x7d, 0x01, 0x4c, 0x8b, 0x20}, b, 10,
              "Pause menu: Show Game Controls row is now Toy Pad");
    }
    // Choosing it: the FUN_01cba2b0(...) call and its argument setup (24 bytes, before `jmp end`)
    // -> movabs rax, PauseToyPad; call rax.
    {
        std::vector<uint8_t> b = {0x48, 0xb8};
        Put64(b, reinterpret_cast<uint64_t>(&PauseToyPad));
        b.insert(b.end(), {0xff, 0xd0});
        Write(0x1959a94, {0x48, 0x8d, 0x05, 0xa5, 0x4a, 0x7d, 0x01, 0x48, 0x8b, 0x38}, b, 0x1959aac - 0x1959a94,
              "Pause menu: Toy Pad opens the figure picker");
    }
}

namespace {

void InGameOptionsCross() {
    // The page's Cross handler knows Back by its old index, which our first row now has: with the cursor on
    // Back (below our rows), point it at that index for the game to act on.
    const uint64_t menu = g_menu.load();
    if (!menu || g_menu_flip.load() < hle::VideoOutFlipCount()) return;
    auto* cursor = reinterpret_cast<int16_t*>(menu + 0x0a);
    if (*cursor == g_row_index.load() + kInGameRows) *cursor = int16_t(g_row_index.load());
}

bool Fresh(const std::atomic<uint64_t>& flip) {
    const uint64_t f = flip.load();
    return f && f >= hle::VideoOutFlipCount();
}

// scePad button bits (libscepad.cpp).
constexpr uint32_t kOptions = 0x8, kRight = 0x20, kLeft = 0x80, kL2 = 0x100, kR2 = 0x200, kL1 = 0x400, kR1 = 0x800,
                   kTriangle = 0x1000, kCircle = 0x2000, kCross = 0x4000, kSquare = 0x8000, kTouchPad = 0x100000;
// While a settings page is shown the game only gets Up / Down (its cursor); everything else is ours or held back.
constexpr uint32_t kHeldBack = kOptions | kRight | kLeft | kL2 | kR2 | kL1 | kR1 | kTriangle | kCircle | kCross |
                               kSquare | kTouchPad;

void EnterPause(ui::settings::Page which) {
    const uint64_t menu = g_menu.load();
    if (!menu) return;
    auto* cursor = reinterpret_cast<int16_t*>(menu + 0x0a);
    g_pause_return_cursor = *cursor;
    g_native_page = int(which);
    g_native = kPause;
    *cursor = 0;
    std::fprintf(stderr, "[options] %s (pause page)\n", ui::settings::PageTitle(which));
}

void ExitPause() {
    g_native = kNone;
    if (const uint64_t menu = g_menu.load())
        *reinterpret_cast<int16_t*>(menu + 0x0a) = int16_t(g_pause_return_cursor.load());
    std::fprintf(stderr, "[options] back to the pause Options page\n");
}

void ExitNative() {
    if (g_native.load() == kPause) ExitPause();
    else if (g_native.load() == kTitle) ExitTitle();
}

// The highlighted setting (-1: none; on the pause page, the Back row is items.size()).
int NativeCursor() {
    if (g_native.load() == kPause) {
        const uint64_t menu = g_menu.load();
        return menu ? *reinterpret_cast<const int16_t*>(menu + 0x0a) : -1;
    }
    const uint64_t focused = TitleFocused();
    for (const TitleRow& r : g_title_rows)
        if (r.item >= 0 && r.row == focused) return r.item;
    return -1;
}

}  // namespace

void MenuPadInput(int slot, uint32_t& buttons, uint8_t& left_x) {
    if (slot < 0 || slot >= 4) return;
    static uint32_t was[4] = {};       // last buttons seen, per pad (Left / Right include the stick)
    static uint64_t repeat_at[4] = {};  // flip count of the next Left / Right repeat while held
    static uint32_t hold_back[4] = {};  // buttons that closed a settings page: kept from the game until let go
    uint32_t now = buttons;
    if (left_x < 0x40) now |= kLeft;
    if (left_x > 0xc0) now |= kRight;
    const uint32_t pressed = now & ~was[slot];
    const uint64_t flip = hle::VideoOutFlipCount();
    hold_back[slot] &= now;
    buttons &= ~hold_back[slot];

    if (g_native.load() == kNone) {
        // The pause Options page's Display Settings / Gameplay rows: Cross on one shows that page.
        if (pressed & kCross) {
            if (Fresh(g_row_selected_flip)) EnterPause(ui::settings::Page::Display);
            else if (Fresh(g_gameplay_selected_flip)) EnterPause(ui::settings::Page::Gameplay);
            else InGameOptionsCross();
            if (g_native.load() != kNone) {
                buttons &= ~kCross;
                for (uint32_t& w : was) w = ~0u;  // nothing acts until it's let go
                return;
            }
        }
        was[slot] = now;
        return;
    }
    // The pause page went away (the pause menu closed some other way): leave the settings page.
    if (g_native.load() == kPause && g_menu_flip.load() + 30 < flip) {
        g_native = kNone;
        was[slot] = now;
        return;
    }
    const auto page = NativePage();
    const int count = int(ui::settings::Items(page).size());
    const int cursor = NativeCursor();
    int step = 0;
    if (pressed & kRight) step = 1, repeat_at[slot] = flip + 24;
    else if (pressed & kLeft) step = -1, repeat_at[slot] = flip + 24;
    else if ((now & (kLeft | kRight)) && flip >= repeat_at[slot]) {
        step = (now & kRight) ? 1 : -1;
        repeat_at[slot] = flip + 8;
    }
    if (pressed & kCircle) {
        ExitNative();
    } else if (pressed & kCross) {
        if (cursor == count) ExitNative();  // Back
        else if (cursor >= 0 && cursor < count) step = 1;
    }
    if (step && cursor >= 0 && cursor < count && g_native.load() != kNone) {
        ui::settings::Change(page, cursor, step);
        if (g_native.load() == kTitle) {
            // A setting can add or remove a row under it (LAN Toy Pad's address): lay the page out again.
            if (int(ui::settings::Items(page).size()) != count) {
                const uint64_t title_page = g_title_page;
                ExitTitle();
                EnterTitle(title_page, page);
            } else {
                RelabelTitle();
            }
        }
    }
    if (g_native.load() == kNone)  // just closed: the closing button stays ours on every pad until let go
        for (uint32_t& h : hold_back) h |= kCircle | kCross;
    was[slot] = now;
    buttons &= ~kHeldBack;
    left_x = 0x80;
}

// A small executable block for a hook's trampoline.
static uint8_t* ExecPage() {
    uint8_t* p = host::AllocateImage(0x1000);
    host::SetProtection(p, 0x1000, host::kRead | host::kWrite | host::kExec);
    return p;
}

void InstallOptionsMenu(uint8_t* base) {
    g_base = base;
    InstallPauseMenu();
    // In-game Options: hook the row-add function (FUN_01b51d20) through a trampoline of its 13-byte prologue.
    {
        static const uint8_t kPrologue[kAddRowPrologue] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                                           0x56, 0x41, 0x55, 0x41, 0x54, 0x53};
        uint8_t* at = base + kAddRow;
        if (std::memcmp(at, kPrologue, kAddRowPrologue) == 0) {
            auto* tramp = ExecPage();
            std::vector<uint8_t> t(kPrologue, kPrologue + kAddRowPrologue);
            t.insert(t.end(), {0x48, 0xb8});
            Put64(t, reinterpret_cast<uint64_t>(at + kAddRowPrologue));
            t.insert(t.end(), {0xff, 0xe0});  // jmp rax
            std::memcpy(tramp, t.data(), t.size());
            g_add_row_original = reinterpret_cast<AddRowFn>(tramp);
            std::vector<uint8_t> h = {0x48, 0xb8};
            Put64(h, reinterpret_cast<uint64_t>(&AddRowHook));
            h.insert(h.end(), {0xff, 0xe0, 0x90});  // movabs rax, hook; jmp rax; nop (13 bytes)
            std::memcpy(at, h.data(), h.size());
            std::fprintf(stderr, "[patch] in-game Options: Display Settings and Gameplay rows\n");
        } else {
            std::fprintf(stderr, "[patch] in-game Options: bytes differ, not hooked\n");
        }
    }
    // Hook the pause page's update (FUN_02955670) the same way: the cursor skips the rows moved to Display Settings.
    {
        static const uint8_t kPrologue[kUpdatePrologue] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                                           0x56, 0x41, 0x55, 0x41, 0x54, 0x53};
        uint8_t* at = base + kUpdate;
        if (std::memcmp(at, kPrologue, kUpdatePrologue) == 0) {
            auto* tramp = ExecPage();
            std::vector<uint8_t> t(kPrologue, kPrologue + kUpdatePrologue);
            t.insert(t.end(), {0x48, 0xb8});
            Put64(t, reinterpret_cast<uint64_t>(at + kUpdatePrologue));
            t.insert(t.end(), {0xff, 0xe0});  // jmp rax
            std::memcpy(tramp, t.data(), t.size());
            g_update_original = reinterpret_cast<UpdateFn>(tramp);
            std::vector<uint8_t> h = {0x48, 0xb8};
            Put64(h, reinterpret_cast<uint64_t>(&UpdateHook));
            h.insert(h.end(), {0xff, 0xe0, 0x90});  // movabs rax, hook; jmp rax; nop (13 bytes)
            std::memcpy(at, h.data(), h.size());
            std::fprintf(stderr, "[patch] in-game Options: update hooked (Brightness and Motion Blur moved)\n");
        } else {
            std::fprintf(stderr, "[patch] in-game Options update: bytes differ, not hooked\n");
        }
    }
    // Hook the pause page's help line (FUN_029576b0) the same way: the game's own help stays out while the page
    // shows the port's settings.
    {
        static const uint8_t kPrologue[kHelpPrologue] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53,
                                                         0x48, 0x83, 0xec, 0x30, 0x48, 0x89, 0xf3};
        uint8_t* at = base + kHelp;
        if (std::memcmp(at, kPrologue, kHelpPrologue) == 0) {
            auto* tramp = ExecPage();
            std::vector<uint8_t> t(kPrologue, kPrologue + kHelpPrologue);
            t.insert(t.end(), {0x48, 0xb8});
            Put64(t, reinterpret_cast<uint64_t>(at + kHelpPrologue));
            t.insert(t.end(), {0xff, 0xe0});  // jmp rax
            std::memcpy(tramp, t.data(), t.size());
            g_help_original = reinterpret_cast<HelpFn>(tramp);
            std::vector<uint8_t> h = {0x48, 0xb8};
            Put64(h, reinterpret_cast<uint64_t>(&HelpHook));
            h.insert(h.end(), {0xff, 0xe0, 0x90, 0x90});  // movabs rax, hook; jmp rax; nop; nop (14 bytes)
            std::memcpy(at, h.data(), h.size());
            std::fprintf(stderr, "[patch] in-game Options: help line hooked\n");
        } else {
            std::fprintf(stderr, "[patch] in-game Options help line: bytes differ, not hooked\n");
        }
    }
    // Hook the text setter (FUN_01b816a0) to remember each element's text, for the rows a settings list borrows.
    {
        static const uint8_t kPrologue[kSetTextPrologue] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53,
                                                            0x48, 0x89, 0xf3, 0x49, 0x89, 0xfe};
        uint8_t* at = base + kSetText;
        if (std::memcmp(at, kPrologue, kSetTextPrologue) == 0) {
            auto* tramp = ExecPage();
            std::vector<uint8_t> t(kPrologue, kPrologue + kSetTextPrologue);
            t.insert(t.end(), {0x48, 0xb8});
            Put64(t, reinterpret_cast<uint64_t>(at + kSetTextPrologue));
            t.insert(t.end(), {0xff, 0xe0});  // jmp rax
            std::memcpy(tramp, t.data(), t.size());
            g_set_text_original = reinterpret_cast<SetTextFn>(tramp);
            std::vector<uint8_t> h = {0x48, 0xb8};
            Put64(h, reinterpret_cast<uint64_t>(&SetTextHook));
            h.insert(h.end(), {0xff, 0xe0, 0x90});  // movabs rax, hook; jmp rax; nop (13 bytes)
            std::memcpy(at, h.data(), h.size());
            std::fprintf(stderr, "[patch] Options page: text setter hooked\n");
        } else {
            std::fprintf(stderr, "[patch] Options page text setter: bytes differ, not hooked\n");
        }
    }
    // Hook the text lookup: entry -> movabs rax, LookupHook; jmp rax. The trampoline runs the 14 moved
    // prologue bytes, then jumps back to the original after them.
    {
        static const uint8_t kPrologue[kLookupPrologue] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                                           0x56, 0x41, 0x54, 0x53, 0x49, 0x89, 0xf6};
        uint8_t* at = base + kLookup;
        if (std::memcmp(at, kPrologue, kLookupPrologue) == 0) {
            auto* tramp = ExecPage();
            std::vector<uint8_t> t(kPrologue, kPrologue + kLookupPrologue);
            t.insert(t.end(), {0x48, 0xb8});
            Put64(t, reinterpret_cast<uint64_t>(at + kLookupPrologue));
            t.insert(t.end(), {0xff, 0xe0});  // jmp rax
            std::memcpy(tramp, t.data(), t.size());
            g_lookup_original = reinterpret_cast<LookupFn>(tramp);
            std::vector<uint8_t> h = {0x48, 0xb8};
            Put64(h, reinterpret_cast<uint64_t>(&LookupHook));
            h.insert(h.end(), {0xff, 0xe0, 0x90, 0x90});
            std::memcpy(at, h.data(), h.size());
            std::fprintf(stderr, "[patch] text lookup: port labels for the Options rows\n");
        } else {
            std::fprintf(stderr, "[patch] text lookup: bytes differ, not hooked\n");
        }
    }
    // Setup: 0xcbb4da..0xcbb504 hides rows +0xf8, +0xe8, +0xf0 -> mov rdi, rbx; movabs rax, OptionsInit; call rax.
    {
        std::vector<uint8_t> b = {0x48, 0x89, 0xdf, 0x48, 0xb8};
        Put64(b, reinterpret_cast<uint64_t>(&OptionsInit));
        b.insert(b.end(), {0xff, 0xd0});
        Write(0xcbb4da, {0x48, 0x8b, 0xbb, 0xf8, 0x00, 0x00, 0x00, 0x31, 0xf6, 0xe8}, b, 0xcbb504 - 0xcbb4da,
              "Options menu: Display Settings and Gameplay rows");
    }
    // Cross on a row: the body after `CMP R14, [RBX + row]; JNZ next` -> OptionsSelect(page, row); jmp next.
    struct Site { uint64_t body, next; int row; std::vector<uint8_t> expect; const char* what; };
    // Cross on the Video row (+0xe8) opens Display Settings, on the Effects row (+0xf0) Gameplay.
    const Site kSites[] = {
        {0xcbb96a, 0xcbb9ef, 0, {0xc7, 0x45, 0xc8, 0xff, 0xff, 0xff, 0xff}, "Options menu: Display Settings opens the settings list"},
        {0xcbb9fc, 0xcbba81, 1, {0xc7, 0x45, 0xc8, 0xff, 0xff, 0xff, 0xff}, "Options menu: Gameplay opens the gameplay list"},
    };
    for (const Site& s : kSites) {
        std::vector<uint8_t> b = {0x48, 0x89, 0xdf, 0xbe};  // mov rdi, rbx; mov esi, row
        Put32(b, uint32_t(s.row));
        b.insert(b.end(), {0x48, 0xb8});
        Put64(b, reinterpret_cast<uint64_t>(&OptionsSelect));
        b.insert(b.end(), {0xff, 0xd0, 0xe9});  // call rax; jmp next
        const uint64_t after_jmp = s.body + b.size() + 4;
        Put32(b, uint32_t(int32_t(int64_t(s.next) - int64_t(after_jmp))));
        Write(s.body, s.expect, b, s.next - s.body, s.what);
    }
}

}  // namespace port
