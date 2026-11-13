#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gw2::recovery {
// A launch stopped by a game installation problem. Accounts are kept by ID; no IDs is
// the normal launch through GW2's own sign-in.
struct Request {
    std::vector<std::string> ids;
};
// Holds one stopped launch while its installation is repaired. It resumes at most once:
// after its installation save succeeds, or directly when the saved installation works.
// Cancel and a failed save never launch; a failed save keeps the request.
class Recovery {
    std::optional<Request> request_;
    std::uint64_t save_{};

  public:
    bool open() const { return request_.has_value(); }
    bool saving() const { return save_ != 0; }
    void stop(Request request) {
        request_ = std::move(request);
        save_ = 0;
    }
    void cancel() {
        request_.reset();
        save_ = 0;
    }
    void save(std::uint64_t serial) {
        if (request_) save_ = serial;
    }
    // Command `serial` finished. Returns the request when that was its successful save.
    template <class Exists> std::optional<Request> finished(std::uint64_t serial, bool failed, Exists exists) {
        if (!save_ || serial != save_) return std::nullopt;
        save_ = 0;
        return failed ? std::nullopt : resume(exists);
    }
    // A removed account ends the launch instead of narrowing or widening it.
    template <class Exists> std::optional<Request> resume(Exists exists) {
        auto request = std::exchange(request_, std::nullopt);
        save_ = 0;
        if (request && !std::ranges::all_of(request->ids, exists)) return std::nullopt;
        return request;
    }
};
} // namespace gw2::recovery
