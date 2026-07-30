/* pipeline.cpp -- the same telemetry pipeline as poc3/, written in C++98.
 *
 * Same input, same bug, same 50 leaks of 256 bytes. LeakHunter should report
 * both identically, because interception happens below the language: it sees
 * malloc and operator new, and has no opinion about which standard produced
 * the call.
 *
 * The 1998 dress is not a costume. There is no std::expected, so failure comes
 * back as a bool and an out-parameter; there is no RAII in this style, so the
 * error path is where ownership goes to die. Twenty-five years later poc3/
 * makes the failure explicit and leaks in exactly the same place.
 *
 * Expected: 50 leaks, 12800 bytes, blamed on parseSample.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace poc4 {

const std::size_t kSampleBytes = 256;

/* No scoped enums in C++98. */
enum ParseError {
    ParseOk = 0,
    ParseMissingSeparator = 1,
    ParseEmptyChannel = 2
};

/* Owns `payload`. In 1998 this is simply how it was done. */
struct Sample {
    long channel;
    char* payload;

    Sample() : channel(0), payload(0) {}
};

/* No std::expected: the result is a bool, the value an out-parameter, and the
 * error a second out-parameter. Every caller has to remember three things.
 */
__attribute__((noinline))
bool parseSample(const std::string& text, Sample& out, ParseError& error) {
    const std::string::size_type colon = text.find(':');
    if (colon == std::string::npos) {
        error = ParseMissingSeparator;
        return false;
    }

    out.channel = std::strtol(text.substr(0, colon).c_str(), 0, 10);
    out.payload = static_cast<char*>(std::malloc(kSampleBytes));
    if (out.payload == 0) {
        error = ParseEmptyChannel;
        return false;
    }

    const std::string reading = text.substr(colon + 1);
    std::strncpy(out.payload, reading.c_str(), kSampleBytes - 1);
    out.payload[kSampleBytes - 1] = '\0';

    if (reading.empty()) {
        /* -------------------------------------------------------------
         * THE BUG, and it is the same one poc3/ has. `out.payload` is
         * allocated and owned, and this returns false without it. The
         * caller gets `false` and an error code, and has no idea that a
         * buffer exists, let alone a handle to free it.
         * ------------------------------------------------------------- */
        error = ParseEmptyChannel;
        return false;
    }

    error = ParseOk;
    return true;
}

void releaseSample(Sample& sample) {
    std::free(sample.payload);
    sample.payload = 0;
}

/* No multidimensional operator[] in C++98, so the index arithmetic is on show.
 * That is the honest difference: the same idea, spelled out.
 */
class Histogram {
public:
    Histogram(std::size_t rows, std::size_t columns)
        : rows_(rows), columns_(columns), cells_(rows * columns, 0) {}

    std::size_t& at(std::size_t row, std::size_t column) {
        return cells_[row * columns_ + column];
    }

    std::size_t total() const {
        std::size_t sum = 0;
        for (std::size_t i = 0; i < cells_.size(); ++i) {
            sum += cells_[i];
        }
        return sum;
    }

private:
    std::size_t rows_;
    std::size_t columns_;
    std::vector<std::size_t> cells_;
};

bool channelIsInteresting(const Sample& sample) {
    return sample.channel % 3 != 0;
}

/* No std::to_string in C++98. */
std::string toString(long value) {
    char buffer[32];
    std::sprintf(buffer, "%ld", value);
    return std::string(buffer);
}

std::string makeLine(int index) {
    if (index % 6 == 0) {
        return toString(index) + ":";
    }
    return toString(index) + ":" + toString(static_cast<long>(index) * 7);
}

}  /* namespace poc4 */

int main() {
    using namespace poc4;

    std::printf("pipeline starting (__cplusplus = %ldL)\n", static_cast<long>(__cplusplus));

    const int kRecords = 300;

    Histogram histogram(4, 8);
    std::size_t accepted = 0;
    std::size_t rejected = 0;

    for (int index = 1; index <= kRecords; ++index) {
        const std::string line = makeLine(index);

        Sample sample;
        ParseError error = ParseOk;
        if (!parseSample(line, sample, error)) {
            ++rejected;
            /* The caller sees false and an error code, and throws the result
             * away. Exactly as blind as poc3/'s caller, for exactly the same
             * reason.
             */
            continue;
        }

        if (channelIsInteresting(sample)) {
            histogram.at(static_cast<std::size_t>(index) % 4,
                         static_cast<std::size_t>(index) % 8) += 1;
        }

        ++accepted;
        releaseSample(sample); /* the happy path is correct */
    }

    /* No ranges, no lambdas: a loop and a substring search. */
    std::vector<std::string> tags;
    tags.push_back("telemetry");
    tags.push_back("pipeline");
    tags.push_back("poc4");

    std::size_t tagCount = 0;
    for (std::size_t i = 0; i < tags.size(); ++i) {
        if (tags[i].find("po") != std::string::npos) {
            ++tagCount;
        }
    }

    std::printf("  %lu accepted, %lu rejected, histogram total %lu, %lu tag(s)\n",
                static_cast<unsigned long>(accepted), static_cast<unsigned long>(rejected),
                static_cast<unsigned long>(histogram.total()),
                static_cast<unsigned long>(tagCount));
    std::printf("pipeline finished cleanly\n");
    return 0;
}
