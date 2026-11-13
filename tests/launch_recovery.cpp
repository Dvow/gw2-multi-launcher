#include "../src/launch_recovery.hpp"
#include <iostream>
#include <stdexcept>

using gw2::recovery::Recovery;
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
    try {
        const auto all = [](const std::string &) { return true; };
        Recovery recovery;
        require(!recovery.resume(all), "nothing resumes before a launch stops");
        recovery.stop({{"a", "b"}});
        recovery.cancel();
        require(!recovery.open() && !recovery.resume(all), "cancel launches nothing");
        recovery.stop({{"a", "b"}});
        recovery.save(7);
        require(!recovery.finished(6, false, all) && recovery.saving(), "another command does not resume");
        require(!recovery.finished(7, true, all) && recovery.open() && !recovery.saving(),
            "a failed save keeps the request");
        recovery.save(8);
        const auto resumed = recovery.finished(8, false, all);
        require(resumed && resumed->ids == std::vector<std::string>{"a", "b"},
            "a saved repair resumes the original selection");
        require(!recovery.open() && !recovery.finished(8, false, all) && !recovery.resume(all),
            "a launch resumes once");
        recovery.stop({});
        const auto normal = recovery.resume(all);
        require(normal && normal->ids.empty(), "Continue resumes the normal launch");
        recovery.stop({{"a", "removed"}});
        require(!recovery.resume([](const std::string &id) { return id != "removed"; }) && !recovery.open(),
            "a removed account launches nothing");
        std::cout << "Launch recovery tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
