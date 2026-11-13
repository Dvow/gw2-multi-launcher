#include "../native/patch_flags.hpp"
#include <iostream>
#include <stdexcept>

using gw2::patch_flags::inSettings;
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
    try {
        // Each "run:" row is a Settings.json the game was started with on 2026-11-12
        // (toolbox gw2-hidden-run); the expected value is what GW2 then did.
        require(inSettings(R"({"arguments":["-shareArchive"]})") == 1, "run: -shareArchive stops the patch");
        require(inSettings(R"({"arguments":["-SHAREARCHIVE"]})") == 1, "run: letter case does not matter");
        require(inSettings(R"({"arguments":["/shareArchive"]})") == 1, "run: a slash names a flag too");
        require(inSettings(R"({"arguments":["-windowed -shareArchive"]})") == 1, "run: one string may hold several flags");
        require(inSettings(R"({"arguments":["-noPatch"]})") == 2, "run: -noPatch stops the archive patch");
        require(inSettings(R"({"arguments":"-shareArchive"})") == 0, "run: a string instead of a list is not read");
        require(inSettings(R"({"arguments":["-shareArch"]})") == 0, "run: a shortened name is no flag");
        require(inSettings(R"({"arguments":["-multi"]})") == 0, "run: -multi still patches");
        require(inSettings(R"({"arguments":["-log"]})") == 0, "run: other flags pass");

        require(inSettings(R"({"arguments":["-noPatch","-bmp","-shareArchive"]})") == 3, "both flags are reported");
        require(inSettings(R"({"arguments":[60,true,null,"-noPatch"]})") == 2, "values that are not text name no flag");
        require(inSettings(R"({"arguments":["\"-shareArchive\""]})") == 1, "a quoted flag is still that flag");
        require(inSettings(R"({"arguments":["-dat \"D:\\Games\\a -shareArchive b\\Gw2.dat\""]})") == 0,
            "a quoted path holding the word is not the flag");
        require(inSettings(R"({"arguments":["-shareArchiveX","shareArchive","--shareArchive","-nopatching"]})") == 0,
            "only the whole name after one - or / counts");
        require(inSettings("\xEF\xBB\xBF" R"({"arguments":["-shareArchive"]})") == 1, "a UTF-8 byte-order mark is skipped");
        require(inSettings(R"({"Arguments":["-shareArchive"]})") == 0, "another key is not the list");
        require(inSettings(R"({"other":{"arguments":["-shareArchive"]}})") == 0, "only the top-level list counts");
        require(inSettings("") == 0 && inSettings("-shareArchive") == 0 && inSettings(R"(["-shareArchive"])") == 0 &&
                    inSettings(R"({"arguments":["-shareArchive")") == 0,
            "text that is not a settings object holds no flag");
        std::cout << "Patch flag tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
