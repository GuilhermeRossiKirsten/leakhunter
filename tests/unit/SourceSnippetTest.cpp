/// Reading source lines around a blamed location.
///
/// The paths handled here come from the *traced binary's* debug info, which is
/// to say from outside. Most of these tests are about the ways that input can be
/// hostile or merely wrong.

#include <filesystem>
#include <fstream>
#include <string>

#if defined(__unix__)
#include <sys/stat.h>
#endif

#include "TestFramework.hpp"
#include "leakhunter/source/SourceSnippetReader.hpp"

namespace fs = std::filesystem;
using leakhunter::SourceSnippet;
using leakhunter::source::SnippetConfig;
using leakhunter::source::SourceSnippetReader;

namespace {

struct TempTree {
    fs::path root;

    TempTree() : root(fs::temp_directory_path() / "leakhunter-snippet-test") {
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);
    }
    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    [[nodiscard]] fs::path write(const std::string& name, const std::string& content) const {
        const fs::path path = root / name;
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << content;
        return path;
    }
};

/// Ten numbered lines, so an off-by-one is visible rather than plausible.
std::string tenLines() {
    std::string text;
    for (int i = 1; i <= 10; ++i) {
        text += "line" + std::to_string(i) + "\n";
    }
    return text;
}

}  // namespace

LH_TEST(Snippet, reads_a_window_around_the_blamed_line) {
    const TempTree tree;
    const fs::path file = tree.write("a.cpp", tenLines());

    SnippetConfig config;
    config.contextLines = 2;
    SourceSnippetReader reader(config);

    const SourceSnippet snippet = reader.read(file.string(), 5, 0);
    LH_CHECK(!snippet.empty());
    LH_CHECK_EQ(snippet.firstLine, std::uint32_t{3});
    LH_CHECK_EQ(snippet.blamedLine, std::uint32_t{5});
    LH_CHECK_EQ(snippet.lines.size(), std::size_t{5});
    LH_CHECK_EQ(snippet.lines.front(), std::string{"line3"});
    LH_CHECK_EQ(snippet.lines.back(), std::string{"line7"});

    // The index is what every renderer uses to find the line to highlight.
    LH_CHECK_EQ(snippet.blamedIndex(), std::size_t{2});
    LH_CHECK_EQ(snippet.lines[snippet.blamedIndex()], std::string{"line5"});
}

LH_TEST(Snippet, clamps_at_the_start_of_the_file) {
    const TempTree tree;
    const fs::path file = tree.write("a.cpp", tenLines());

    SnippetConfig config;
    config.contextLines = 4;
    SourceSnippetReader reader(config);

    const SourceSnippet snippet = reader.read(file.string(), 1, 0);
    LH_CHECK_EQ(snippet.firstLine, std::uint32_t{1});
    LH_CHECK_EQ(snippet.lines.front(), std::string{"line1"});
    LH_CHECK_EQ(snippet.blamedIndex(), std::size_t{0});
}

LH_TEST(Snippet, clamps_at_the_end_of_the_file) {
    const TempTree tree;
    const fs::path file = tree.write("a.cpp", tenLines());

    SnippetConfig config;
    config.contextLines = 4;
    SourceSnippetReader reader(config);

    const SourceSnippet snippet = reader.read(file.string(), 10, 0);
    LH_CHECK_EQ(snippet.lines.back(), std::string{"line10"});
    LH_CHECK_EQ(snippet.blamedLine, std::uint32_t{10});
    LH_CHECK(snippet.blamedIndex() != static_cast<std::size_t>(-1));
}

LH_TEST(Snippet, a_line_past_the_end_yields_nothing) {
    // Stale debug info against an edited file. Highlighting an unrelated line
    // with confidence would be worse than showing no snippet at all.
    const TempTree tree;
    const fs::path file = tree.write("a.cpp", tenLines());

    SourceSnippetReader reader;
    LH_CHECK(reader.read(file.string(), 11, 0).empty());
    LH_CHECK(reader.read(file.string(), 100000, 0).empty());
}

LH_TEST(Snippet, line_zero_and_an_empty_path_yield_nothing) {
    const TempTree tree;
    const fs::path file = tree.write("a.cpp", tenLines());

    SourceSnippetReader reader;
    LH_CHECK(reader.read(file.string(), 0, 0).empty());
    LH_CHECK(reader.read("", 5, 0).empty());
}

LH_TEST(Snippet, a_missing_file_is_recorded_not_fatal) {
    SourceSnippetReader reader;
    LH_CHECK(reader.read("/nonexistent/nowhere/a.cpp", 5, 0).empty());
    LH_CHECK_EQ(reader.missingFiles().size(), std::size_t{1});
}

LH_TEST(Snippet, a_directory_is_not_read_as_a_file) {
    const TempTree tree;
    SourceSnippetReader reader;
    LH_CHECK(reader.read(tree.root.string(), 1, 0).empty());
}

#if defined(__unix__)
LH_TEST(Snippet, a_fifo_is_refused_rather_than_blocking_forever) {
    // The single most important case here. Source paths come from the traced
    // binary's DWARF; opening a FIFO with no writer blocks for ever, which would
    // hang the tool after the target had already exited successfully.
    const TempTree tree;
    const fs::path fifo = tree.root / "pipe.cpp";
    if (::mkfifo(fifo.c_str(), 0644) != 0) {
        return;  // no permission to create one here; nothing to assert
    }

    SourceSnippetReader reader;
    LH_CHECK(reader.read(fifo.string(), 1, 0).empty());
}
#endif

