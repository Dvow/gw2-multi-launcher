export module data_folder;

export const char *&gw2ConfigFolder() {
    static const char *name = "GW2 Multi Launcher";
    return name;
}

export const char *&gw2ProductName() {
    static const char *name = "GW2 Multi Launcher";
    return name;
}

#ifndef GW2_HAS_UI_EXT
extern "C" void gw2ApplyNames() {}
#else
extern "C" void gw2ApplyNames();
#endif

namespace {
struct ApplyNames {
    ApplyNames() { gw2ApplyNames(); }
};
const ApplyNames applyNames;
}
