/// @file pipeline.cpp
/// @brief A C++23 telemetry pipeline with a leak on its error path.
///
/// The counterpart to poc4/, which is the same program written in C++98. Both
/// leak the same 50 blocks of 256 bytes, and LeakHunter should say so
/// identically -- interception happens below the language, so the dialect is
/// not something it can see.
///
/// The point of the modern dress is that it does not help. `std::expected`
/// makes the error path *explicit* and still lets you leave an owned raw
/// pointer behind on it, because nothing about `std::unexpected` releases what
/// you allocated before you got there.
///
/// Expected: 50 leaks, 12800 bytes, blamed on `parseSample`.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace poc3 {
namespace {

constexpr std::size_t kSampleBytes = 256;

enum class ParseError : std::uint8_t {
    MissingSeparator = 1,
    EmptyChannel = 2,
};

/// C++23: std::to_underlying, instead of a static_cast nobody reads.
[[nodiscard]] constexpr std::uint8_t codeOf(ParseError error) noexcept {
    return std::to_underlying(error);
}

/// One decoded sample. Owns `payload` -- a raw pointer on purpose, because that
/// is where the bug lives and modern syntax does not remove the possibility.
struct Sample {
    long channel = 0;
    char* payload = nullptr;
};

/// C++23: a static operator(), so the functor needs no object to be called
/// through and the compiler need not pass one.
struct ChannelIsInteresting {
    static constexpr bool operator()(const Sample& sample) noexcept {
        return sample.channel % 3 != 0;
    }
};

/// C++23: `if consteval`, to keep one function usable in both worlds.
[[nodiscard]] constexpr std::size_t payloadBudget(std::size_t samples) {
    if consteval {
        return samples * kSampleBytes;  // no overflow check needed at compile time
    }
    return samples > (std::size_t{1} << 40) ? 0 : samples * kSampleBytes;
}

/// Parses `channel:reading`.
///
/// The buffer is allocated *before* the last validation, which is the whole
/// trap: `std::unexpected` returns as cleanly as anything, and takes nothing
/// with it.
///
/// `noinline` so the two demos name the same function. Without it the compiler
/// folds this into main() and the report blames `main` at the right line --
/// correct, and useless for a side-by-side comparison.
[[nodiscard]] __attribute__((noinline)) std::expected<Sample, ParseError> parseSample(
    std::string_view text) {
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        return std::unexpected(ParseError::MissingSeparator);
    }

    Sample sample;
    sample.channel = std::strtol(std::string(text.substr(0, colon)).c_str(), nullptr, 10);
    sample.payload = static_cast<char*>(std::malloc(kSampleBytes));
    if (sample.payload == nullptr) {
        return std::unexpected(ParseError::EmptyChannel);
    }
    std::snprintf(sample.payload, kSampleBytes, "%.*s",
                  static_cast<int>(text.size() - colon - 1), text.data() + colon + 1);

    if (text.substr(colon + 1).empty()) {
        // ---------------------------------------------------------------
        // THE BUG. `sample.payload` is live and owned, and this returns
        // without it. std::expected made the failure explicit and did
        // nothing at all about the ownership -- there was never a claim
        // that it would.
        // ---------------------------------------------------------------
        return std::unexpected(ParseError::EmptyChannel);
    }

    return sample;
}

void releaseSample(Sample& sample) noexcept {
    std::free(sample.payload);
    sample.payload = nullptr;
}

/// C++23: multidimensional operator[], so `histogram[row, column]` reads the way
/// it is spelled on a whiteboard.
class Histogram {
public:
    explicit Histogram(std::size_t rows, std::size_t columns)
        : rows_(rows), columns_(columns), cells_(rows * columns, 0) {}

    [[nodiscard]] std::size_t& operator[](std::size_t row, std::size_t column) {
        return cells_[row * columns_ + column];
    }

    [[nodiscard]] std::size_t total() const {
        // C++23 ranges::fold_left would be nicer still, but this is what both
        // libstdc++ 13 and libc++ 18 agree on today.
        std::size_t sum = 0;
        for (const std::size_t cell : cells_) {
            sum += cell;
        }
        return sum;
    }

private:
    std::size_t rows_;
    std::size_t columns_;
    std::vector<std::size_t> cells_;
};

[[nodiscard]] std::string makeLine(int index) {
    // Every sixth record has an empty reading, which is the error path.
    if (index % 6 == 0) {
        return std::to_string(index) + ":";
    }
    return std::to_string(index) + ":" + std::to_string(index * 7);
}

}  // namespace
}  // namespace poc3

int main() {
    using namespace poc3;

    std::printf("pipeline starting (__cplusplus = %ldL)\n", static_cast<long>(__cplusplus));

    constexpr int kRecords = 300;
    static_assert(payloadBudget(4uz) == 4 * kSampleBytes, "consteval branch");

    Histogram histogram(4, 8);
    std::size_t accepted = 0;
    std::size_t rejected = 0;

    for (int index = 1; index <= kRecords; ++index) {
        const std::string line = makeLine(index);

        auto parsed = parseSample(line);
        if (!parsed) {
            ++rejected;
            // The caller sees an error code and throws the result away. It has
            // no way to know a buffer was allocated, and no handle to free.
            (void)codeOf(parsed.error());
            continue;
        }

        // C++23: auto(x) decay-copy, so the predicate gets a value and the
        // original stays ours to release.
        if (ChannelIsInteresting{}(auto(parsed.value()))) {
            histogram[static_cast<std::size_t>(index) % 4uz,
                      static_cast<std::size_t>(index) % 8uz] += 1;
        }

        ++accepted;
        releaseSample(parsed.value());  // the happy path is correct
    }

    // C++23: std::string::contains, and ranges over a filtered view.
    const std::vector<std::string> tags{"telemetry", "pipeline", "poc3"};
    auto interesting = tags | std::views::filter([](const std::string& tag) {
                           return tag.contains("po");
                       });
    const auto tagCount = static_cast<std::size_t>(std::ranges::distance(interesting));

    std::printf("  %zu accepted, %zu rejected, histogram total %zu, %zu tag(s)\n", accepted,
                rejected, histogram.total(), tagCount);
    std::printf("pipeline finished cleanly\n");
    return 0;
}