LH_TEST(Snippet, an_empty_file_yields_nothing) {
    const TempTree tree;
    const fs::path file = tree.write("empty.cpp", "");

    SourceSnippetReader reader;
    LH_CHECK(reader.read(file.string(), 1, 0).empty());
}

LH_TEST(Snippet, a_file_without_a_trailing_newline_still_reads) {
    const TempTree tree;
    const fs::path file = tree.write("a.cpp", "one\ntwo\nthree");

    SourceSnippetReader reader;
    const SourceSnippet snippet = reader.read(file.string(), 3, 0);
    LH_CHECK_EQ(snippet.lines.back(), std::string{"three"});
}

LH_TEST(Snippet, crlf_line_endings_lose_the_carriage_return) {
    const TempTree tree;
    const fs::path file = tree.write("a.cpp", "one\r\ntwo\r\nthree\r\n");

    SourceSnippetReader reader;
    const SourceSnippet snippet = reader.read(file.string(), 2, 0);
    // A stray \r would render as a control character in the HTML and break the
    // caret alignment in the terminal.
    LH_CHECK_EQ(snippet.lines[snippet.blamedIndex()], std::string{"two"});
}

LH_TEST(Snippet, tabs_are_expanded_so_the_column_means_something) {
    const TempTree tree;
    const fs::path file = tree.write("a.cpp", "\tif (x) {\n\t\tleak();\n\t}\n");

    SnippetConfig config;
    config.tabWidth = 4;
    SourceSnippetReader reader(config);

    const SourceSnippet snippet = reader.read(file.string(), 2, 0);
    LH_CHECK_EQ(snippet.lines[snippet.blamedIndex()], std::string{"        leak();"});
}

LH_TEST(Snippet, an_oversized_file_is_skipped) {
    const TempTree tree;
    const fs::path file = tree.write("big.cpp", std::string(4096, 'x') + "\n");

    SnippetConfig config;
    config.maxFileBytes = 1024;
    SourceSnippetReader reader(config);
    LH_CHECK(reader.read(file.string(), 1, 0).empty());
}

LH_TEST(Snippet, the_total_budget_stops_later_sites) {
    const TempTree tree;
    const fs::path file = tree.write("a.cpp", tenLines());

    SnippetConfig config;
    config.contextLines = 1;
    config.maxTotalBytes = 40;  // room for one small window, not two
    SourceSnippetReader reader(config);

    LH_CHECK(!reader.read(file.string(), 5, 0).empty());
    LH_CHECK(!reader.budgetExhausted());

    // Keep asking until the budget gives out, then confirm it stays given out.
    for (int i = 0; i < 10 && !reader.budgetExhausted(); ++i) {
        (void)reader.read(file.string(), 5, 0);
    }
    LH_CHECK(reader.budgetExhausted());
    LH_CHECK(reader.read(file.string(), 5, 0).empty());
}

LH_TEST(Snippet, the_column_is_carried_through_untouched) {
    const TempTree tree;
    const fs::path file = tree.write("a.cpp", tenLines());

    SourceSnippetReader reader;
    LH_CHECK_EQ(reader.read(file.string(), 5, 17).column, std::uint32_t{17});
    // addr2line gives no column; 0 has to survive as "unknown".
    LH_CHECK_EQ(reader.read(file.string(), 5, 0).column, std::uint32_t{0});
}

// --- path remapping --------------------------------------------------------

LH_TEST(Snippet, a_source_root_remaps_a_build_machine_path) {
    // The path recorded by a CI build does not exist on the machine reading the
    // report. This is the case --source-root is for.
    const TempTree tree;
    (void)tree.write("src/app/foo.cpp", tenLines());

    SnippetConfig config;
    config.roots = {tree.root};
    SourceSnippetReader reader(config);

    const SourceSnippet snippet =
        reader.read("/build/agent/work/1/s/src/app/foo.cpp", 5, 0);
    LH_CHECK(!snippet.empty());
    LH_CHECK_EQ(snippet.lines[snippet.blamedIndex()], std::string{"line5"});
}

LH_TEST(Snippet, a_source_root_that_does_not_help_is_harmless) {
    const TempTree tree;
    SnippetConfig config;
    config.roots = {tree.root};
    SourceSnippetReader reader(config);

    LH_CHECK(reader.read("/elsewhere/nope.cpp", 1, 0).empty());
    LH_CHECK_EQ(reader.missingFiles().size(), std::size_t{1});
}

LH_TEST(Snippet, the_recorded_path_wins_over_a_root) {
    const TempTree tree;
    const fs::path real = tree.write("direct.cpp", "the real one\n");
    (void)tree.write("decoy/direct.cpp", "the decoy\n");

    SnippetConfig config;
    config.roots = {tree.root / "decoy"};
    SourceSnippetReader reader(config);

    const SourceSnippet snippet = reader.read(real.string(), 1, 0);
    LH_CHECK_EQ(snippet.lines.front(), std::string{"the real one"});
}

LH_TEST(Snippet, a_file_is_read_once_however_many_sites_it_has) {
    // Several leak sites in one file is the common case. Re-reading per site
    // would turn a 3-site report into 3 file reads for no reason.
    const TempTree tree;
    const fs::path file = tree.write("a.cpp", tenLines());

    SourceSnippetReader reader;
    for (std::uint32_t line = 1; line <= 10; ++line) {
        LH_CHECK(!reader.read(file.string(), line, 0).empty());
    }
    // A missing file is likewise looked up once, not once per site.
    for (int i = 0; i < 5; ++i) {
        (void)reader.read("/nope/gone.cpp", 1, 0);
    }
    LH_CHECK_EQ(reader.missingFiles().size(), std::size_t{1});
}
