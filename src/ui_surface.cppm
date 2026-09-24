module;
#include <imgui.h>

export module ui_surface;

export namespace gw2::surface {

enum class Part : unsigned {
    header = 1u << 0,
    toolbar = 1u << 1,
    error = 1u << 2,
    loading = 1u << 3,
    emptyState = 1u << 4,
    accounts = 1u << 5,
    settings = 1u << 6,
    updatePrompt = 1u << 7,
};

enum class Insert : unsigned {
    beforeHeader,
    afterHeader,
    beforeToolbar,
    afterToolbar,
    beforePage,
    afterPage,
    settingsTail,
    count,
};

struct Palette {
    ImVec4 background{0.055f, 0.067f, 0.082f, 1};
    ImVec4 accent{0.68f, 0.55f, 1, 1};
    ImVec4 primary{0.44f, 0.22f, 0.86f, 1};
    ImVec4 launch{0.09f, 0.45f, 0.25f, 1};
    ImVec4 close{0.72f, 0.17f, 0.28f, 1};
    ImVec4 muted{0.54f, 0.59f, 0.65f, 1};
    ImVec4 error{1, 0.55f, 0.48f, 1};
};

using Paint = void (*)();
using Style = void (*)(ImGuiStyle &style);

struct Surface {
    unsigned hidden = 0;
    Palette palette{};
    Style style = nullptr;
    Paint insert[static_cast<unsigned>(Insert::count)]{};
    // Main-thread lifecycle for optional services, including cancellation before SDL teardown.
    void (*poll)() = nullptr;
    void (*shutdown)() noexcept = nullptr;
    const char *brand = "GW2";
    const char *brandRest = "Launcher";
    const char *home = "https://github.com/Dvow/gw2-multi-launcher";
};

Surface &active();
void install();

inline bool shown(Part part) {
    return (active().hidden & static_cast<unsigned>(part)) == 0;
}

inline void paint(Insert slot) {
    if (const auto draw = active().insert[static_cast<unsigned>(slot)]) draw();
}

} // namespace gw2::surface

namespace gw2::surface {
Surface &active() {
    static Surface surface;
    return surface;
}
} // namespace gw2::surface

#ifndef GW2_HAS_UI_EXT
extern "C" void gw2ApplySurface() {}
#else
extern "C" void gw2ApplySurface();
#endif

void gw2::surface::install() {
    gw2ApplySurface();
}
