#include "ui_surface.hpp"

namespace gw2::surface {

Surface &active() {
    static Surface surface;
    return surface;
}

#ifndef GW2_HAS_PRIVATE_UI
void install() {}
#endif

} // namespace gw2::surface
