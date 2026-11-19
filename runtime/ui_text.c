// Catalog of the text the host shows the player (ui_text.h). One array per catalog language, indexed by UiTextId: a new
// string gets an id in ui_text.h and an entry in every language that has one (a missing entry falls back to English).
// The software keyboard has every language; the menu has English, Russian and Brazilian Portuguese.
#include <stdatomic.h>
#include <string.h>

#include "cfg.h"
#include "ov.h"
#include "platform/platform.h"
#include "ui_text.h"

typedef struct { const char *lang; const char *const *text; } Row;

// The language names are the same in every row.
#define LANG_NAMES \
    [UI_MENU_LANGNAME_EN] = "English", [UI_MENU_LANGNAME_JA] = "日本語", [UI_MENU_LANGNAME_FR] = "Français", \
    [UI_MENU_LANGNAME_DE] = "Deutsch", [UI_MENU_LANGNAME_IT] = "Italiano", [UI_MENU_LANGNAME_ES] = "Español", \
    [UI_MENU_LANGNAME_NL] = "Nederlands", [UI_MENU_LANGNAME_RU] = "Русский", \
    [UI_MENU_LANGNAME_ZH_HANS] = "简体中文", [UI_MENU_LANGNAME_ZH_HANT] = "繁體中文", [UI_MENU_LANGNAME_KO] = "한국어", \
    [UI_MENU_LANGNAME_PT_BR] = "Português (Brasil)"

// SWKBD: TITLE, PROMPT, DEFAULT_NAME (at most 9 UTF-16 units, no '@' '%' '\'), OK, CANCEL, PAD_HINT.
#define SWKBD(title, prompt, name, ok, cancel, hint) \
    [UI_SWKBD_TITLE] = title, [UI_SWKBD_PROMPT] = prompt, [UI_SWKBD_DEFAULT_NAME] = name, [UI_SWKBD_OK] = ok, \
    [UI_SWKBD_CANCEL] = cancel, [UI_SWKBD_PAD_HINT] = hint, LANG_NAMES

