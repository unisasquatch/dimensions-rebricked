// The port's in-game overlay, drawn over the game with Dear ImGui: the Toy Pad picker (the pause menu's
// "Toy Pad" row) and the Display Settings list (the Options page's "Display Settings" row).
#pragma once
#include <vulkan/vulkan.h>

union SDL_Event;
struct SDL_Window;

namespace ui {

// Main thread, once the window exists.
void Init(SDL_Window* window);
// Main thread: every SDL event. Returns true when the overlay used it (the game must not see it).
bool ProcessEvent(const SDL_Event& e);

void OpenPicker();    // any thread
void OpenSettings();          // any thread: Display Settings
void OpenGameplaySettings();  // any thread: Gameplay (local players, keyboard, cutscene skipping)
bool PickerOpen();    // either panel is open
// True while the picker is open and briefly after it closes, so the button that closed it doesn't
// reach the game.
bool BlocksGameInput();

// Presenter (GPU thread): draws the overlay into the swapchain image `view` (COLOR_ATTACHMENT_OPTIMAL).
// Only called while the picker is open.
void Record(VkCommandBuffer cmd, VkImageView view, VkExtent2D extent, VkFormat format);

}  // namespace ui