static const char *const text_en[UI_TEXT_COUNT] = {
    SWKBD("Text entry", "Enter a name", "Horse", "OK", "Cancel", "Controller: A = OK, B = Cancel"),

    [UI_MENU_TITLE] = "Settings",
    [UI_MENU_TAB_SCREEN] = "Screen", [UI_MENU_TAB_GAME] = "Game", [UI_MENU_TAB_SOUND] = "Sound",
    [UI_MENU_TAB_CONTROLS] = "Controls", [UI_MENU_TAB_MENU] = "Menu", [UI_MENU_TAB_ADVANCED] = "Advanced",
    [UI_MENU_CONTINUE] = "Continue", [UI_MENU_QUIT] = "Quit game",
    [UI_MENU_ON] = "On", [UI_MENU_OFF] = "Off", [UI_MENU_AUTO] = "Automatic", [UI_MENU_DEFAULT_MARK] = "default",
    [UI_MENU_AFTER_RESTART] = "after restart", [UI_MENU_SET_BY_ENV] = "set by environment variable",
    [UI_MENU_SAVE_FAILED] = "could not save settings",
    [UI_MENU_VERSION] = "XC3 Recomp %s, game %s",

    [UI_MENU_FULLSCREEN] = "Fullscreen", [UI_MENU_FULLSCREEN_DESC] = "Fills the screen with the game; F11 toggles it.",
    [UI_MENU_VRR] = "Variable refresh rate",
    [UI_MENU_VRR_DESC] = "For G-SYNC and FreeSync monitors: each frame shows as soon as it is ready, with the frame rate "
                         "held a little under the refresh rate so it stays in the monitor's range. Replaces the "
                         "presentation setting. Needs VRR on in the driver; works best in fullscreen.",
    [UI_MENU_WINDOW_SIZE] = "Window size", [UI_MENU_WINDOW_SIZE_DESC] = "Size of the game window when not fullscreen.",
    [UI_MENU_WINDOW_SIZE_CUSTOM] = "Custom",
    [UI_MENU_PRESENT] = "Presentation", [UI_MENU_PRESENT_DESC] = "How finished frames reach the screen.",
    [UI_MENU_PRESENT_MAILBOX] = "Mailbox", [UI_MENU_PRESENT_VSYNC] = "VSync", [UI_MENU_PRESENT_IMMEDIATE] = "Immediate",
    [UI_MENU_PRESENT_IN_USE] = "in use: %s",
    [UI_MENU_GPU] = "Graphics card", [UI_MENU_GPU_DESC] = "The Vulkan device that draws the game.",
    [UI_MENU_GPU_AUTO] = "Automatic",
    [UI_MENU_PERF_OVERLAY] = "Performance overlay",
    [UI_MENU_PERF_OVERLAY_DESC] = "Frame rate in the top right corner. Full adds frame times with a graph, CPU, GPU and "
                                  "memory load, temperatures and the render thread's waits. F10 cycles the modes.",
    [UI_MENU_PERF_OFF] = "Off", [UI_MENU_PERF_FPS] = "FPS", [UI_MENU_PERF_FULL] = "Full",
    // printf format of the overlay line in FPS mode: the frame rate
    [UI_PERF_LINE_FPS] = "%.0f fps",
    [UI_MENU_GRAPHICS] = "Picture",
    [UI_MENU_OUTPUT_RES] = "Game resolution",
    [UI_MENU_OUTPUT_RES_DESC] = "The size the game draws its picture and interface in. Above 1920 x 1080 the game needs "
                                "more video memory and GPU time.",
    [UI_MENU_RENDER_SCALE] = "3D render scale",
    [UI_MENU_RENDER_SCALE_DESC] = "Size of the 3D scene relative to the game resolution; the game scales it up to "
                                  "that size and draws the interface at full size. Game draws the whole scene. "
                                  "For supersampling, set the game resolution above the window's. Takes effect after "
                                  "a restart.",
    [UI_MENU_RENDER_SCALE_GAME] = "Game (whole)",
    [UI_MENU_RENDER_SCALE_MODE] = "%s (%d %%)",
    [UI_MENU_MODE_ULTRA_PERF] = "Ultra performance",
    [UI_MENU_MODE_PERF] = "Performance",
    [UI_MENU_MODE_BALANCED] = "Balanced",
    [UI_MENU_MODE_QUALITY] = "Quality",
    [UI_MENU_UPSCALER] = "Upscaler",
    [UI_MENU_UPSCALER_DESC] = "FSR 1 and bilinear scale the picture to the window when the window is larger than the "
                              "game resolution; FSR 1 keeps edges and detail sharper. The temporal upscalers rebuild the "
                              "3D scene in place of the game's own reconstruction, which draws it at half the game "
                              "resolution (or at the render scale): AMD FSR 3 needs amd_fidelityfx_vk.dll next to "
                              "xc3.exe; NVIDIA DLSS and DLAA need an RTX card and the Streamline DLLs next to xc3.exe, "
                              "and take effect after a restart. DLAA draws the scene at the full game resolution and "
                              "only smooths it.",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1", [UI_MENU_UPSCALER_BILINEAR] = "Bilinear",
    [UI_MENU_UPSCALER_FSR3] = "AMD FSR 3 (temporal)", [UI_MENU_UPSCALER_DLSS] = "NVIDIA DLSS",
    [UI_MENU_UPSCALER_DLAA] = "NVIDIA DLAA",
    [UI_MENU_FRAME_GEN] = "Frame generation",
    [UI_MENU_FRAME_GEN_DESC] = "Shows a generated frame between every two the game draws, for twice the frames on "
                               "screen; the frame rate limit then counts the shown frames, so 120 draws 60. It works "
                               "from the depth and motion of a temporal upscaler (AMD FSR 3, NVIDIA DLSS or DLAA) and is "
                               "off with the others, and while this menu is open. NVIDIA DLSS Frame Generation needs an "
                               "RTX 40 or newer card, hardware-accelerated GPU scheduling and the Streamline DLLs; AMD "
                               "FSR 3 runs on any card with amd_fidelityfx_vk.dll. Takes effect after a restart.",
    [UI_MENU_FRAME_GEN_OFF] = "Off", [UI_MENU_FRAME_GEN_DLSS] = "NVIDIA DLSS Frame Generation",
    [UI_MENU_FRAME_GEN_FSR] = "AMD FSR 3 frame generation",
    [UI_MENU_FRAME_GEN_MULTI] = "DLSS frame multiplier",
    [UI_MENU_FRAME_GEN_MULTI_DESC] = "Frames shown per frame the game draws with NVIDIA DLSS Frame Generation: 3x to 6x "
                                     "(Multi Frame Generation) need an RTX 50 card, up to what it supports; others stay "
                                     "at 2x. The frame rate "
                                     "limit counts the shown frames, so 240 at 4x draws 60.",
    [UI_MENU_PICTURE_FIT] = "Picture shape",
    [UI_MENU_PICTURE_FIT_DESC] = "When the window's shape differs from the game resolution's (a 16:9 game on the Steam "
                                 "Deck's 16:10 screen), keep the picture's proportions with black bars, or stretch it "
                                 "over the whole window.",
    [UI_MENU_PICTURE_FIT_KEEP] = "Keep (bars)", [UI_MENU_PICTURE_FIT_STRETCH] = "Stretch",
    [UI_MENU_SHARPNESS] = "FSR sharpness",
    [UI_MENU_SHARPNESS_DESC] = "Sharpening after FSR scaling; 0 turns it off.",
    [UI_MENU_DLSS_PRESET] = "DLSS model",
    [UI_MENU_DLSS_PRESET_DESC] = "The DLSS model. Default is NVIDIA's choice per mode (K for DLAA, quality and balanced, M for "
                                 "performance, L for ultra performance); K, J, L or M takes that model in every mode. J "
                                 "ghosts a little less behind moving characters and flickers a little more than K.",
    [UI_MENU_DLSS_PRESET_DEFAULT] = "Default",
    [UI_MENU_REFLEX] = "NVIDIA Reflex",
    [UI_MENU_REFLEX_DESC] = "Low latency with DLSS or DLAA: the game starts each frame as late as the GPU allows, so the "
                            "picture is closer to your input. Boost also keeps the GPU clocks up. DLSS Frame Generation "
                            "always keeps it on.",
    [UI_MENU_REFLEX_BOOST] = "On + boost",
    [UI_MENU_DYNAMIC_RES] = "Dynamic resolution",
    [UI_MENU_DYNAMIC_RES_DESC] = "The game's own: the 3D scene draws smaller while the GPU cannot keep up with the frame "
                                 "rate limit, and whole again when it can. Best with DLSS or FSR 3, which scale it back "
                                 "up.",
    [UI_MENU_HDR] = "HDR",
    [UI_MENU_HDR_DESC] = "HDR10 output: the game's picture expanded to your display's range. Needs Windows' HDR on. "
                         "Takes a restart.",
    [UI_MENU_HDR_PEAK] = "HDR peak brightness",
    [UI_MENU_HDR_PEAK_DESC] = "Your display's peak brightness in nits; the brightest highlights reach it.",
    [UI_MENU_HDR_GAME] = "HDR scene white",
    [UI_MENU_HDR_GAME_DESC] = "The brightness in nits of the scene's white before the highlights are expanded.",
    [UI_MENU_HDR_UI] = "HDR interface brightness",
    [UI_MENU_HDR_UI_DESC] = "The brightness in nits of the game's interface and this menu.",
    [UI_MENU_HDR_HIGHLIGHTS] = "HDR highlights",
    [UI_MENU_HDR_HIGHLIGHTS_DESC] = "How far the scene's bright parts are taken towards the peak; 0 keeps the SDR "
                                    "picture.",
    [UI_MENU_HDR_SATURATION] = "HDR saturation",
    [UI_MENU_HDR_SATURATION_DESC] = "Colour strength of the scene; 100 % leaves it as the game made it.",
    [UI_MENU_HDR_GAMMA] = "HDR gamma",
    [UI_MENU_HDR_GAMMA_DESC] = "How the game's colours are decoded: 2.2 matches most SDR displays, sRGB lifts the darks.",

    [UI_MENU_FPS_LIMIT] = "Frame rate limit",
    [UI_MENU_FPS_LIMIT_DESC] = "The most frames per second the game draws; the game's clock follows it. Above 60 is "
                               "experimental.",
    [UI_MENU_FPS_30] = "30 FPS", [UI_MENU_FPS_40] = "40 FPS", [UI_MENU_FPS_45] = "45 FPS",
    [UI_MENU_FPS_60] = "60 FPS", [UI_MENU_FPS_90] = "90 FPS", [UI_MENU_FPS_120] = "120 FPS", [UI_MENU_FPS_144] = "144 FPS",
    [UI_MENU_FPS_165] = "165 FPS", [UI_MENU_FPS_240] = "240 FPS", [UI_MENU_FPS_360] = "360 FPS",
    // the monitor's refresh rate; printf format with the rate in Hz when it is known
    [UI_MENU_FPS_DISPLAY] = "Monitor rate", [UI_MENU_FPS_DISPLAY_HZ] = "Monitor rate (%.0f Hz)",
    [UI_MENU_FRAME_QUEUE] = "Frame queue",
    [UI_MENU_FRAME_QUEUE_DESC] = "The game starts a frame while the previous one waits for its turn on screen: far fewer "
                                 "dropped frames at the limit, up to one frame more input delay. Applies at once.",
    [UI_MENU_CUTSCENE_FPS] = "Cutscene frame rate",
    [UI_MENU_CUTSCENE_FPS_DESC] = "30 fps: every cutscene at 30, as on the console. Mixed: most in-game cutscenes at the "
                                  "frame rate limit, the rest and videos at 30. Limit: every cutscene at the limit "
                                  "(experimental). Applies after a restart.",
    [UI_MENU_CUTSCENE_30] = "30 fps", [UI_MENU_CUTSCENE_MIXED] = "Mixed", [UI_MENU_CUTSCENE_LIMIT] = "Limit",
    [UI_MENU_REALTIME] = "Game speed follows the clock",
    [UI_MENU_REALTIME_DESC] = "Steps the game by real time, so a slow frame does not slow the game down.",
    [UI_MENU_CLOTH_REALTIME] = "Cloth on the clock",
    [UI_MENU_CLOTH_REALTIME_DESC] = "Steps cloth and hair by real time as well.",
    [UI_MENU_HIGHFPS_FIX] = "High frame rate physics fix",
    [UI_MENU_HIGHFPS_FIX_DESC] = "Corrects physics that misbehave above 30 frames per second.",
    [UI_MENU_GAME_LANG] = "Game language", [UI_MENU_GAME_LANG_DESC] = "Language of the game's text and voices.",
    [UI_MENU_GAME_LANG_AUTO] = "Automatic",
    [UI_MENU_OPERATION_MODE] = "Console mode",
    [UI_MENU_OPERATION_MODE_DESC] = "Whether the game thinks it runs docked or handheld.",
    [UI_MENU_MODE_DOCKED] = "Docked", [UI_MENU_MODE_HANDHELD] = "Handheld",
    [UI_MENU_MOD_ARCHIVE] = "Mods replace archive files",
    [UI_MENU_MOD_ARCHIVE_DESC] = "Mod files in the mods folder replace the files inside the game's archives (text, fonts, models), as the xc3-file-loader plugin does on an emulator. Off: mods replace only movies and sound banks.",

    [UI_MENU_VOLUME] = "Volume", [UI_MENU_VOLUME_DESC] = "Master volume of the game.",

    [UI_MENU_MOTION] = "Motion controls", [UI_MENU_MOTION_DESC] = "Aiming with the controller's gyroscope.",
    [UI_MENU_RUMBLE_STRENGTH] = "Vibration strength",
    [UI_MENU_RUMBLE_STRENGTH_DESC] = "How strong the controller vibrates.",
    [UI_MENU_RUMBLE_CURVE] = "Vibration curve",
    [UI_MENU_RUMBLE_CURVE_DESC] = "Lower values make weak vibration stronger.",
    [UI_MENU_RUMBLE_TRIGGERS] = "Trigger vibration (Xbox)",
    [UI_MENU_RUMBLE_TRIGGERS_DESC] = "Vibrates the triggers of an Xbox controller too.",
    [UI_MENU_PAD_LAYOUT] = "Button layout",
    [UI_MENU_PAD_LAYOUT_DESC] = "Which button is A. Nintendo: the right one, as on a Switch. Xbox: the bottom one, as "
                                "labelled on an Xbox controller (cross on a PlayStation controller). Automatic is "
                                "Nintendo for every controller, so the arts sit where the game shows them.",
    [UI_MENU_PAD_LAYOUT_AUTO] = "Automatic", [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo (A right)",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox (A bottom)", [UI_MENU_PAD_LAYOUT_IN_USE] = "in use: %s",
    [UI_MENU_PROMPTS] = "Button prompts",
    [UI_MENU_PROMPTS_DESC] = "The button glyphs the game shows. Automatic picks them from the controller connected at "
                             "start. Takes effect at the next start.",
    [UI_MENU_PROMPTS_AUTO] = "Automatic", [UI_MENU_PROMPTS_SWITCH] = "Switch", [UI_MENU_PROMPTS_XBOX] = "Xbox",
    [UI_MENU_PROMPTS_PLAYSTATION] = "PlayStation",
    [UI_MENU_MOUSE_SENS] = "Mouse camera sensitivity",
    [UI_MENU_MOUSE_SENS_DESC] = "How fast the mouse turns the camera.",

    [UI_BINDS_HELP] = "Select a cell and press A or Enter, then the key, mouse button or controller button. Delete or "
                      "X clears a cell. A key in use moves here; a controller button swaps with the one it replaces.",
    [UI_BINDS_COL_KEY1] = "Key 1", [UI_BINDS_COL_KEY2] = "Key 2", [UI_BINDS_COL_PAD] = "Controller",
    [UI_BINDS_GROUP_BUTTONS] = "Buttons", [UI_BINDS_GROUP_MOVE] = "Movement", [UI_BINDS_GROUP_CAMERA] = "Camera",
    [UI_BINDS_PRESS_KEY] = "Press a key or mouse button for %s",
    [UI_BINDS_PRESS_PAD] = "Press a controller button for %s",
    [UI_BINDS_CANCEL_HINT] = "Esc cancels, or wait %d s",
    [UI_BINDS_TAKEN] = "%s was taken from %s", [UI_BINDS_SWAPPED] = "%s swapped with %s",
    [UI_BINDS_RESET] = "Reset all buttons",
    [UI_BINDS_RESET_DESC] = "Back to the default keys and controller buttons. The button layout above applies again: remapped controller buttons stay where you put them until this reset.",
    [UI_BIND_A] = "A", [UI_BIND_B] = "B", [UI_BIND_X] = "X", [UI_BIND_Y] = "Y",
    [UI_BIND_LCLICK] = "Left stick click", [UI_BIND_RCLICK] = "Right stick click", [UI_BIND_L] = "L",
    [UI_BIND_R] = "R", [UI_BIND_ZL] = "ZL", [UI_BIND_ZR] = "ZR", [UI_BIND_PLUS] = "Plus", [UI_BIND_MINUS] = "Minus",
    [UI_BIND_LEFT] = "D-pad left", [UI_BIND_UP] = "D-pad up", [UI_BIND_RIGHT] = "D-pad right",
    [UI_BIND_DOWN] = "D-pad down",
    [UI_BIND_LSTICK_LEFT] = "Move left", [UI_BIND_LSTICK_UP] = "Move forward", [UI_BIND_LSTICK_RIGHT] = "Move right",
    [UI_BIND_LSTICK_DOWN] = "Move back",
    [UI_BIND_RSTICK_LEFT] = "Camera left", [UI_BIND_RSTICK_UP] = "Camera up", [UI_BIND_RSTICK_RIGHT] = "Camera right",
    [UI_BIND_RSTICK_DOWN] = "Camera down",
    [UI_BIND_DESC_A] = "Interact, pick up, talk.", [UI_BIND_DESC_B] = "Run, cancel, go back.",
    [UI_BIND_DESC_X] = "Jump.", [UI_BIND_DESC_Y] = "Attack.", [UI_BIND_DESC_LCLICK] = "Crouch.",
    [UI_BIND_DESC_RCLICK] = "First-person view.", [UI_BIND_DESC_L] = "Abilities.",
    [UI_BIND_DESC_R] = "Throw a weapon.", [UI_BIND_DESC_ZL] = "Shield, lock on.", [UI_BIND_DESC_ZR] = "Bow.",
    [UI_BIND_DESC_PLUS] = "Menu, inventory.", [UI_BIND_DESC_MINUS] = "Map.",
    [UI_BIND_DESC_DPAD] = "Quick-select.",
    [UI_BIND_DESC_MOVE] = "Moves Link like the left stick. The controller's left stick always works too.",
    [UI_BIND_DESC_CAMERA] = "Turns the camera like the right stick. The mouse and the controller's right stick always "
                            "work too.",
    [UI_KEY_SPACE] = "Space", [UI_KEY_TAB] = "Tab", [UI_KEY_LSHIFT] = "Left Shift", [UI_KEY_LCTRL] = "Left Ctrl",
    [UI_KEY_UP] = "Up arrow", [UI_KEY_DOWN] = "Down arrow", [UI_KEY_LEFT] = "Left arrow",
    [UI_KEY_RIGHT] = "Right arrow",
    [UI_KEY_MOUSE_L] = "Left mouse", [UI_KEY_MOUSE_R] = "Right mouse", [UI_KEY_MOUSE_M] = "Middle mouse",
    [UI_KEY_MOUSE_X1] = "Mouse 4", [UI_KEY_MOUSE_X2] = "Mouse 5",
    [UI_KEY_ENTER] = "Enter", [UI_KEY_BACKSPACE] = "Backspace", [UI_KEY_RSHIFT] = "Right Shift",
    [UI_KEY_RCTRL] = "Right Ctrl", [UI_KEY_LALT] = "Alt", [UI_KEY_CAPSLOCK] = "Caps Lock", [UI_KEY_INSERT] = "Insert",
    [UI_KEY_DELETE] = "Delete", [UI_KEY_HOME] = "Home", [UI_KEY_END] = "End", [UI_KEY_PAGEUP] = "Page Up",
    [UI_KEY_PAGEDOWN] = "Page Down", [UI_KEY_NUM_FMT] = "Num %s",
    [UI_PAD_SOUTH] = "Bottom button", [UI_PAD_EAST] = "Right button", [UI_PAD_WEST] = "Left button",
    [UI_PAD_NORTH] = "Top button", [UI_PAD_LSTICK] = "Left stick", [UI_PAD_RSTICK] = "Right stick",
    [UI_PAD_L] = "Left bumper", [UI_PAD_R] = "Right bumper", [UI_PAD_START] = "Start", [UI_PAD_BACK] = "Back",
    [UI_PAD_DPAD_LEFT] = "D-pad left", [UI_PAD_DPAD_UP] = "D-pad up", [UI_PAD_DPAD_RIGHT] = "D-pad right",
    [UI_PAD_DPAD_DOWN] = "D-pad down", [UI_PAD_ZL] = "Left trigger", [UI_PAD_ZR] = "Right trigger",
    [UI_PAD_CROSS] = "Cross", [UI_PAD_CIRCLE] = "Circle", [UI_PAD_SQUARE] = "Square", [UI_PAD_TRIANGLE] = "Triangle",

    [UI_MENU_MENU_LANG] = "Menu language", [UI_MENU_MENU_LANG_DESC] = "Language of this menu and the overlay.",
    [UI_MENU_MENU_LANG_AUTO] = "Automatic (from Windows)",

    [UI_MENU_ADV_WARNING] = "These settings are for testing and troubleshooting. Change them only when you know why.",
    [UI_MENU_GROUP_SCHED] = "Scheduling", [UI_MENU_GROUP_SHADERS] = "Shaders and pipelines",
    [UI_MENU_GROUP_TEXMEM] = "Textures and memory", [UI_MENU_GROUP_FIXES] = "Render fixes",
    [UI_MENU_GROUP_VULKAN] = "Vulkan", [UI_MENU_GROUP_DIAG] = "Diagnostics",
    [UI_MENU_ASYNC_PREEMPT] = "Virtual core preemption",
    [UI_MENU_ASYNC_PREEMPT_DESC] = "Lets a game thread interrupt another one on the same virtual core.",
    [UI_MENU_PREEMPT_OFF] = "Off", [UI_MENU_PREEMPT_ON] = "On", [UI_MENU_PREEMPT_INHERIT] = "Inheritance only",
    [UI_MENU_REPLAY_ASYNC] = "GPU frame prep on its own thread",
    [UI_MENU_REPLAY_ASYNC_DESC] = "Prepares frames for the GPU on a separate thread instead of the game thread. Adds up to one frame of latency.",
    [UI_MENU_FREE_CORES] = "Virtual core gate",
    [UI_MENU_FREE_CORES_DESC] = "Lets only one thread run per virtual core, as on the console.",
    [UI_MENU_TIMER_1MS] = "1 ms Windows timer",
    [UI_MENU_TIMER_1MS_DESC] = "Raises the system timer resolution to 1 ms for steadier frame pacing.",
    [UI_MENU_PIPE_WAIT] = "Waiting draw handling",
    [UI_MENU_PIPE_WAIT_DESC] = "Default keeps the current scheduling with a reserved worker for waiting draws; Old (draw) uses the previous scheduling.",
    [UI_MENU_PIPE_WAIT_DEFAULT] = "Default", [UI_MENU_PIPE_WAIT_DRAW] = "Old (draw)",
    [UI_MENU_PIPE_SKIP] = "Draws skipped while a pipeline builds",
    [UI_MENU_PIPE_SKIP_DESC] = "Which draws are dropped instead of waiting for their pipeline.",
    [UI_MENU_PIPE_SKIP_STRICT] = "Strict", [UI_MENU_PIPE_SKIP_ANY] = "Any",
    [UI_MENU_PIPE_SKIP_FULLSCREEN] = "Fullscreen passes",
    [UI_MENU_LIB_PREBUILD] = "Stage library prebuild",
    [UI_MENU_LIB_PREBUILD_DESC] = "Builds shader stage libraries ahead of time in the background.",
    [UI_MENU_LIB_PREBUILD_THREADS] = "Stage library prebuild threads",
    [UI_MENU_LIB_PREBUILD_THREADS_DESC] = "Threads used for the prebuild; Automatic picks a number from your CPU.",
    [UI_MENU_PIPE_PREBUILD] = "Pipeline prebuild",
    [UI_MENU_PIPE_PREBUILD_DESC] = "Builds known pipelines ahead of time.",
    [UI_MENU_PREWARM_XLATE] = "Translator prewarm",
    [UI_MENU_PREWARM_XLATE_DESC] = "Threads that translate known shaders at start; Automatic takes half the CPU threads, at most 16.",
    [UI_MENU_GPL] = "Graphics pipeline library",
    [UI_MENU_GPL_DESC] = "Uses the Vulkan pipeline library extension; turn off if your driver misbehaves with it.",
    [UI_MENU_ASTC] = "ASTC texture decoding", [UI_MENU_ASTC_DESC] = "Where compressed ASTC textures are decoded.",
    [UI_MENU_ASTC_AUTO] = "Automatic", [UI_MENU_ASTC_GPU] = "GPU shader", [UI_MENU_ASTC_NATIVE] = "Native",
    [UI_MENU_ASTC_CPU] = "CPU",
    [UI_MENU_MEMORY] = "GPU memory mode", [UI_MENU_MEMORY_DESC] = "How game memory is shared with the GPU.",
    [UI_MENU_MEMORY_AUTO] = "Automatic", [UI_MENU_MEMORY_IMPORT] = "Import", [UI_MENU_MEMORY_MIRROR] = "Mirror",
    [UI_MENU_MEMORY_VERIFY] = "Verify",
    [UI_MENU_TEX_ALIAS] = "Texture aliasing", [UI_MENU_TEX_ALIAS_DESC] = "A new texture created over a live texture's memory starts with that content instead of empty.",
    [UI_MENU_LOD_GUARD] = "LOD guard", [UI_MENU_LOD_GUARD_DESC] = "Prevents objects flashing in the wrong place or a flat far field for a frame, by waiting for the game to finish rewriting level-of-detail data before the GPU reads it.",
    [UI_MENU_FIX_C0] = "Constant buffer fix",
    [UI_MENU_FIX_C0_DESC] = "Fills the driver-reserved constant bank c0 (storage sizes, dispatch size, point size, base instance) instead of reading zeros.",
    [UI_MENU_FIX_ZERO] = "Zero new GPU memory",
    [UI_MENU_FIX_ZERO_DESC] = "New GPU memory pools start zero-filled, as Switch memory does.",
    [UI_MENU_BIND_SKIP] = "Skip repeated binds",
    [UI_MENU_BIND_SKIP_DESC] = "Optimisation: skips bindings that did not change.",
    [UI_MENU_SHADER_MEMO] = "Shader memo", [UI_MENU_SHADER_MEMO_DESC] = "Optimisation: remembers shader lookups.",
    [UI_MENU_PIPE_MEMO] = "Pipeline memo", [UI_MENU_PIPE_MEMO_DESC] = "Optimisation: reuses the previous draw's pipeline when its state repeats.",
    [UI_MENU_TEX_MEMO] = "Texture binding memo", [UI_MENU_TEX_MEMO_DESC] = "Optimisation: remembers texture descriptors between draws.",
    [UI_MENU_DESC_TEMPLATE] = "Descriptor templates",
    [UI_MENU_DESC_TEMPLATE_DESC] = "Optimisation: updates descriptors in bulk.",
    [UI_MENU_PUSH_SKIP] = "Skip repeated push constants",
    [UI_MENU_PUSH_SKIP_DESC] = "Optimisation: skips push constants that did not change.",
    [UI_MENU_SPLIT_DRAWS] = "Early GPU submission",
    [UI_MENU_SPLIT_DRAWS_DESC] = "Submits GPU work in parts every N draws between passes so the GPU starts earlier; 0 turns it off.",
    [UI_MENU_VK_VALIDATION] = "Validation layer",
    [UI_MENU_VK_VALIDATION_DESC] = "Checks every Vulkan call and slows the game down a lot.",
    [UI_MENU_VK_VALIDATION_MISSING] = "The validation layer is not installed.",
    [UI_MENU_VK_DYN_VERTEX] = "Dynamic vertex state",
    [UI_MENU_VK_DYN_VERTEX_DESC] = "Forces dynamic vertex input on or off; the default depends on the GPU.",
    [UI_MENU_VK_DYN_A2C] = "Dynamic alpha to coverage",
    [UI_MENU_VK_DYN_A2C_DESC] = "Forces dynamic alpha to coverage on or off. Default: off on AMD's driver for RDNA 2 and "
                                "older, which applies alpha to coverage while it is set off, on elsewhere.",
    [UI_MENU_VK_SUBGROUP_PIN] = "Pin subgroup size", [UI_MENU_VK_SUBGROUP_PIN_DESC] = "Fixes the shader subgroup size.",
    [UI_MENU_TRACE] = "Performance log", [UI_MENU_TRACE_DESC] = "Writes frame timings to a file.",
    [UI_MENU_FLIGHT] = "Flight recorder",
    [UI_MENU_FLIGHT_DESC] = "Keeps a short history and saves it when a frame freezes.",
    [UI_MENU_FLIGHT_SPIKE_MS] = "Freeze threshold (ms)",
    [UI_MENU_FLIGHT_SPIKE_MS_DESC] = "Frame time that counts as a freeze.",
    [UI_MENU_INPUT_RECORD] = "Input recording",
    [UI_MENU_INPUT_RECORD_DESC] = "Writes your inputs to a file for bug reports.",
    [UI_MENU_OPEN_LOG] = "Open log folder", [UI_MENU_OPEN_LOG_DESC] = "Shows the folder with the log files.",
    [UI_MENU_COPY_INFO] = "Copy system info",
    [UI_MENU_COPY_INFO_DESC] = "Copies GPU, driver, CPU, RAM and changed settings to the clipboard.",
    [UI_MENU_COPY_INFO_DONE] = "Copied", [UI_MENU_ACTION_FAILED] = "did not work",
    [UI_MENU_RESET] = "Reset all settings to defaults", [UI_MENU_RESET_DESC] = "Applies to every tab.",
    [UI_MENU_RESET_CONFIRM] = "Reset every setting to its default?", [UI_MENU_RESET_YES] = "Reset",
    [UI_MENU_RESET_NO] = "Cancel",

    [UI_MENU_OV_FPS] = "%.0f FPS", [UI_MENU_OV_FRAME_MS] = "%.1f ms", [UI_MENU_OV_GAME_PCT] = "game %.0f%%",
    [UI_MENU_OV_MAX_MS] = "max %.1f ms",
    [UI_MENU_OV_GPU] = "GPU", [UI_MENU_OV_CPU] = "CPU", [UI_MENU_OV_RAM] = "RAM", [UI_MENU_OV_VRAM] = "VRAM",
    [UI_MENU_OV_TEMP] = "Temperature", [UI_MENU_OV_POWER] = "Power", [UI_MENU_OV_CLOCK] = "Clock",
    [UI_MENU_OV_NONE] = "-",
    [UI_MENU_OV_REPLAY] = "GPU frame prep", [UI_MENU_OV_GPU_TIME] = "GPU busy per frame",
    [UI_MENU_OV_PIPE_WAIT] = "Pipeline waits",
    [UI_MENU_OV_SHADER_WAIT] = "Shader waits", [UI_MENU_OV_TEX_UPLOADS] = "Texture uploads",
    [UI_MENU_OV_FMT_PCT] = "%.0f%%", [UI_MENU_OV_FMT_TEMP] = "%.0f °C", [UI_MENU_OV_FMT_POWER] = "%.0f W",
    [UI_MENU_OV_FMT_MHZ] = "%.0f MHz", [UI_MENU_OV_FMT_GB] = "%.1f GB", [UI_MENU_OV_FMT_GB_OF] = "/ %.1f GB",
    [UI_MENU_OV_GAME_GB] = "game %.1f GB", [UI_MENU_OV_FMT_MS] = "%.2f ms", [UI_MENU_OV_PER_FRAME] = "%.1f per frame",
    [UI_MENU_OV_WINDOW] = "Window", [UI_MENU_OV_OUTPUT] = "Game image", [UI_MENU_OV_SCENE] = "3D scene",
    [UI_MENU_OV_SCALING] = "Scaling", [UI_MENU_OV_LIMIT] = "Frame limit", [UI_MENU_OV_PRESENT] = "Presentation",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u", [UI_MENU_OV_FMT_SCENE] = "%ux%u, %.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1, sharpness %d%%", [UI_MENU_OV_SCALE_NONE] = "none (1:1)",
    [UI_MENU_OV_SCALE_BILINEAR] = "bilinear",
};

// Brazilian Portuguese: the menu and the overlay. The software keyboard keeps the "pt" row's text (text_pt).
static const char *const text_pt_br[UI_TEXT_COUNT] = {
    SWKBD("Inserir texto", "Digite um nome", "Cavalo", "OK", "Cancelar", "Controle: A = OK, B = Cancelar"),

    [UI_MENU_TITLE] = "Configurações",
    [UI_MENU_TAB_SCREEN] = "Tela", [UI_MENU_TAB_GAME] = "Jogo", [UI_MENU_TAB_SOUND] = "Som",
    [UI_MENU_TAB_CONTROLS] = "Controles", [UI_MENU_TAB_MENU] = "Menu", [UI_MENU_TAB_ADVANCED] = "Avançado",
    [UI_MENU_CONTINUE] = "Continuar", [UI_MENU_QUIT] = "Sair do jogo",
    [UI_MENU_ON] = "Ligado", [UI_MENU_OFF] = "Desligado", [UI_MENU_AUTO] = "Automático", [UI_MENU_DEFAULT_MARK] = "padrão",
    [UI_MENU_AFTER_RESTART] = "ao reiniciar", [UI_MENU_SET_BY_ENV] = "definido por variável de ambiente",
    [UI_MENU_SAVE_FAILED] = "não foi possível salvar as configurações",
    [UI_MENU_VERSION] = "XC3 Recomp %s, jogo %s",

    [UI_MENU_FULLSCREEN] = "Tela cheia", [UI_MENU_FULLSCREEN_DESC] = "O jogo ocupa a tela inteira; F11 alterna.",
    [UI_MENU_VRR] = "Taxa de atualização variável",
    [UI_MENU_VRR_DESC] = "Para monitores G-SYNC e FreeSync: cada quadro aparece assim que fica pronto, com a taxa de "
                         "quadros um pouco abaixo da taxa de atualização para ficar dentro da faixa do monitor. "
                         "Substitui a opção de apresentação. Precisa do VRR ligado no driver; funciona melhor em tela "
                         "cheia.",
    [UI_MENU_WINDOW_SIZE] = "Tamanho da janela", [UI_MENU_WINDOW_SIZE_DESC] = "Tamanho da janela do jogo fora da tela cheia.",
    [UI_MENU_WINDOW_SIZE_CUSTOM] = "Personalizado",
    [UI_MENU_PRESENT] = "Apresentação", [UI_MENU_PRESENT_DESC] = "Como os quadros prontos chegam à tela.",
    [UI_MENU_PRESENT_MAILBOX] = "Mailbox", [UI_MENU_PRESENT_VSYNC] = "VSync", [UI_MENU_PRESENT_IMMEDIATE] = "Imediata",
    [UI_MENU_PRESENT_IN_USE] = "em uso: %s",
    [UI_MENU_GPU] = "Placa de vídeo", [UI_MENU_GPU_DESC] = "O dispositivo Vulkan que desenha o jogo.",
    [UI_MENU_GPU_AUTO] = "Automática",
    [UI_MENU_PERF_OVERLAY] = "Painel de desempenho",
    [UI_MENU_PERF_OVERLAY_DESC] = "Taxa de quadros no canto superior direito. Completo mostra também o tempo dos quadros "
                                  "com um gráfico, o uso de CPU, GPU e memória, as temperaturas e as esperas da thread de "
                                  "renderização. F10 alterna os modos.",
    [UI_MENU_PERF_OFF] = "Desligado", [UI_MENU_PERF_FPS] = "FPS", [UI_MENU_PERF_FULL] = "Completo",
    [UI_PERF_LINE_FPS] = "%.0f fps",
    [UI_MENU_GRAPHICS] = "Imagem",
    [UI_MENU_OUTPUT_RES] = "Resolução do jogo",
    [UI_MENU_OUTPUT_RES_DESC] = "O tamanho em que o jogo desenha a imagem e a interface. Acima de 1920 x 1080 o jogo "
                                "precisa de mais memória de vídeo e mais tempo de GPU.",
    [UI_MENU_RENDER_SCALE] = "Escala da renderização 3D",
    [UI_MENU_RENDER_SCALE_DESC] = "Tamanho da cena 3D em relação à resolução do jogo; o jogo a amplia até esse tamanho e "
                                  "desenha a interface no tamanho cheio. Jogo desenha a cena inteira. Para "
                                  "supersampling, use uma resolução do jogo maior que a da janela. Vale ao reiniciar.",
    [UI_MENU_RENDER_SCALE_GAME] = "Jogo (inteira)",
    [UI_MENU_RENDER_SCALE_MODE] = "%s (%d %%)",
    [UI_MENU_MODE_ULTRA_PERF] = "Ultradesempenho",
    [UI_MENU_MODE_PERF] = "Desempenho",
    [UI_MENU_MODE_BALANCED] = "Equilibrado",
    [UI_MENU_MODE_QUALITY] = "Qualidade",
    [UI_MENU_UPSCALER] = "Upscaler",
    [UI_MENU_UPSCALER_DESC] = "FSR 1 e bilinear ampliam a imagem até a janela quando ela é maior que a resolução do "
                              "jogo; o FSR 1 mantém bordas e detalhes mais nítidos. Os upscalers temporais reconstroem a "
                              "cena 3D no lugar da reconstrução do próprio jogo, que a desenha na metade da resolução do "
                              "jogo (ou na escala de renderização): o AMD FSR 3 precisa do amd_fidelityfx_vk.dll ao lado "
                              "do xc3.exe; o NVIDIA DLSS e o DLAA precisam de uma placa RTX e das DLLs do Streamline ao "
                              "lado do xc3.exe, e valem ao reiniciar. O DLAA desenha a cena na resolução cheia do jogo e "
                              "só a suaviza.",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1", [UI_MENU_UPSCALER_BILINEAR] = "Bilinear",
    [UI_MENU_UPSCALER_FSR3] = "AMD FSR 3 (temporal)", [UI_MENU_UPSCALER_DLSS] = "NVIDIA DLSS",
    [UI_MENU_UPSCALER_DLAA] = "NVIDIA DLAA",
    [UI_MENU_FRAME_GEN] = "Geração de quadros",
    [UI_MENU_FRAME_GEN_DESC] = "Mostra um quadro gerado entre cada dois que o jogo desenha, dobrando os quadros na tela; "
                               "o limite de quadros passa a contar os quadros mostrados, então 120 desenha 60. Usa a "
                               "profundidade e o movimento de um upscaler temporal (AMD FSR 3, NVIDIA DLSS ou DLAA) e "
                               "fica desligada com os outros e enquanto este menu está aberto. O NVIDIA DLSS Frame "
                               "Generation precisa de uma placa RTX 40 ou mais nova, do agendamento de GPU acelerado por "
                               "hardware e das DLLs do Streamline; o AMD FSR 3 roda em qualquer placa com o "
                               "amd_fidelityfx_vk.dll. Vale ao reiniciar.",
    [UI_MENU_FRAME_GEN_OFF] = "Desligada", [UI_MENU_FRAME_GEN_DLSS] = "NVIDIA DLSS Frame Generation",
    [UI_MENU_FRAME_GEN_FSR] = "Geração de quadros AMD FSR 3",
    [UI_MENU_FRAME_GEN_MULTI] = "Multiplicador de quadros do DLSS",
    [UI_MENU_FRAME_GEN_MULTI_DESC] = "Quadros mostrados para cada quadro que o jogo desenha com o NVIDIA DLSS Frame "
                                     "Generation: 3x a 6x (Multi Frame Generation) precisam de uma placa RTX 50, até o "
                                     "que ela suporta; as outras ficam em 2x. O limite de quadros conta os quadros "
                                     "mostrados, então 240 com 4x desenha 60.",
    [UI_MENU_PICTURE_FIT] = "Formato da imagem",
    [UI_MENU_PICTURE_FIT_DESC] = "Quando o formato da janela é diferente do da resolução do jogo (um jogo 16:9 na tela "
                                 "16:10 do Steam Deck), mantém as proporções da imagem com barras pretas ou a estica "
                                 "pela janela inteira.",
    [UI_MENU_PICTURE_FIT_KEEP] = "Manter (barras)", [UI_MENU_PICTURE_FIT_STRETCH] = "Esticar",
    [UI_MENU_SHARPNESS] = "Nitidez do FSR",
    [UI_MENU_SHARPNESS_DESC] = "Nitidez aplicada depois da ampliação do FSR; 0 desliga.",
    [UI_MENU_DLSS_PRESET] = "Modelo do DLSS",
    [UI_MENU_DLSS_PRESET_DESC] = "O modelo do DLSS. Padrão é a escolha da NVIDIA para cada modo (K para DLAA, qualidade "
                                 "e equilibrado, M para desempenho, L para ultradesempenho); K, J, L ou M usam esse "
                                 "modelo em todos os modos. O J deixa um pouco menos de rastro atrás de personagens em "
                                 "movimento e cintila um pouco mais que o K.",
    [UI_MENU_DLSS_PRESET_DEFAULT] = "Padrão",
    [UI_MENU_REFLEX] = "NVIDIA Reflex",
    [UI_MENU_REFLEX_DESC] = "Baixa latência com DLSS ou DLAA: o jogo começa cada quadro o mais tarde que a GPU permite, "
                            "então a imagem fica mais perto do seu comando. Boost também mantém os clocks da GPU altos. "
                            "O DLSS Frame Generation sempre o mantém ligado.",
    [UI_MENU_REFLEX_BOOST] = "Ligado + boost",
    [UI_MENU_DYNAMIC_RES] = "Resolução dinâmica",
    [UI_MENU_DYNAMIC_RES_DESC] = "A do próprio jogo: a cena 3D é desenhada menor enquanto a GPU não acompanha o limite "
                                 "de quadros, e volta ao tamanho cheio quando consegue. Melhor com DLSS ou FSR 3, que a "
                                 "ampliam de volta.",
    [UI_MENU_HDR] = "HDR",
    [UI_MENU_HDR_DESC] = "Saída HDR10: a imagem do jogo expandida para a faixa da sua tela. Precisa do HDR do Windows "
                         "ligado. Vale ao reiniciar.",
    [UI_MENU_HDR_PEAK] = "Brilho máximo do HDR",
    [UI_MENU_HDR_PEAK_DESC] = "O brilho máximo da sua tela em nits; os pontos mais claros chegam a ele.",
    [UI_MENU_HDR_GAME] = "Branco da cena no HDR",
    [UI_MENU_HDR_GAME_DESC] = "O brilho em nits do branco da cena antes de os pontos claros serem expandidos.",
    [UI_MENU_HDR_UI] = "Brilho da interface no HDR",
    [UI_MENU_HDR_UI_DESC] = "O brilho em nits da interface do jogo e deste menu.",
    [UI_MENU_HDR_HIGHLIGHTS] = "Pontos claros do HDR",
    [UI_MENU_HDR_HIGHLIGHTS_DESC] = "Quanto as partes claras da cena são levadas em direção ao brilho máximo; 0 mantém "
                                    "a imagem SDR.",
    [UI_MENU_HDR_SATURATION] = "Saturação do HDR",
    [UI_MENU_HDR_SATURATION_DESC] = "Intensidade das cores da cena; 100 % deixa como o jogo fez.",
    [UI_MENU_HDR_GAMMA] = "Gama do HDR",
    [UI_MENU_HDR_GAMMA_DESC] = "Como as cores do jogo são decodificadas: 2.2 corresponde à maioria das telas SDR, sRGB "
                               "clareia as partes escuras.",

    [UI_MENU_FPS_LIMIT] = "Limite de quadros",
    [UI_MENU_FPS_LIMIT_DESC] = "O máximo de quadros por segundo que o jogo desenha; o relógio do jogo acompanha. Acima "
                               "de 60 é experimental.",
    [UI_MENU_FPS_30] = "30 FPS", [UI_MENU_FPS_40] = "40 FPS", [UI_MENU_FPS_45] = "45 FPS",
    [UI_MENU_FPS_60] = "60 FPS", [UI_MENU_FPS_90] = "90 FPS", [UI_MENU_FPS_120] = "120 FPS", [UI_MENU_FPS_144] = "144 FPS",
    [UI_MENU_FPS_165] = "165 FPS", [UI_MENU_FPS_240] = "240 FPS", [UI_MENU_FPS_360] = "360 FPS",
    [UI_MENU_FPS_DISPLAY] = "Taxa do monitor", [UI_MENU_FPS_DISPLAY_HZ] = "Taxa do monitor (%.0f Hz)",
    [UI_MENU_FRAME_QUEUE] = "Fila de quadros",
    [UI_MENU_FRAME_QUEUE_DESC] = "O jogo começa um quadro enquanto o anterior espera a vez de ir para a tela: muito "
                                 "menos quadros perdidos no limite, com até um quadro a mais de atraso nos comandos. "
                                 "Vale na hora.",
    [UI_MENU_CUTSCENE_FPS] = "Taxa de quadros das cenas",
    [UI_MENU_CUTSCENE_FPS_DESC] = "30 fps: todas as cenas a 30, como no console. Misto: a maioria das cenas do jogo no "
                                  "limite de quadros, o resto e os vídeos a 30. Limite: todas as cenas no limite "
                                  "(experimental). Vale ao reiniciar.",
    [UI_MENU_CUTSCENE_30] = "30 fps", [UI_MENU_CUTSCENE_MIXED] = "Misto", [UI_MENU_CUTSCENE_LIMIT] = "Limite",
    [UI_MENU_REALTIME] = "Velocidade do jogo pelo relógio",
    [UI_MENU_REALTIME_DESC] = "Avança o jogo pelo tempo real, então um quadro lento não deixa o jogo mais lento.",
    [UI_MENU_CLOTH_REALTIME] = "Tecidos pelo relógio",
    [UI_MENU_CLOTH_REALTIME_DESC] = "Avança tecidos e cabelos pelo tempo real também.",
    [UI_MENU_HIGHFPS_FIX] = "Correção da física em alta taxa de quadros",
    [UI_MENU_HIGHFPS_FIX_DESC] = "Corrige a física que se comporta mal acima de 30 quadros por segundo.",
    [UI_MENU_GAME_LANG] = "Idioma do jogo", [UI_MENU_GAME_LANG_DESC] = "Idioma dos textos e das vozes do jogo.",
    [UI_MENU_GAME_LANG_AUTO] = "Automático",
    [UI_MENU_OPERATION_MODE] = "Modo do console",
    [UI_MENU_OPERATION_MODE_DESC] = "Se o jogo acha que está na base (TV) ou no modo portátil.",
    [UI_MENU_MODE_DOCKED] = "Na base (TV)", [UI_MENU_MODE_HANDHELD] = "Portátil",
    [UI_MENU_MOD_ARCHIVE] = "Mods substituem arquivos compactados",
    [UI_MENU_MOD_ARCHIVE_DESC] = "Os arquivos da pasta mods substituem os arquivos dentro dos pacotes do jogo (textos, "
                                 "fontes, modelos), como o plugin xc3-file-loader faz num emulador. Desligado: os mods "
                                 "substituem só vídeos e bancos de som.",

    [UI_MENU_VOLUME] = "Volume", [UI_MENU_VOLUME_DESC] = "Volume geral do jogo.",

    [UI_MENU_MOTION] = "Controles de movimento", [UI_MENU_MOTION_DESC] = "Mirar com o giroscópio do controle.",
    [UI_MENU_RUMBLE_STRENGTH] = "Força da vibração",
    [UI_MENU_RUMBLE_STRENGTH_DESC] = "Com que força o controle vibra.",
    [UI_MENU_RUMBLE_CURVE] = "Curva da vibração",
    [UI_MENU_RUMBLE_CURVE_DESC] = "Valores menores deixam a vibração fraca mais forte.",
    [UI_MENU_RUMBLE_TRIGGERS] = "Vibração nos gatilhos (Xbox)",
    [UI_MENU_RUMBLE_TRIGGERS_DESC] = "Vibra também os gatilhos de um controle Xbox.",
    [UI_MENU_PAD_LAYOUT] = "Layout dos botões",
    [UI_MENU_PAD_LAYOUT_DESC] = "Qual botão é o A. Nintendo: o da direita, como no Switch. Xbox: o de baixo, como está "
                                "escrito no controle Xbox (o X num controle PlayStation). Automático usa Nintendo em "
                                "todos os controles, para que as artes fiquem onde o jogo as mostra.",
    [UI_MENU_PAD_LAYOUT_AUTO] = "Automático", [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo (A à direita)",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox (A embaixo)", [UI_MENU_PAD_LAYOUT_IN_USE] = "em uso: %s",
    [UI_MENU_PROMPTS] = "Ícones dos botões",
    [UI_MENU_PROMPTS_DESC] = "Os ícones de botão que o jogo mostra. Automático escolhe pelo controle conectado na "
                             "abertura. Vale na próxima vez que o jogo abrir.",
    [UI_MENU_PROMPTS_AUTO] = "Automático", [UI_MENU_PROMPTS_SWITCH] = "Switch", [UI_MENU_PROMPTS_XBOX] = "Xbox",
    [UI_MENU_PROMPTS_PLAYSTATION] = "PlayStation",
    [UI_MENU_MOUSE_SENS] = "Sensibilidade da câmera no mouse",
    [UI_MENU_MOUSE_SENS_DESC] = "Velocidade com que o mouse gira a câmera.",

    [UI_BINDS_HELP] = "Selecione uma célula e aperte A ou Enter, depois a tecla, o botão do mouse ou o botão do controle. "
                      "Delete ou X limpa a célula. Uma tecla já em uso vem para cá; um botão do controle troca de lugar "
                      "com o que ele substitui.",
    [UI_BINDS_COL_KEY1] = "Tecla 1", [UI_BINDS_COL_KEY2] = "Tecla 2", [UI_BINDS_COL_PAD] = "Controle",
    [UI_BINDS_GROUP_BUTTONS] = "Botões", [UI_BINDS_GROUP_MOVE] = "Movimento", [UI_BINDS_GROUP_CAMERA] = "Câmera",
    [UI_BINDS_PRESS_KEY] = "Aperte uma tecla ou botão do mouse para %s",
    [UI_BINDS_PRESS_PAD] = "Aperte um botão do controle para %s",
    [UI_BINDS_CANCEL_HINT] = "Esc cancela, ou espere %d s",
    [UI_BINDS_TAKEN] = "%s foi tirado de %s", [UI_BINDS_SWAPPED] = "%s trocou de lugar com %s",
    [UI_BINDS_RESET] = "Restaurar todos os botões",
    [UI_BINDS_RESET_DESC] = "Volta às teclas e aos botões do controle padrão. O layout dos botões acima volta a valer: os "
                            "botões do controle remapeados ficam onde você os colocou até esta restauração.",
    [UI_BIND_A] = "A", [UI_BIND_B] = "B", [UI_BIND_X] = "X", [UI_BIND_Y] = "Y",
    [UI_BIND_LCLICK] = "Clique do analógico esquerdo", [UI_BIND_RCLICK] = "Clique do analógico direito", [UI_BIND_L] = "L",
    [UI_BIND_R] = "R", [UI_BIND_ZL] = "ZL", [UI_BIND_ZR] = "ZR", [UI_BIND_PLUS] = "Mais", [UI_BIND_MINUS] = "Menos",
    [UI_BIND_LEFT] = "Direcional esquerda", [UI_BIND_UP] = "Direcional cima", [UI_BIND_RIGHT] = "Direcional direita",
    [UI_BIND_DOWN] = "Direcional baixo",
    [UI_BIND_LSTICK_LEFT] = "Andar para a esquerda", [UI_BIND_LSTICK_UP] = "Andar para a frente",
    [UI_BIND_LSTICK_RIGHT] = "Andar para a direita", [UI_BIND_LSTICK_DOWN] = "Andar para trás",
    [UI_BIND_RSTICK_LEFT] = "Câmera para a esquerda", [UI_BIND_RSTICK_UP] = "Câmera para cima",
    [UI_BIND_RSTICK_RIGHT] = "Câmera para a direita", [UI_BIND_RSTICK_DOWN] = "Câmera para baixo",
    [UI_BIND_DESC_A] = "Interagir, pegar, conversar.", [UI_BIND_DESC_B] = "Correr, cancelar, voltar.",
    [UI_BIND_DESC_X] = "Pular.", [UI_BIND_DESC_Y] = "Atacar.", [UI_BIND_DESC_LCLICK] = "Agachar.",
    [UI_BIND_DESC_RCLICK] = "Visão em primeira pessoa.", [UI_BIND_DESC_L] = "Habilidades.",
    [UI_BIND_DESC_R] = "Arremessar uma arma.", [UI_BIND_DESC_ZL] = "Escudo, travar a mira.", [UI_BIND_DESC_ZR] = "Arco.",
    [UI_BIND_DESC_PLUS] = "Menu, inventário.", [UI_BIND_DESC_MINUS] = "Mapa.",
    [UI_BIND_DESC_DPAD] = "Seleção rápida.",
    [UI_BIND_DESC_MOVE] = "Move o personagem como o analógico esquerdo. O analógico esquerdo do controle também funciona "
                          "sempre.",
    [UI_BIND_DESC_CAMERA] = "Gira a câmera como o analógico direito. O mouse e o analógico direito do controle também "
                            "funcionam sempre.",
    [UI_KEY_SPACE] = "Espaço", [UI_KEY_TAB] = "Tab", [UI_KEY_LSHIFT] = "Shift esquerdo", [UI_KEY_LCTRL] = "Ctrl esquerdo",
    [UI_KEY_UP] = "Seta para cima", [UI_KEY_DOWN] = "Seta para baixo", [UI_KEY_LEFT] = "Seta para a esquerda",
    [UI_KEY_RIGHT] = "Seta para a direita",
    [UI_KEY_MOUSE_L] = "Botão esquerdo do mouse", [UI_KEY_MOUSE_R] = "Botão direito do mouse",
    [UI_KEY_MOUSE_M] = "Botão do meio do mouse", [UI_KEY_MOUSE_X1] = "Mouse 4", [UI_KEY_MOUSE_X2] = "Mouse 5",
    [UI_KEY_ENTER] = "Enter", [UI_KEY_BACKSPACE] = "Backspace", [UI_KEY_RSHIFT] = "Shift direito",
    [UI_KEY_RCTRL] = "Ctrl direito", [UI_KEY_LALT] = "Alt", [UI_KEY_CAPSLOCK] = "Caps Lock", [UI_KEY_INSERT] = "Insert",
    [UI_KEY_DELETE] = "Delete", [UI_KEY_HOME] = "Home", [UI_KEY_END] = "End", [UI_KEY_PAGEUP] = "Page Up",
    [UI_KEY_PAGEDOWN] = "Page Down", [UI_KEY_NUM_FMT] = "Num %s",
    [UI_PAD_SOUTH] = "Botão de baixo", [UI_PAD_EAST] = "Botão da direita", [UI_PAD_WEST] = "Botão da esquerda",
    [UI_PAD_NORTH] = "Botão de cima", [UI_PAD_LSTICK] = "Analógico esquerdo", [UI_PAD_RSTICK] = "Analógico direito",
    [UI_PAD_L] = "Bumper esquerdo", [UI_PAD_R] = "Bumper direito", [UI_PAD_START] = "Start", [UI_PAD_BACK] = "Back",
    [UI_PAD_DPAD_LEFT] = "Direcional esquerda", [UI_PAD_DPAD_UP] = "Direcional cima",
    [UI_PAD_DPAD_RIGHT] = "Direcional direita", [UI_PAD_DPAD_DOWN] = "Direcional baixo",
    [UI_PAD_ZL] = "Gatilho esquerdo", [UI_PAD_ZR] = "Gatilho direito",
    [UI_PAD_CROSS] = "Xis", [UI_PAD_CIRCLE] = "Círculo", [UI_PAD_SQUARE] = "Quadrado", [UI_PAD_TRIANGLE] = "Triângulo",

    [UI_MENU_MENU_LANG] = "Idioma do menu", [UI_MENU_MENU_LANG_DESC] = "Idioma deste menu e do painel de desempenho.",
    [UI_MENU_MENU_LANG_AUTO] = "Automático (pelo Windows)",

    [UI_MENU_ADV_WARNING] = "Estas opções são para testes e solução de problemas. Mude só quando souber o porquê.",
    [UI_MENU_GROUP_SCHED] = "Agendamento", [UI_MENU_GROUP_SHADERS] = "Shaders e pipelines",
    [UI_MENU_GROUP_TEXMEM] = "Texturas e memória", [UI_MENU_GROUP_FIXES] = "Correções de renderização",
    [UI_MENU_GROUP_VULKAN] = "Vulkan", [UI_MENU_GROUP_DIAG] = "Diagnóstico",
    [UI_MENU_ASYNC_PREEMPT] = "Preempção de núcleo virtual",
    [UI_MENU_ASYNC_PREEMPT_DESC] = "Deixa uma thread do jogo interromper outra no mesmo núcleo virtual.",
    [UI_MENU_PREEMPT_OFF] = "Desligada", [UI_MENU_PREEMPT_ON] = "Ligada", [UI_MENU_PREEMPT_INHERIT] = "Só herança",
    [UI_MENU_REPLAY_ASYNC] = "Preparo de quadros da GPU em thread própria",
    [UI_MENU_REPLAY_ASYNC_DESC] = "Prepara os quadros para a GPU numa thread separada, em vez da thread do jogo. "
                                  "Acrescenta até um quadro de latência.",
    [UI_MENU_FREE_CORES] = "Trava de núcleo virtual",
    [UI_MENU_FREE_CORES_DESC] = "Deixa só uma thread rodar por núcleo virtual, como no console.",
    [UI_MENU_TIMER_1MS] = "Timer de 1 ms do Windows",
    [UI_MENU_TIMER_1MS_DESC] = "Sobe a resolução do timer do sistema para 1 ms, para um ritmo de quadros mais estável.",
    [UI_MENU_PIPE_WAIT] = "Tratamento de desenhos em espera",
    [UI_MENU_PIPE_WAIT_DESC] = "Padrão mantém o agendamento atual com um worker reservado para desenhos em espera; "
                               "Antigo (desenho) usa o agendamento anterior.",
    [UI_MENU_PIPE_WAIT_DEFAULT] = "Padrão", [UI_MENU_PIPE_WAIT_DRAW] = "Antigo (desenho)",
    [UI_MENU_PIPE_SKIP] = "Desenhos pulados enquanto uma pipeline é criada",
    [UI_MENU_PIPE_SKIP_DESC] = "Quais desenhos são descartados em vez de esperar pela pipeline.",
    [UI_MENU_PIPE_SKIP_STRICT] = "Restrito", [UI_MENU_PIPE_SKIP_ANY] = "Qualquer um",
    [UI_MENU_PIPE_SKIP_FULLSCREEN] = "Passes de tela cheia",
    [UI_MENU_LIB_PREBUILD] = "Pré-criação de bibliotecas de estágio",
    [UI_MENU_LIB_PREBUILD_DESC] = "Cria antes, em segundo plano, as bibliotecas de estágio de shader.",
    [UI_MENU_LIB_PREBUILD_THREADS] = "Threads da pré-criação de bibliotecas",
    [UI_MENU_LIB_PREBUILD_THREADS_DESC] = "Threads usadas na pré-criação; Automático escolhe um número pela sua CPU.",
    [UI_MENU_PIPE_PREBUILD] = "Pré-criação de pipelines",
    [UI_MENU_PIPE_PREBUILD_DESC] = "Cria antes as pipelines conhecidas.",
    [UI_MENU_PREWARM_XLATE] = "Aquecimento do tradutor",
    [UI_MENU_PREWARM_XLATE_DESC] = "Threads que traduzem os shaders conhecidos na abertura; Automático usa metade das "
                                   "threads da CPU, no máximo 16.",
    [UI_MENU_GPL] = "Graphics pipeline library",
    [UI_MENU_GPL_DESC] = "Usa a extensão de biblioteca de pipelines do Vulkan; desligue se o seu driver se comportar mal "
                         "com ela.",
    [UI_MENU_ASTC] = "Decodificação de texturas ASTC",
    [UI_MENU_ASTC_DESC] = "Onde as texturas ASTC comprimidas são decodificadas.",
    [UI_MENU_ASTC_AUTO] = "Automático", [UI_MENU_ASTC_GPU] = "Shader na GPU", [UI_MENU_ASTC_NATIVE] = "Nativa",
    [UI_MENU_ASTC_CPU] = "CPU",
    [UI_MENU_MEMORY] = "Modo de memória da GPU",
    [UI_MENU_MEMORY_DESC] = "Como a memória do jogo é compartilhada com a GPU.",
    [UI_MENU_MEMORY_AUTO] = "Automático", [UI_MENU_MEMORY_IMPORT] = "Importar", [UI_MENU_MEMORY_MIRROR] = "Espelhar",
    [UI_MENU_MEMORY_VERIFY] = "Verificar",
    [UI_MENU_TEX_ALIAS] = "Aliasing de texturas",
    [UI_MENU_TEX_ALIAS_DESC] = "Uma textura nova criada sobre a memória de uma textura viva começa com esse conteúdo em "
                               "vez de vazia.",
    [UI_MENU_LOD_GUARD] = "Proteção de LOD",
    [UI_MENU_LOD_GUARD_DESC] = "Evita objetos piscando no lugar errado ou um fundo distante achatado por um quadro, "
                               "esperando o jogo terminar de reescrever os dados de nível de detalhe antes de a GPU lê-los.",
    [UI_MENU_FIX_C0] = "Correção do constant buffer",
    [UI_MENU_FIX_C0_DESC] = "Preenche o banco de constantes c0 reservado ao driver (tamanhos de storage, tamanho do "
                            "dispatch, tamanho do ponto, instância base) em vez de ler zeros.",
    [UI_MENU_FIX_ZERO] = "Zerar memória nova da GPU",
    [UI_MENU_FIX_ZERO_DESC] = "Pools novos de memória da GPU começam zerados, como a memória do Switch.",
    [UI_MENU_BIND_SKIP] = "Pular binds repetidos",
    [UI_MENU_BIND_SKIP_DESC] = "Otimização: pula bindings que não mudaram.",
    [UI_MENU_SHADER_MEMO] = "Memória de shaders", [UI_MENU_SHADER_MEMO_DESC] = "Otimização: lembra as buscas de shader.",
    [UI_MENU_PIPE_MEMO] = "Memória de pipelines",
    [UI_MENU_PIPE_MEMO_DESC] = "Otimização: reaproveita a pipeline do desenho anterior quando o estado se repete.",
    [UI_MENU_TEX_MEMO] = "Memória de bindings de textura",
    [UI_MENU_TEX_MEMO_DESC] = "Otimização: lembra os descritores de textura entre desenhos.",
    [UI_MENU_DESC_TEMPLATE] = "Templates de descritores",
    [UI_MENU_DESC_TEMPLATE_DESC] = "Otimização: atualiza os descritores em bloco.",
    [UI_MENU_PUSH_SKIP] = "Pular push constants repetidas",
    [UI_MENU_PUSH_SKIP_DESC] = "Otimização: pula push constants que não mudaram.",
    [UI_MENU_SPLIT_DRAWS] = "Envio antecipado para a GPU",
    [UI_MENU_SPLIT_DRAWS_DESC] = "Envia o trabalho da GPU em partes a cada N desenhos entre passes, para a GPU começar "
                                 "antes; 0 desliga.",
    [UI_MENU_VK_VALIDATION] = "Camada de validação",
    [UI_MENU_VK_VALIDATION_DESC] = "Confere cada chamada Vulkan e deixa o jogo muito mais lento.",
    [UI_MENU_VK_VALIDATION_MISSING] = "A camada de validação não está instalada.",
    [UI_MENU_VK_DYN_VERTEX] = "Estado de vértices dinâmico",
    [UI_MENU_VK_DYN_VERTEX_DESC] = "Força a entrada de vértices dinâmica ligada ou desligada; o padrão depende da GPU.",
    [UI_MENU_VK_DYN_A2C] = "Alpha to coverage dinâmico",
    [UI_MENU_VK_DYN_A2C_DESC] = "Força o alpha to coverage dinâmico ligado ou desligado. Padrão: desligado no driver da "
                                "AMD para RDNA 2 e anteriores, que aplica o alpha to coverage mesmo desligado; ligado "
                                "nos outros.",
    [UI_MENU_VK_SUBGROUP_PIN] = "Fixar tamanho de subgrupo",
    [UI_MENU_VK_SUBGROUP_PIN_DESC] = "Fixa o tamanho de subgrupo dos shaders.",
    [UI_MENU_TRACE] = "Log de desempenho", [UI_MENU_TRACE_DESC] = "Grava os tempos dos quadros num arquivo.",
    [UI_MENU_FLIGHT] = "Gravador de voo",
    [UI_MENU_FLIGHT_DESC] = "Guarda um histórico curto e o salva quando um quadro trava.",
    [UI_MENU_FLIGHT_SPIKE_MS] = "Limite de travamento (ms)",
    [UI_MENU_FLIGHT_SPIKE_MS_DESC] = "Tempo de quadro que conta como travamento.",
    [UI_MENU_INPUT_RECORD] = "Gravação de comandos",
    [UI_MENU_INPUT_RECORD_DESC] = "Grava seus comandos num arquivo para relatórios de bug.",
    [UI_MENU_OPEN_LOG] = "Abrir pasta de logs", [UI_MENU_OPEN_LOG_DESC] = "Mostra a pasta com os arquivos de log.",
    [UI_MENU_COPY_INFO] = "Copiar informações do sistema",
    [UI_MENU_COPY_INFO_DESC] = "Copia GPU, driver, CPU, RAM e as configurações alteradas para a área de transferência.",
    [UI_MENU_COPY_INFO_DONE] = "Copiado", [UI_MENU_ACTION_FAILED] = "não funcionou",
    [UI_MENU_RESET] = "Restaurar todas as configurações", [UI_MENU_RESET_DESC] = "Vale para todas as abas.",
    [UI_MENU_RESET_CONFIRM] = "Restaurar todas as configurações para o padrão?", [UI_MENU_RESET_YES] = "Restaurar",
    [UI_MENU_RESET_NO] = "Cancelar",

    [UI_MENU_OV_FPS] = "%.0f FPS", [UI_MENU_OV_FRAME_MS] = "%.1f ms", [UI_MENU_OV_GAME_PCT] = "jogo %.0f%%",
    [UI_MENU_OV_MAX_MS] = "máx. %.1f ms",
    [UI_MENU_OV_GPU] = "GPU", [UI_MENU_OV_CPU] = "CPU", [UI_MENU_OV_RAM] = "RAM", [UI_MENU_OV_VRAM] = "VRAM",
    [UI_MENU_OV_TEMP] = "Temperatura", [UI_MENU_OV_POWER] = "Consumo", [UI_MENU_OV_CLOCK] = "Clock",
    [UI_MENU_OV_NONE] = "-",
    [UI_MENU_OV_REPLAY] = "Preparo de quadros da GPU", [UI_MENU_OV_GPU_TIME] = "GPU ocupada por quadro",
    [UI_MENU_OV_PIPE_WAIT] = "Esperas por pipeline",
    [UI_MENU_OV_SHADER_WAIT] = "Esperas por shader", [UI_MENU_OV_TEX_UPLOADS] = "Envios de textura",
    [UI_MENU_OV_FMT_PCT] = "%.0f%%", [UI_MENU_OV_FMT_TEMP] = "%.0f °C", [UI_MENU_OV_FMT_POWER] = "%.0f W",
    [UI_MENU_OV_FMT_MHZ] = "%.0f MHz", [UI_MENU_OV_FMT_GB] = "%.1f GB", [UI_MENU_OV_FMT_GB_OF] = "/ %.1f GB",
    [UI_MENU_OV_GAME_GB] = "jogo %.1f GB", [UI_MENU_OV_FMT_MS] = "%.2f ms", [UI_MENU_OV_PER_FRAME] = "%.1f por quadro",
    [UI_MENU_OV_WINDOW] = "Janela", [UI_MENU_OV_OUTPUT] = "Imagem do jogo", [UI_MENU_OV_SCENE] = "Cena 3D",
    [UI_MENU_OV_SCALING] = "Ampliação", [UI_MENU_OV_LIMIT] = "Limite de quadros", [UI_MENU_OV_PRESENT] = "Apresentação",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u", [UI_MENU_OV_FMT_SCENE] = "%ux%u, %.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1, nitidez %d%%", [UI_MENU_OV_SCALE_NONE] = "nenhuma (1:1)",
    [UI_MENU_OV_SCALE_BILINEAR] = "bilinear",
};

static const char *const text_ru[UI_TEXT_COUNT] = {
    SWKBD("Ввод текста", "Введите имя", "Лошадь", "ОК", "Отмена", "Контроллер: A — ОК, B — отмена"),

    [UI_MENU_TITLE] = "Настройки",
    [UI_MENU_TAB_SCREEN] = "Экран", [UI_MENU_TAB_GAME] = "Игра", [UI_MENU_TAB_SOUND] = "Звук",
    [UI_MENU_TAB_CONTROLS] = "Управление", [UI_MENU_TAB_MENU] = "Меню", [UI_MENU_TAB_ADVANCED] = "Дополнительно",
    [UI_MENU_CONTINUE] = "Продолжить", [UI_MENU_QUIT] = "Выйти из игры",
    [UI_MENU_ON] = "Вкл.", [UI_MENU_OFF] = "Выкл.", [UI_MENU_AUTO] = "Автоматически",
    [UI_MENU_DEFAULT_MARK] = "по умолчанию",
    [UI_MENU_AFTER_RESTART] = "после перезапуска", [UI_MENU_SET_BY_ENV] = "задано переменной окружения",
    [UI_MENU_SAVE_FAILED] = "не удалось сохранить настройки",
    [UI_MENU_VERSION] = "XC3 Recomp %s, игра %s",

    [UI_MENU_FULLSCREEN] = "Полный экран", [UI_MENU_FULLSCREEN_DESC] = "Игра на весь экран; F11 переключает режим.",
    [UI_MENU_VRR] = "Переменная частота обновления",
    [UI_MENU_VRR_DESC] = "Для мониторов G-SYNC и FreeSync: каждый кадр выводится, как только готов, а частота кадров "
                         "держится чуть ниже частоты обновления, чтобы оставаться в диапазоне монитора. Заменяет "
                         "настройку вывода кадров. Нужна включённая VRR в драйвере; лучше всего в полноэкранном режиме.",
    [UI_MENU_WINDOW_SIZE] = "Размер окна", [UI_MENU_WINDOW_SIZE_DESC] = "Размер окна игры вне полноэкранного режима.",
    [UI_MENU_WINDOW_SIZE_CUSTOM] = "Другой",
    [UI_MENU_PRESENT] = "Вывод кадров", [UI_MENU_PRESENT_DESC] = "Как готовые кадры попадают на экран.",
    [UI_MENU_PRESENT_MAILBOX] = "Mailbox", [UI_MENU_PRESENT_VSYNC] = "Вертикальная синхронизация",
    [UI_MENU_PRESENT_IMMEDIATE] = "Без ожидания",
    [UI_MENU_PRESENT_IN_USE] = "сейчас: %s",
    [UI_MENU_GPU] = "Видеокарта", [UI_MENU_GPU_DESC] = "Устройство Vulkan, которое рисует игру.",
    [UI_MENU_GPU_AUTO] = "Автоматически",
    [UI_MENU_PERF_OVERLAY] = "Индикатор производительности",
    [UI_MENU_PERF_OVERLAY_DESC] = "Частота кадров в правом верхнем углу. «Полный» добавляет время кадров с графиком, "
                                  "загрузку ЦП, GPU и памяти, температуры и ожидания потока отрисовки. F10 переключает "
                                  "режимы.",
    [UI_MENU_PERF_OFF] = "Выкл.", [UI_MENU_PERF_FPS] = "FPS", [UI_MENU_PERF_FULL] = "Полный",
    [UI_PERF_LINE_FPS] = "%.0f к/с",
    [UI_MENU_GRAPHICS] = "Изображение",
    [UI_MENU_OUTPUT_RES] = "Разрешение игры",
    [UI_MENU_OUTPUT_RES_DESC] = "Размер, в котором игра рисует картинку и интерфейс. Выше 1920 × 1080 игре нужно больше "
                                "видеопамяти и времени GPU.",
    [UI_MENU_RENDER_SCALE] = "Масштаб 3D-отрисовки",
    [UI_MENU_RENDER_SCALE_DESC] = "Размер 3D-сцены относительно разрешения игры; игра растягивает её до этого размера, "
                                  "а интерфейс рисует в полном. «Как в игре» рисует сцену целиком. "
                                  "Для суперсэмплинга задайте разрешение игры выше разрешения окна. Применяется после "
                                  "перезапуска.",
    [UI_MENU_RENDER_SCALE_GAME] = "Как в игре (целиком)",
    [UI_MENU_RENDER_SCALE_MODE] = "%s (%d %%)",
    [UI_MENU_MODE_ULTRA_PERF] = "Макс. производительность",
    [UI_MENU_MODE_PERF] = "Производительность",
    [UI_MENU_MODE_BALANCED] = "Баланс",
    [UI_MENU_MODE_QUALITY] = "Качество",
    [UI_MENU_UPSCALER] = "Масштабирование",
    [UI_MENU_UPSCALER_DESC] = "FSR 1 и билинейное растягивают картинку под окно, когда окно больше разрешения игры; "
                              "FSR 1 сохраняет края и детали чётче. Временные апскейлеры восстанавливают 3D-сцену "
                              "вместо собственной реконструкции игры, которая рисует её в половине разрешения игры "
                              "(или в масштабе рендера): AMD FSR 3 нужен amd_fidelityfx_vk.dll рядом с xc3.exe; "
                              "NVIDIA DLSS и DLAA нужны карта RTX и библиотеки Streamline рядом с xc3.exe, и они "
                              "включаются после перезапуска. DLAA рисует сцену в полном разрешении игры и только "
                              "сглаживает её.",
    [UI_MENU_UPSCALER_FSR] = "AMD FSR 1", [UI_MENU_UPSCALER_BILINEAR] = "Билинейное",
    [UI_MENU_UPSCALER_FSR3] = "AMD FSR 3 (временной)", [UI_MENU_UPSCALER_DLSS] = "NVIDIA DLSS",
    [UI_MENU_UPSCALER_DLAA] = "NVIDIA DLAA",
    [UI_MENU_FRAME_GEN] = "Генерация кадров",
    [UI_MENU_FRAME_GEN_DESC] = "Показывает сгенерированный кадр между каждыми двумя нарисованными, вдвое больше кадров "
                               "на экране; ограничение частоты кадров тогда считает показанные кадры, так что 120 рисует "
                               "60. Работает по глубине и движению временного апскейлера (AMD FSR 3, NVIDIA DLSS или "
                               "DLAA), с другими выключена, и пока открыто это меню. NVIDIA DLSS Frame Generation нужны "
                               "карта RTX 40 или новее, аппаратное планирование GPU и библиотеки Streamline; AMD FSR 3 "
                               "работает на любой карте с amd_fidelityfx_vk.dll. Включается после перезапуска.",
    [UI_MENU_FRAME_GEN_OFF] = "Выкл.", [UI_MENU_FRAME_GEN_DLSS] = "NVIDIA DLSS Frame Generation",
    [UI_MENU_FRAME_GEN_FSR] = "AMD FSR 3 (генерация кадров)",
    [UI_MENU_FRAME_GEN_MULTI] = "Множитель кадров DLSS",
    [UI_MENU_FRAME_GEN_MULTI_DESC] = "Сколько кадров показывается на каждый нарисованный игрой с NVIDIA DLSS Frame "
                                     "Generation: от 3x до 6x (Multi Frame Generation) требуют карту RTX 50, до "
                                     "того, что она поддерживает; на других остаётся 2x. Ограничение частоты считает показанные кадры: 240 при 4x рисует 60.",
    [UI_MENU_PICTURE_FIT] = "Пропорции картинки",
    [UI_MENU_PICTURE_FIT_DESC] = "Когда форма окна отличается от формы разрешения игры (игра 16:9 на экране Steam Deck "
                                 "16:10), сохранять пропорции картинки с чёрными полосами или растягивать её на всё окно.",
    [UI_MENU_PICTURE_FIT_KEEP] = "Сохранять (полосы)", [UI_MENU_PICTURE_FIT_STRETCH] = "Растягивать",
    [UI_MENU_SHARPNESS] = "Резкость FSR",
    [UI_MENU_SHARPNESS_DESC] = "Повышение резкости после масштабирования FSR; 0 выключает его.",
    [UI_MENU_DLSS_PRESET] = "Модель DLSS",
    [UI_MENU_DLSS_PRESET_DESC] = "Модель DLSS. По умолчанию — выбор NVIDIA для каждого режима (K для DLAA, качества и "
                                 "баланса, M для производительности, L для макс. производительности); K, J, L или M "
                                 "задают одну модель во всех режимах. J оставляет чуть меньше шлейфов за движущимися "
                                 "персонажами и чуть сильнее мерцает, чем K.",
    [UI_MENU_DLSS_PRESET_DEFAULT] = "По умолчанию",
    [UI_MENU_REFLEX] = "NVIDIA Reflex",
    [UI_MENU_REFLEX_DESC] = "Низкая задержка с DLSS или DLAA: игра начинает каждый кадр так поздно, как позволяет GPU, и "
                            "картинка ближе к вводу. Ускорение также держит частоты GPU высокими. С генерацией кадров "
                            "DLSS он всегда включён.",
    [UI_MENU_REFLEX_BOOST] = "Вкл. + ускорение",
    [UI_MENU_DYNAMIC_RES] = "Динамическое разрешение",
    [UI_MENU_DYNAMIC_RES_DESC] = "Собственное разрешение игры: 3D-сцена рисуется меньше, пока GPU не успевает за "
                                 "ограничением частоты кадров, и снова целиком, когда успевает. Лучше всего с DLSS или "
                                 "FSR 3, которые возвращают её к полному размеру.",
    [UI_MENU_HDR] = "HDR",
    [UI_MENU_HDR_DESC] = "Вывод HDR10: картинка игры расширяется до диапазона вашего дисплея. Нужен включённый HDR "
                         "в Windows. Требует перезапуска.",
    [UI_MENU_HDR_PEAK] = "Пиковая яркость HDR",
    [UI_MENU_HDR_PEAK_DESC] = "Пиковая яркость дисплея в нитах; её достигают самые яркие блики.",
    [UI_MENU_HDR_GAME] = "Белый сцены HDR",
    [UI_MENU_HDR_GAME_DESC] = "Яркость белого сцены в нитах до расширения бликов.",
    [UI_MENU_HDR_UI] = "Яркость интерфейса HDR",
    [UI_MENU_HDR_UI_DESC] = "Яркость интерфейса игры и этого меню в нитах.",
    [UI_MENU_HDR_HIGHLIGHTS] = "Блики HDR",
    [UI_MENU_HDR_HIGHLIGHTS_DESC] = "Насколько яркие части сцены поднимаются к пику; 0 оставляет картинку SDR.",
    [UI_MENU_HDR_SATURATION] = "Насыщенность HDR",
    [UI_MENU_HDR_SATURATION_DESC] = "Насыщенность цветов сцены; 100 % оставляет их как в игре.",
    [UI_MENU_HDR_GAMMA] = "Гамма HDR",
    [UI_MENU_HDR_GAMMA_DESC] = "Как декодируются цвета игры: 2.2 как у большинства SDR-дисплеев, sRGB светлее в тенях.",

    [UI_MENU_FPS_LIMIT] = "Ограничение частоты кадров",
    [UI_MENU_FPS_LIMIT_DESC] = "Наибольшее число кадров в секунду; часы игры идут по нему. Выше 60 — экспериментальный "
                               "режим.",
    [UI_MENU_FPS_30] = "30 FPS", [UI_MENU_FPS_40] = "40 FPS", [UI_MENU_FPS_45] = "45 FPS",
    [UI_MENU_FPS_60] = "60 FPS", [UI_MENU_FPS_90] = "90 FPS", [UI_MENU_FPS_120] = "120 FPS", [UI_MENU_FPS_144] = "144 FPS",
    [UI_MENU_FPS_165] = "165 FPS", [UI_MENU_FPS_240] = "240 FPS", [UI_MENU_FPS_360] = "360 FPS",
    [UI_MENU_FPS_DISPLAY] = "Как у монитора", [UI_MENU_FPS_DISPLAY_HZ] = "Как у монитора (%.0f Гц)",
    [UI_MENU_FRAME_QUEUE] = "Очередь кадров",
    [UI_MENU_FRAME_QUEUE_DESC] = "Игра начинает кадр, пока предыдущий ждёт показа: намного меньше пропущенных кадров на "
                                 "лимите, задержка ввода больше не более чем на кадр. Применяется сразу.",
    [UI_MENU_CUTSCENE_FPS] = "Частота кадров в роликах",
    [UI_MENU_CUTSCENE_FPS_DESC] = "30 кадров: все ролики в 30, как на консоли. Смешанно: большинство игровых роликов с "
                                  "ограничением частоты кадров, остальные и видео в 30. Ограничение: все ролики с "
                                  "ограничением (экспериментально). Действует после перезапуска.",
    [UI_MENU_CUTSCENE_30] = "30 кадров", [UI_MENU_CUTSCENE_MIXED] = "Смешанно", [UI_MENU_CUTSCENE_LIMIT] = "Ограничение",
    [UI_MENU_REALTIME] = "Скорость игры по часам",
    [UI_MENU_REALTIME_DESC] = "Игра идёт по реальному времени, медленный кадр её не замедляет.",
    [UI_MENU_CLOTH_REALTIME] = "Ткань по часам",
    [UI_MENU_CLOTH_REALTIME_DESC] = "Ткань и волосы тоже идут по реальному времени.",
    [UI_MENU_HIGHFPS_FIX] = "Исправление физики на высоком FPS",
    [UI_MENU_HIGHFPS_FIX_DESC] = "Исправляет физику, которая ломается при частоте выше 30 кадров в секунду.",
    [UI_MENU_GAME_LANG] = "Язык игры", [UI_MENU_GAME_LANG_DESC] = "Язык текста и озвучки игры.",
    [UI_MENU_GAME_LANG_AUTO] = "Автоматически",
    [UI_MENU_OPERATION_MODE] = "Режим консоли",
    [UI_MENU_OPERATION_MODE_DESC] = "Игра считает, что консоль в док-станции или в портативном режиме.",
    [UI_MENU_MODE_DOCKED] = "В док-станции", [UI_MENU_MODE_HANDHELD] = "Портативный",
    [UI_MENU_MOD_ARCHIVE] = "Моды заменяют файлы архивов",
    [UI_MENU_MOD_ARCHIVE_DESC] = "Файлы из папки mods заменяют файлы внутри архивов игры (тексты, шрифты, модели), как плагин xc3-file-loader в эмуляторе. Выкл.: моды заменяют только видео и звуковые банки.",

    [UI_MENU_VOLUME] = "Громкость", [UI_MENU_VOLUME_DESC] = "Общая громкость игры.",

    [UI_MENU_MOTION] = "Управление движением", [UI_MENU_MOTION_DESC] = "Прицеливание гироскопом контроллера.",
    [UI_MENU_RUMBLE_STRENGTH] = "Сила вибрации",
    [UI_MENU_RUMBLE_STRENGTH_DESC] = "Насколько сильно вибрирует контроллер.",
    [UI_MENU_RUMBLE_CURVE] = "Кривая вибрации",
    [UI_MENU_RUMBLE_CURVE_DESC] = "Чем ниже значение, тем сильнее слабая вибрация.",
    [UI_MENU_RUMBLE_TRIGGERS] = "Вибрация триггеров (Xbox)",
    [UI_MENU_RUMBLE_TRIGGERS_DESC] = "Вибрация и в триггерах контроллера Xbox.",
    [UI_MENU_PAD_LAYOUT] = "Раскладка кнопок",
    [UI_MENU_PAD_LAYOUT_DESC] = "Какая кнопка работает как A. Nintendo: правая, как на Switch. Xbox: нижняя, как "
                                "подписано на контроллере Xbox (крест на контроллере PlayStation). Автоматически: "
                                "Nintendo для любого контроллера, чтобы приёмы были там, где их показывает игра.",
    [UI_MENU_PAD_LAYOUT_AUTO] = "Автоматически", [UI_MENU_PAD_LAYOUT_NINTENDO] = "Nintendo (A справа)",
    [UI_MENU_PAD_LAYOUT_XBOX] = "Xbox (A снизу)", [UI_MENU_PAD_LAYOUT_IN_USE] = "сейчас: %s",
    [UI_MENU_PROMPTS] = "Значки кнопок",
    [UI_MENU_PROMPTS_DESC] = "Значки кнопок в игре. Автоматически: по контроллеру, подключённому при запуске. "
                             "Применяется при следующем запуске.",
    [UI_MENU_PROMPTS_AUTO] = "Автоматически", [UI_MENU_PROMPTS_SWITCH] = "Switch", [UI_MENU_PROMPTS_XBOX] = "Xbox",
    [UI_MENU_PROMPTS_PLAYSTATION] = "PlayStation",
    [UI_MENU_MOUSE_SENS] = "Чувствительность мыши",
    [UI_MENU_MOUSE_SENS_DESC] = "Как быстро мышь поворачивает камеру.",

    [UI_BINDS_HELP] = "Выберите ячейку и нажмите A или Enter, затем клавишу, кнопку мыши или кнопку контроллера. "
                      "Delete или X очищает ячейку. Занятая клавиша переходит сюда; кнопка контроллера меняется "
                      "местами с той, которую заменяет.",
    [UI_BINDS_COL_KEY1] = "Клавиша 1", [UI_BINDS_COL_KEY2] = "Клавиша 2", [UI_BINDS_COL_PAD] = "Контроллер",
    [UI_BINDS_GROUP_BUTTONS] = "Кнопки", [UI_BINDS_GROUP_MOVE] = "Движение", [UI_BINDS_GROUP_CAMERA] = "Камера",
    [UI_BINDS_PRESS_KEY] = "Нажмите клавишу или кнопку мыши для «%s»",
    [UI_BINDS_PRESS_PAD] = "Нажмите кнопку контроллера для «%s»",
    [UI_BINDS_CANCEL_HINT] = "Esc отменяет, или подождите %d с",
    [UI_BINDS_TAKEN] = "%s: убрано с «%s»", [UI_BINDS_SWAPPED] = "«%s» и «%s» поменялись местами",
    [UI_BINDS_RESET] = "Сбросить все кнопки",
    [UI_BINDS_RESET_DESC] = "Вернуть клавиши и кнопки контроллера по умолчанию. Снова действует раскладка кнопок выше: "
                            "переназначенные кнопки контроллера остаются на своих местах до "
                            "этого сброса.",
    [UI_BIND_A] = "A", [UI_BIND_B] = "B", [UI_BIND_X] = "X", [UI_BIND_Y] = "Y",
    [UI_BIND_LCLICK] = "Нажатие левого стика", [UI_BIND_RCLICK] = "Нажатие правого стика", [UI_BIND_L] = "L",
    [UI_BIND_R] = "R", [UI_BIND_ZL] = "ZL", [UI_BIND_ZR] = "ZR", [UI_BIND_PLUS] = "Плюс", [UI_BIND_MINUS] = "Минус",
    [UI_BIND_LEFT] = "Крестовина влево", [UI_BIND_UP] = "Крестовина вверх", [UI_BIND_RIGHT] = "Крестовина вправо",
    [UI_BIND_DOWN] = "Крестовина вниз",
    [UI_BIND_LSTICK_LEFT] = "Влево", [UI_BIND_LSTICK_UP] = "Вперёд", [UI_BIND_LSTICK_RIGHT] = "Вправо",
    [UI_BIND_LSTICK_DOWN] = "Назад",
    [UI_BIND_RSTICK_LEFT] = "Камера влево", [UI_BIND_RSTICK_UP] = "Камера вверх",
    [UI_BIND_RSTICK_RIGHT] = "Камера вправо", [UI_BIND_RSTICK_DOWN] = "Камера вниз",
    [UI_BIND_DESC_A] = "Взаимодействие, подобрать, поговорить.", [UI_BIND_DESC_B] = "Бег, отмена, назад.",
    [UI_BIND_DESC_X] = "Прыжок.", [UI_BIND_DESC_Y] = "Атака.", [UI_BIND_DESC_LCLICK] = "Присесть.",
    [UI_BIND_DESC_RCLICK] = "Вид от первого лица.", [UI_BIND_DESC_L] = "Способности.",
    [UI_BIND_DESC_R] = "Бросок оружия.", [UI_BIND_DESC_ZL] = "Щит, захват цели.", [UI_BIND_DESC_ZR] = "Лук.",
    [UI_BIND_DESC_PLUS] = "Меню, инвентарь.", [UI_BIND_DESC_MINUS] = "Карта.",
    [UI_BIND_DESC_DPAD] = "Быстрый выбор.",
    [UI_BIND_DESC_MOVE] = "Двигает Линка, как левый стик. Левый стик контроллера тоже всегда работает.",
    [UI_BIND_DESC_CAMERA] = "Поворачивает камеру, как правый стик. Мышь и правый стик контроллера тоже всегда "
                            "работают.",
    [UI_KEY_SPACE] = "Пробел", [UI_KEY_TAB] = "Tab", [UI_KEY_LSHIFT] = "Левый Shift", [UI_KEY_LCTRL] = "Левый Ctrl",
    [UI_KEY_UP] = "Стрелка вверх", [UI_KEY_DOWN] = "Стрелка вниз", [UI_KEY_LEFT] = "Стрелка влево",
    [UI_KEY_RIGHT] = "Стрелка вправо",
    [UI_KEY_MOUSE_L] = "ЛКМ", [UI_KEY_MOUSE_R] = "ПКМ", [UI_KEY_MOUSE_M] = "СКМ", [UI_KEY_MOUSE_X1] = "Мышь 4",
    [UI_KEY_MOUSE_X2] = "Мышь 5",
    [UI_KEY_ENTER] = "Enter", [UI_KEY_BACKSPACE] = "Backspace", [UI_KEY_RSHIFT] = "Правый Shift",
    [UI_KEY_RCTRL] = "Правый Ctrl", [UI_KEY_LALT] = "Alt", [UI_KEY_CAPSLOCK] = "Caps Lock", [UI_KEY_INSERT] = "Insert",
    [UI_KEY_DELETE] = "Delete", [UI_KEY_HOME] = "Home", [UI_KEY_END] = "End", [UI_KEY_PAGEUP] = "Page Up",
    [UI_KEY_PAGEDOWN] = "Page Down", [UI_KEY_NUM_FMT] = "Num %s",
    [UI_PAD_SOUTH] = "Нижняя кнопка", [UI_PAD_EAST] = "Правая кнопка", [UI_PAD_WEST] = "Левая кнопка",
    [UI_PAD_NORTH] = "Верхняя кнопка", [UI_PAD_LSTICK] = "Левый стик", [UI_PAD_RSTICK] = "Правый стик",
    [UI_PAD_L] = "Левый бампер", [UI_PAD_R] = "Правый бампер", [UI_PAD_START] = "Start", [UI_PAD_BACK] = "Back",
    [UI_PAD_DPAD_LEFT] = "Крестовина влево", [UI_PAD_DPAD_UP] = "Крестовина вверх",
    [UI_PAD_DPAD_RIGHT] = "Крестовина вправо", [UI_PAD_DPAD_DOWN] = "Крестовина вниз", [UI_PAD_ZL] = "Левый курок",
    [UI_PAD_ZR] = "Правый курок",
    [UI_PAD_CROSS] = "Крест", [UI_PAD_CIRCLE] = "Круг", [UI_PAD_SQUARE] = "Квадрат", [UI_PAD_TRIANGLE] = "Треугольник",

    [UI_MENU_MENU_LANG] = "Язык меню", [UI_MENU_MENU_LANG_DESC] = "Язык этого меню и индикатора.",
    [UI_MENU_MENU_LANG_AUTO] = "Автоматически (из Windows)",

    [UI_MENU_ADV_WARNING] = "Эти настройки нужны для тестов и поиска неполадок. Меняйте их, только если знаете зачем.",
    [UI_MENU_GROUP_SCHED] = "Планирование", [UI_MENU_GROUP_SHADERS] = "Шейдеры и конвейеры",
    [UI_MENU_GROUP_TEXMEM] = "Текстуры и память", [UI_MENU_GROUP_FIXES] = "Исправления графики",
    [UI_MENU_GROUP_VULKAN] = "Vulkan", [UI_MENU_GROUP_DIAG] = "Диагностика",
    [UI_MENU_ASYNC_PREEMPT] = "Вытеснение на виртуальных ядрах",
    [UI_MENU_ASYNC_PREEMPT_DESC] = "Позволяет потоку игры прервать другой поток на том же виртуальном ядре.",
    [UI_MENU_PREEMPT_OFF] = "Выкл.", [UI_MENU_PREEMPT_ON] = "Вкл.", [UI_MENU_PREEMPT_INHERIT] = "Только наследование",
    [UI_MENU_REPLAY_ASYNC] = "Подготовка кадра для GPU в отдельном потоке",
    [UI_MENU_REPLAY_ASYNC_DESC] = "Кадр для GPU готовится в отдельном потоке, а не в потоке игры. Добавляет до одного кадра задержки.",
    [UI_MENU_FREE_CORES] = "Ограничение виртуальных ядер",
    [UI_MENU_FREE_CORES_DESC] = "На каждом виртуальном ядре работает один поток, как на консоли.",
    [UI_MENU_TIMER_1MS] = "Таймер Windows 1 мс",
    [UI_MENU_TIMER_1MS_DESC] = "Повышает точность системного таймера до 1 мс для ровного темпа кадров.",
    [UI_MENU_PIPE_WAIT] = "Обработка ожидающих отрисовок",
    [UI_MENU_PIPE_WAIT_DESC] = "«По умолчанию» — текущее планирование с отдельным потоком для ожидающих отрисовок; «Старый способ» — прежнее планирование.",
    [UI_MENU_PIPE_WAIT_DEFAULT] = "По умолчанию", [UI_MENU_PIPE_WAIT_DRAW] = "Старый способ",
    [UI_MENU_PIPE_SKIP] = "Пропуск отрисовок при сборке конвейера",
    [UI_MENU_PIPE_SKIP_DESC] = "Какие отрисовки отбрасываются, а не ждут свой конвейер.",
    [UI_MENU_PIPE_SKIP_STRICT] = "Строго", [UI_MENU_PIPE_SKIP_ANY] = "Любые",
    [UI_MENU_PIPE_SKIP_FULLSCREEN] = "Полноэкранные проходы",
    [UI_MENU_LIB_PREBUILD] = "Предсборка библиотек стадий",
    [UI_MENU_LIB_PREBUILD_DESC] = "Заранее собирает библиотеки стадий шейдеров в фоне.",
    [UI_MENU_LIB_PREBUILD_THREADS] = "Потоки предсборки библиотек",
    [UI_MENU_LIB_PREBUILD_THREADS_DESC] = "Сколько потоков занято предсборкой; «Автоматически» — по числу ядер процессора.",
    [UI_MENU_PIPE_PREBUILD] = "Предсборка конвейеров",
    [UI_MENU_PIPE_PREBUILD_DESC] = "Заранее собирает известные конвейеры.",
    [UI_MENU_PREWARM_XLATE] = "Прогрев транслятора",
    [UI_MENU_PREWARM_XLATE_DESC] = "Потоки, которые при запуске переводят известные шейдеры; «Автоматически» — половина потоков процессора, не больше 16.",
    [UI_MENU_GPL] = "Библиотека графических конвейеров",
    [UI_MENU_GPL_DESC] = "Использует расширение Vulkan для библиотек конвейеров; выключите, если драйвер с ним сбоит.",
    [UI_MENU_ASTC] = "Распаковка текстур ASTC", [UI_MENU_ASTC_DESC] = "Где распаковываются сжатые текстуры ASTC.",
    [UI_MENU_ASTC_AUTO] = "Автоматически", [UI_MENU_ASTC_GPU] = "Шейдер GPU", [UI_MENU_ASTC_NATIVE] = "Аппаратно",
    [UI_MENU_ASTC_CPU] = "Процессор",
    [UI_MENU_MEMORY] = "Режим памяти GPU", [UI_MENU_MEMORY_DESC] = "Как память игры передаётся видеокарте.",
    [UI_MENU_MEMORY_AUTO] = "Автоматически", [UI_MENU_MEMORY_IMPORT] = "Импорт", [UI_MENU_MEMORY_MIRROR] = "Зеркало",
    [UI_MENU_MEMORY_VERIFY] = "Проверка",
    [UI_MENU_TEX_ALIAS] = "Наследование содержимого текстур", [UI_MENU_TEX_ALIAS_DESC] = "Новая текстура поверх памяти живой текстуры получает её содержимое, а не пустую.",
    [UI_MENU_LOD_GUARD] = "Защита LOD", [UI_MENU_LOD_GUARD_DESC] = "Не даёт объектам мелькать не на своём месте, а дальнему плану — на кадр становиться плоским: ждёт, пока игра допишет данные уровней детализации, и только потом их читает GPU.",
    [UI_MENU_FIX_C0] = "Исправление константного буфера",
    [UI_MENU_FIX_C0_DESC] = "Заполняет зарезервированный драйвером банк констант c0 (размеры хранилищ, размер запуска, размер точки, базовый экземпляр) вместо чтения нулей.",
    [UI_MENU_FIX_ZERO] = "Обнулять новую память GPU",
    [UI_MENU_FIX_ZERO_DESC] = "Новые пулы памяти GPU заполняются нулями, как память Switch.",
    [UI_MENU_BIND_SKIP] = "Пропуск повторных привязок",
    [UI_MENU_BIND_SKIP_DESC] = "Оптимизация: пропускает неизменившиеся привязки.",
    [UI_MENU_SHADER_MEMO] = "Память шейдеров", [UI_MENU_SHADER_MEMO_DESC] = "Оптимизация: запоминает поиск шейдеров.",
    [UI_MENU_PIPE_MEMO] = "Память конвейера", [UI_MENU_PIPE_MEMO_DESC] = "Оптимизация: повторяет конвейер прошлой отрисовки, если её состояние не изменилось.",
    [UI_MENU_TEX_MEMO] = "Память привязок текстур", [UI_MENU_TEX_MEMO_DESC] = "Оптимизация: запоминает дескрипторы текстур между отрисовками.",
    [UI_MENU_DESC_TEMPLATE] = "Шаблоны дескрипторов",
    [UI_MENU_DESC_TEMPLATE_DESC] = "Оптимизация: обновляет дескрипторы пачками.",
    [UI_MENU_PUSH_SKIP] = "Пропуск повторных push-констант",
    [UI_MENU_PUSH_SKIP_DESC] = "Оптимизация: пропускает неизменившиеся push-константы.",
    [UI_MENU_SPLIT_DRAWS] = "Ранняя отправка работы GPU",
    [UI_MENU_SPLIT_DRAWS_DESC] = "Отправляет работу GPU частями через каждые N отрисовок между проходами, чтобы GPU начинал раньше; 0 — выключено.",
    [UI_MENU_VK_VALIDATION] = "Слой проверки",
    [UI_MENU_VK_VALIDATION_DESC] = "Проверяет каждый вызов Vulkan и сильно замедляет игру.",
    [UI_MENU_VK_VALIDATION_MISSING] = "Слой проверки не установлен.",
    [UI_MENU_VK_DYN_VERTEX] = "Динамическое состояние вершин",
    [UI_MENU_VK_DYN_VERTEX_DESC] = "Принудительно включает или выключает динамический ввод вершин; по умолчанию зависит от GPU.",
    [UI_MENU_VK_DYN_A2C] = "Динамический alpha to coverage",
    [UI_MENU_VK_DYN_A2C_DESC] = "Принудительно включает или выключает динамический alpha to coverage. По умолчанию "
                                "выключен на драйвере AMD для RDNA 2 и более старых GPU, который применяет alpha to "
                                "coverage, даже когда он выключен, и включён на остальных.",
    [UI_MENU_VK_SUBGROUP_PIN] = "Фиксировать размер подгруппы",
    [UI_MENU_VK_SUBGROUP_PIN_DESC] = "Фиксирует размер подгруппы шейдера.",
    [UI_MENU_TRACE] = "Журнал производительности", [UI_MENU_TRACE_DESC] = "Пишет время кадров в файл.",
    [UI_MENU_FLIGHT] = "Запись последних кадров (для отчёта о фризе)",
    [UI_MENU_FLIGHT_DESC] = "Хранит короткую историю и сохраняет её, когда кадр подвисает.",
    [UI_MENU_FLIGHT_SPIKE_MS] = "Порог подвисания (мс)",
    [UI_MENU_FLIGHT_SPIKE_MS_DESC] = "Время кадра, которое считается подвисанием.",
    [UI_MENU_INPUT_RECORD] = "Запись ввода",
    [UI_MENU_INPUT_RECORD_DESC] = "Пишет ваш ввод в файл для отчёта об ошибке.",
    [UI_MENU_OPEN_LOG] = "Открыть папку журналов", [UI_MENU_OPEN_LOG_DESC] = "Показывает папку с файлами журнала.",
    [UI_MENU_COPY_INFO] = "Копировать сведения о системе",
    [UI_MENU_COPY_INFO_DESC] = "Копирует в буфер обмена видеокарту, драйвер, процессор, память и изменённые настройки.",
    [UI_MENU_COPY_INFO_DONE] = "Скопировано", [UI_MENU_ACTION_FAILED] = "не получилось",
    [UI_MENU_RESET] = "Сбросить все настройки", [UI_MENU_RESET_DESC] = "Действует на все вкладки.",
    [UI_MENU_RESET_CONFIRM] = "Сбросить все настройки к значениям по умолчанию?", [UI_MENU_RESET_YES] = "Сбросить",
    [UI_MENU_RESET_NO] = "Отмена",

    [UI_MENU_OV_FPS] = "%.0f FPS", [UI_MENU_OV_FRAME_MS] = "%.1f мс", [UI_MENU_OV_GAME_PCT] = "игра %.0f%%",
    [UI_MENU_OV_MAX_MS] = "макс. %.1f мс",
    [UI_MENU_OV_GPU] = "GPU", [UI_MENU_OV_CPU] = "ЦП", [UI_MENU_OV_RAM] = "ОЗУ", [UI_MENU_OV_VRAM] = "Видеопамять",
    [UI_MENU_OV_TEMP] = "Температура", [UI_MENU_OV_POWER] = "Мощность", [UI_MENU_OV_CLOCK] = "Частота",
    [UI_MENU_OV_NONE] = "-",
    [UI_MENU_OV_REPLAY] = "Подготовка кадра для GPU", [UI_MENU_OV_GPU_TIME] = "Занятость GPU за кадр",
    [UI_MENU_OV_PIPE_WAIT] = "Ожидания конвейеров",
    [UI_MENU_OV_SHADER_WAIT] = "Ожидания шейдеров", [UI_MENU_OV_TEX_UPLOADS] = "Загрузки текстур",
    [UI_MENU_OV_FMT_PCT] = "%.0f%%", [UI_MENU_OV_FMT_TEMP] = "%.0f °C", [UI_MENU_OV_FMT_POWER] = "%.0f Вт",
    [UI_MENU_OV_FMT_MHZ] = "%.0f МГц", [UI_MENU_OV_FMT_GB] = "%.1f ГБ", [UI_MENU_OV_FMT_GB_OF] = "/ %.1f ГБ",
    [UI_MENU_OV_GAME_GB] = "игра %.1f ГБ", [UI_MENU_OV_FMT_MS] = "%.2f мс", [UI_MENU_OV_PER_FRAME] = "%.1f за кадр",
    [UI_MENU_OV_WINDOW] = "Окно", [UI_MENU_OV_OUTPUT] = "Изображение игры", [UI_MENU_OV_SCENE] = "3D-сцена",
    [UI_MENU_OV_SCALING] = "Масштабирование", [UI_MENU_OV_LIMIT] = "Лимит кадров", [UI_MENU_OV_PRESENT] = "Вывод кадров",
    [UI_MENU_OV_FMT_SIZE] = "%ux%u", [UI_MENU_OV_FMT_SCENE] = "%ux%u, %.0f%%",
    [UI_MENU_OV_FMT_FSR] = "AMD FSR 1, резкость %d%%", [UI_MENU_OV_SCALE_NONE] = "нет (1:1)",
    [UI_MENU_OV_SCALE_BILINEAR] = "билинейное",
};

#define SWKBD_ONLY(name, ...) static const char *const name[UI_TEXT_COUNT] = {SWKBD(__VA_ARGS__)}
SWKBD_ONLY(text_ja, "文字入力", "名前を入力してください", "ウマ", "OK", "キャンセル", "コントローラー: A = OK、B = キャンセル");
SWKBD_ONLY(text_fr, "Saisie de texte", "Entrez un nom", "Cheval", "OK", "Annuler", "Manette : A = OK, B = Annuler");
SWKBD_ONLY(text_de, "Texteingabe", "Gib einen Namen ein", "Pferd", "OK", "Abbrechen", "Controller: A = OK, B = Abbrechen");
SWKBD_ONLY(text_it, "Inserimento testo", "Inserisci un nome", "Cavallo", "OK", "Annulla", "Controller: A = OK, B = Annulla");
SWKBD_ONLY(text_es, "Introducir texto", "Introduce un nombre", "Caballo", "Aceptar", "Cancelar",
           "Mando: A = Aceptar, B = Cancelar");
SWKBD_ONLY(text_nl, "Tekstinvoer", "Voer een naam in", "Paard", "OK", "Annuleren", "Controller: A = OK, B = Annuleren");
SWKBD_ONLY(text_pt, "Inserir texto", "Introduz um nome", "Cavalo", "OK", "Cancelar", "Comando: A = OK, B = Cancelar");
SWKBD_ONLY(text_zh_hans, "文字输入", "请输入名字", "马儿", "确定", "取消", "手柄：A 确定，B 取消");
SWKBD_ONLY(text_zh_hant, "文字輸入", "請輸入名字", "馬兒", "確定", "取消", "手把：A 確定，B 取消");
SWKBD_ONLY(text_ko, "문자 입력", "이름을 입력하세요", "말", "확인", "취소", "컨트롤러: A 확인, B 취소");

static const Row rows[] = {
    {"en", text_en}, {"ja", text_ja}, {"fr", text_fr}, {"de", text_de}, {"it", text_it}, {"es", text_es},
    {"nl", text_nl}, {"pt", text_pt}, {"pt-BR", text_pt_br}, {"ru", text_ru}, {"zh-Hans", text_zh_hans}, {"zh-Hant", text_zh_hant},
    {"ko", text_ko},
};

const char *ui_text_lang(const char *code) {
    static const struct { const char *code, *lang; } map[] = {
        {"en-US", "en"}, {"en-GB", "en"}, {"fr-CA", "fr"}, {"es-419", "es"}, {"pt-BR", "pt"},
        {"zh-CN", "zh-Hans"}, {"zh-TW", "zh-Hant"},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(code, map[i].code)) return map[i].lang;
    return code;
}

static const Row *find_row(const char *lang) {
    for (size_t i = 0; i < sizeof rows / sizeof *rows; i++)
        if (!strcmp(rows[i].lang, lang)) return &rows[i];
    return NULL;
}

int ui_text_defined(const char *lang, UiTextId id) {
    const Row *r = find_row(lang);
    return (unsigned)id < UI_TEXT_COUNT && r && r->text[id] && *r->text[id];
}

const char *ui_text_in(const char *lang, UiTextId id) {
    if ((unsigned)id >= UI_TEXT_COUNT) return "";
    return ui_text_defined(lang, id) ? find_row(lang)->text[id] : rows[0].text[id];
}

const char *ui_text(UiTextId id) { return ui_text_in(ui_text_lang(ov_language()), id); }

// The menu language, re-read when cfg_version changes. XC3_MENU_LANG "ru", "pt-BR" or "en"; anything else (unset
// included) follows the system UI language: Russian for "ru", "ru-RU", ..., Brazilian Portuguese for "pt", "pt-BR",
// "pt-PT", ..., else English.
static const char *resolve_menu_lang(void) {
    const char *v = cfg_value("XC3_MENU_LANG");
    static const char *const set[] = {"en", "ru", "pt-BR"};
    for (size_t i = 0; v && i < sizeof set / sizeof *set; i++)
        if (!strcmp(v, set[i])) return set[i];
    const char *sys = plat_ui_language();
    if (sys && (!strcmp(sys, "ru") || !strncmp(sys, "ru-", 3))) return "ru";
    if (sys && (!strcmp(sys, "pt") || !strncmp(sys, "pt-", 3))) return "pt-BR";
    return "en";
}

const char *menu_text(UiTextId id) {
    static _Atomic(const char *) lang = "en";
    static _Atomic unsigned seen = ~0u;
    unsigned v = cfg_version();
    if (v != atomic_load_explicit(&seen, memory_order_relaxed)) {
        atomic_store_explicit(&lang, resolve_menu_lang(), memory_order_relaxed);
        atomic_store_explicit(&seen, v, memory_order_relaxed);
    }
    return ui_text_in(atomic_load_explicit(&lang, memory_order_relaxed), id);
}
