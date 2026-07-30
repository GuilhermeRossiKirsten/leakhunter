#include "leakhunter/report/HtmlReportGenerator.hpp"

#include <fstream>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "leakhunter/core/Logger.hpp"
#include "leakhunter/report/JsonReportGenerator.hpp"

namespace leakhunter::report {
namespace {

/// The report data is embedded verbatim inside a <script> element, so any
/// literal "</" would end the element early. Escaping the slash keeps the JSON
/// semantically identical while making it impossible to break out of the tag.
[[nodiscard]] std::string escapeForScriptTag(const std::string& json) {
    std::string escaped;
    escaped.reserve(json.size() + 32);

    for (std::size_t i = 0; i < json.size(); ++i) {
        if (json[i] == '<' && i + 1 < json.size() && json[i + 1] == '/') {
            escaped += "<\\/";
            ++i;
        } else {
            escaped.push_back(json[i]);
        }
    }
    return escaped;
}

/// The monitored command is attacker-influenced in exactly one sense: it is
/// arbitrary text from argv. Escape it before it reaches the markup.
[[nodiscard]] std::string escapeHtml(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());

    for (const char character : text) {
        switch (character) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            case '\'': escaped += "&#39;"; break;
            default: escaped.push_back(character); break;
        }
    }
    return escaped;
}

constexpr std::string_view kStyles = R"CSS(
:root {
  color-scheme: light dark;
  --bg: #f6f7f9;
  --surface: #ffffff;
  --surface-alt: #f0f2f5;
  --border: #d9dde3;
  --text: #1b1f24;
  --muted: #5a6270;
  --accent: #2f6feb;
  --danger: #c8372d;
  --warn: #b7791f;
  --ok: #1a7f45;
  --mono: ui-monospace, SFMono-Regular, "SF Mono", Menlo, Consolas, monospace;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #0f1216;
    --surface: #171b21;
    --surface-alt: #1e242c;
    --border: #2b333d;
    --text: #e6e9ee;
    --muted: #9aa4b2;
    --accent: #5b8cff;
    --danger: #f0776a;
    --warn: #e0b23c;
    --ok: #4bbd7c;
  }
}
* { box-sizing: border-box; }
body {
  margin: 0;
  padding: 2rem 1.25rem 4rem;
  background: var(--bg);
  color: var(--text);
  font: 15px/1.55 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
}
.wrap { max-width: 1180px; margin: 0 auto; }
header { margin-bottom: 1.75rem; }
h1 { margin: 0 0 .35rem; font-size: 1.6rem; letter-spacing: -.02em; }
h2 { margin: 2.25rem 0 .85rem; font-size: 1.15rem; }
.sub { color: var(--muted); font-size: .9rem; }
.sub code { font-family: var(--mono); background: var(--surface-alt); padding: .12rem .35rem; border-radius: 4px; }
.verdict { display: inline-block; margin-top: .9rem; padding: .35rem .8rem; border-radius: 999px; font-weight: 600; font-size: .85rem; }
.verdict.bad { background: color-mix(in srgb, var(--danger) 15%, transparent); color: var(--danger); }
.verdict.good { background: color-mix(in srgb, var(--ok) 15%, transparent); color: var(--ok); }
.cards { display: grid; grid-template-columns: repeat(auto-fit, minmax(170px, 1fr)); gap: .85rem; }
.card { background: var(--surface); border: 1px solid var(--border); border-radius: 10px; padding: .9rem 1rem; }
.card .label { color: var(--muted); font-size: .78rem; text-transform: uppercase; letter-spacing: .05em; }
.card .value { font-size: 1.4rem; font-weight: 650; margin-top: .3rem; font-variant-numeric: tabular-nums; }
.card .value.danger { color: var(--danger); }
.card .hint { color: var(--muted); font-size: .78rem; margin-top: .15rem; }
.notice { margin-top: 1rem; padding: .7rem .9rem; border-radius: 8px; border: 1px solid color-mix(in srgb, var(--warn) 40%, var(--border)); background: color-mix(in srgb, var(--warn) 10%, transparent); font-size: .87rem; }
.tablewrap { overflow-x: auto; border: 1px solid var(--border); border-radius: 10px; background: var(--surface); }
table { border-collapse: collapse; width: 100%; min-width: 720px; }
th, td { text-align: left; padding: .6rem .85rem; border-bottom: 1px solid var(--border); vertical-align: top; }
th { background: var(--surface-alt); font-size: .78rem; text-transform: uppercase; letter-spacing: .04em; color: var(--muted); cursor: pointer; user-select: none; white-space: nowrap; }
th[aria-sort="descending"]::after { content: " \25BC"; font-size: .7em; }
th[aria-sort="ascending"]::after { content: " \25B2"; font-size: .7em; }
tbody tr:last-child td { border-bottom: none; }
tbody tr.group-row { cursor: pointer; }
tbody tr.group-row:hover { background: var(--surface-alt); }
td.num { text-align: right; font-variant-numeric: tabular-nums; white-space: nowrap; }
.fn { font-family: var(--mono); font-size: .87rem; word-break: break-all; }
.loc { color: var(--muted); font-size: .8rem; font-family: var(--mono); word-break: break-all; }
.badge { display: inline-block; padding: .08rem .4rem; border-radius: 4px; background: var(--surface-alt); border: 1px solid var(--border); font-size: .72rem; color: var(--muted); font-family: var(--mono); }
tr.detail > td { padding: 0; background: var(--surface-alt); }
tr.detail.hidden { display: none; }
section.hidden { display: none; }
.snippet { margin: 0 0 .85rem; border: 1px solid var(--border); border-radius: 8px; overflow-x: auto; background: var(--surface-alt); }
.snippet .path { padding: .3rem .7rem; border-bottom: 1px solid var(--border); color: var(--muted); font-family: var(--mono); font-size: .74rem; }
.snippet table { border: 0; margin: 0; width: auto; min-width: 100%; background: transparent; }
.snippet td { border: 0; padding: .04rem .7rem .04rem 0; font-family: var(--mono); font-size: .78rem; white-space: pre; vertical-align: baseline; }
.snippet td.ln { padding: .04rem .7rem; text-align: right; color: var(--muted); user-select: none; border-right: 1px solid var(--border); width: 1%; }
.snippet tr.blamed { background: color-mix(in srgb, var(--danger) 14%, transparent); }
.snippet tr.blamed td.ln { color: var(--danger); font-weight: 600; }
.snippet tr.caret td { color: var(--danger); font-weight: 600; }
.snippet .note { color: var(--danger); font-weight: 600; }
.ub { border: 1px solid color-mix(in srgb, var(--danger) 40%, var(--border)); background: color-mix(in srgb, var(--danger) 8%, transparent); border-radius: 10px; padding: .8rem 1rem; margin-bottom: .7rem; }
.ub .what { font-weight: 600; color: var(--danger); font-size: .9rem; }
.ub .meta { color: var(--muted); font-size: .8rem; margin: .15rem 0 .4rem; font-family: var(--mono); }
ol.stack { margin: 0; padding: .75rem 1rem .9rem 2.6rem; font-family: var(--mono); font-size: .82rem; }
ol.stack li { padding: .12rem 0; }
ol.stack li.blamed { color: var(--danger); font-weight: 600; }
ol.stack .where { color: var(--muted); }
.toolbar { display: flex; gap: .6rem; align-items: center; margin-bottom: .7rem; flex-wrap: wrap; }
input[type="search"] { flex: 1 1 260px; padding: .45rem .7rem; border-radius: 8px; border: 1px solid var(--border); background: var(--surface); color: var(--text); font: inherit; font-size: .88rem; }
.empty { padding: 2.5rem 1rem; text-align: center; color: var(--muted); }
footer { margin-top: 3rem; color: var(--muted); font-size: .8rem; }
)CSS";

constexpr std::string_view kScript = R"JS(
(function () {
  const data = JSON.parse(document.getElementById('leakhunter-data').textContent);
  const groups = data.groups || [];
  const leaks = data.leaks || [];

  const bytes = (n) => {
    const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB'];
    let value = Number(n), unit = 0;
    while (value >= 1024 && unit < units.length - 1) { value /= 1024; unit++; }
    return unit === 0 ? value + ' B' : value.toFixed(2) + ' ' + units[unit];
  };
  const escapeHtml = (s) => String(s == null ? '' : s).replace(/[&<>"']/g,
    (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

  const frameHtml = (frame, blamed) => {
    const name = escapeHtml(frame.displayName || frame.function || '??');
    const where = frame.file
      ? escapeHtml(frame.file) + ':' + frame.line
      : escapeHtml(frame.module || '') + '+' + escapeHtml(frame.moduleOffset || '');
    return '<li class="' + (blamed ? 'blamed' : '') + '">' + name +
           ' <span class="where">' + where + '</span></li>';
  };

  const stackHtml = (trace, blamedIndex) =>
    '<ol class="stack">' +
    trace.map((f, i) => frameHtml(f, i === blamedIndex)).join('') +
    '</ol>';

  // The blamed line in its source context, with that line highlighted.
  //
  // Every line of code goes through escapeHtml. Source is full of <, > and &,
  // and this is user code being placed into markup -- getting it wrong here
  // would be HTML injection from the file being analysed.
  const snippetHtml = (snippet, note) => {
    if (!snippet || !snippet.lines || snippet.lines.length === 0) return '';

    const first = snippet.firstLine;
    const blamed = snippet.blamedLine;
    const rows = snippet.lines.map((line, i) => {
      const number = first + i;
      const isBlamed = number === blamed;
      let html = '<tr' + (isBlamed ? ' class="blamed"' : '') + '>' +
                 '<td class="ln">' + number + '</td>' +
                 '<td>' + escapeHtml(line) + '</td></tr>';
      if (isBlamed) {
        // A caret row under the blamed line, at the column when we have one.
        const column = snippet.column || 0;
        const pad = column > 0 ? column - 1 : (line.length - line.replace(/^\s+/, '').length);
        const mark = column > 0 ? '^' : '~'.repeat(Math.max(1, line.trim().length));
        html += '<tr class="caret"><td class="ln"></td><td>' +
                ' '.repeat(Math.max(0, pad)) + mark +
                (note ? '  ' + escapeHtml(note) : '') + '</td></tr>';
      }
      return html;
    }).join('');

    return '<div class="snippet"><div class="path">' + escapeHtml(snippet.file) +
           '</div><table>' + rows + '</table></div>';
  };

  const tbody = document.getElementById('groups-body');
  let sortKey = 'totalBytes';
  let sortDir = -1;
  let filter = '';

  // The "Stack" column sorts by depth, which is not a field of the report.
  // Deriving it once keeps the sort comparator uniform.
  groups.forEach((g) => { g.frameCount = (g.stackTrace || []).length; });

  function render() {
    const needle = filter.trim().toLowerCase();

    if (groups.length === 0) {
      tbody.innerHTML =
        '<tr><td colspan="5" class="empty">No leaks detected.</td></tr>';
      return;
    }

    const rows = groups
      .map((group, index) => ({ group, index }))
      .filter(({ group }) => !needle ||
        (group.function + ' ' + group.module + ' ' + group.location).toLowerCase().includes(needle))
      .sort((a, b) => {
        const x = a.group[sortKey], y = b.group[sortKey];
        if (typeof x === 'string') return sortDir * x.localeCompare(y);
        return sortDir * (x - y);
      });

    if (rows.length === 0) {
      tbody.innerHTML =
        '<tr><td colspan="5" class="empty">No leak site matches that filter.</td></tr>';
      return;
    }

    tbody.innerHTML = rows.map(({ group, index }) => {
      const blamed = group.blamedFrame == null ? -1 : group.blamedFrame;
      const first = group.leakIndices && group.leakIndices.length
        ? leaks[group.leakIndices[0]] : null;
      const threads = first ? first.threadId : '-';
      return '' +
        '<tr class="group-row" data-target="detail-' + index + '">' +
          '<td class="num">' + bytes(group.totalBytes) + '</td>' +
          '<td class="num">' + group.count + '</td>' +
          '<td><div class="fn">' + escapeHtml(group.function) + '</div>' +
            (group.location ? '<div class="loc">' + escapeHtml(group.location) + '</div>' :
              '<div class="loc">' + escapeHtml(group.module) + '</div>') + '</td>' +
          '<td class="num"><span class="badge">' + threads +
            (group.threadCount > 1 ? ' +' + (group.threadCount - 1) : '') + '</span></td>' +
          '<td class="num">' + group.frameCount + ' frames</td>' +
        '</tr>' +
        '<tr class="detail hidden" id="detail-' + index + '"><td colspan="5">' +
          // Source first, stack second: the line is the answer, the stack is
          // the evidence for it.
          snippetHtml(group.snippet,
                      group.count + ' leak(s), ' + bytes(group.totalBytes) + ' here') +
          stackHtml(group.stackTrace || [], blamed) +
        '</td></tr>';
    }).join('');
  }

  tbody.addEventListener('click', (event) => {
    const row = event.target.closest('tr.group-row');
    if (!row) return;
    const detail = document.getElementById(row.dataset.target);
    if (detail) detail.classList.toggle('hidden');
  });

  document.querySelectorAll('th[data-key]').forEach((header) => {
    header.addEventListener('click', () => {
      const key = header.dataset.key;
      sortDir = key === sortKey ? -sortDir : -1;
      sortKey = key;
      document.querySelectorAll('th[data-key]').forEach((h) => h.removeAttribute('aria-sort'));
      header.setAttribute('aria-sort', sortDir === -1 ? 'descending' : 'ascending');
      render();
    });
  });

  const search = document.getElementById('filter');
  search.addEventListener('input', () => { filter = search.value; render(); });

  render();

  // Mismatched frees. Rendered from the same embedded data so the stack markup
  // stays identical to the leak table's; the whole section stays hidden when
  // the run is clean, which is the common case.
  const mismatches = data.mismatchedFrees || [];
  if (mismatches.length > 0) {
    const section = document.getElementById('mismatch-section');
    section.classList.remove('hidden');
    document.getElementById('mismatch-body').innerHTML = mismatches.map((m) => {
      const blamed = m.responsibleFrame == null ? -1 : m.responsibleFrame;
      const threads = m.allocatedOnThread === m.releasedOnThread
        ? 'thread ' + m.allocatedOnThread
        : 'allocated on thread ' + m.allocatedOnThread + ', released on ' + m.releasedOnThread;
      return '' +
        '<div class="ub">' +
          '<div class="what">' + escapeHtml(m.description) + '</div>' +
          '<div class="meta">' + bytes(m.size) + ' at ' + escapeHtml(m.address) +
            ' &middot; ' + escapeHtml(threads) + '</div>' +
          snippetHtml(m.snippet, 'allocated here') +
          stackHtml(m.stackTrace || [], blamed) +
        '</div>';
    }).join('');
  }
})();
)JS";

}  // namespace

std::string HtmlReportGenerator::render(const analysis::LeakReport& report) {
    const nlohmann::json document = JsonReportGenerator::toJson(report);
    const std::string payload =
        escapeForScriptTag(JsonReportGenerator::serialize(document, /*prettyPrint=*/false));

    const auto& stats = report.stats;

    // Three outcomes, not two: a run can leak nothing and still be wrong.
    std::string verdict;
    if (report.clean()) {
        verdict = R"(<span class="verdict good">PASSED &mdash; no leaks, no mismatched frees</span>)";
    } else if (report.leakCount > 0) {
        verdict = fmt::format(R"(<span class="verdict bad">{} leak{} &mdash; {} lost</span>)",
                              report.leakCount, report.leakCount == 1 ? "" : "s",
                              formatBytes(report.leakedBytes));
    } else {
        verdict = fmt::format(
            R"(<span class="verdict bad">{} mismatched free{} &mdash; undefined behaviour</span>)",
            stats.mismatchedFrees, stats.mismatchedFrees == 1 ? "" : "s");
    }
    if (report.leakCount > 0 && stats.mismatchedFrees > 0) {
        verdict += fmt::format(
            R"( <span class="verdict bad">{} mismatched free{}</span>)", stats.mismatchedFrees,
            stats.mismatchedFrees == 1 ? "" : "s");
    }

    std::string notices;
    if (report.process.stoppedByRequest) {
        // A service was stopped on purpose. It has an incomplete trace by
        // definition, and saying "the target may have exited abnormally" about
        // something the user deliberately ended reads as a malfunction.
        notices += fmt::format(
            R"(<div class="notice"><strong>You stopped this run.</strong> The target was still )"
            R"(running when it received signal {}, so this is everything it did up to that )"
            R"(moment &mdash; which is the point when the target is a service. Anything )"
            R"(allocated in the final instant, after the last flush, is not here.</div>)",
            report.process.terminatingSignal);
    } else if (stats.droppedRecords > 0) {
        notices += fmt::format(
            R"(<div class="notice"><strong>Partial data.</strong> The agent reported {} dropped record(s); )"
            R"(the target may have exited abnormally. Leak counts are a lower bound.</div>)",
            stats.droppedRecords);
    }
    if (report.suppressedLeaks > 0) {
        notices += fmt::format(
            R"(<div class="notice">{} leak(s) totalling {} were omitted from the listing by the )"
            R"(size/detail limits, but are included in the totals above.</div>)",
            report.suppressedLeaks, formatBytes(report.suppressedBytes));
    }
    if (report.runtimeLeakCount > 0) {
        notices += fmt::format(
            R"(<div class="notice"><strong>{} runtime block(s)</strong> totalling {} were still )"
            R"(live at exit but were requested from inside libc or the dynamic loader &mdash; )"
            R"(stdio buffers, locale tables and similar. These are released by the OS and are )"
            R"(normally not defects; rerun with <code>--include-runtime</code> to list them.</div>)",
            report.runtimeLeakCount, formatBytes(report.runtimeLeakedBytes));
    }
    if (report.process.terminatingSignal != 0 && !report.process.stoppedByRequest) {
        notices += fmt::format(
            R"(<div class="notice">The target was terminated by signal {}. Allocations made after )"
            R"(the last flush are missing.</div>)",
            report.process.terminatingSignal);
    }
    if (report.suppressedByRules > 0 || report.suppressedMismatchesByRules > 0) {
        std::string rows;
        for (const analysis::LeakReport::RuleHit& hit : report.ruleHits) {
            rows += fmt::format("<li><code>{}</code> &mdash; {} finding(s), {}</li>",
                                escapeHtml(hit.rule), hit.count, formatBytes(hit.bytes));
        }
        const std::string mismatchNote =
            report.suppressedMismatchesByRules > 0
                ? fmt::format(" and {} mismatched free(s)", report.suppressedMismatchesByRules)
                : std::string{};
        notices += fmt::format(
            R"(<div class="notice"><strong>{} leak(s) totalling {}{} were suppressed</strong> by )"
            R"(rules you supplied, and are excluded from the counts above:<ul>{}</ul></div>)",
            report.suppressedByRules, formatBytes(report.suppressedByRulesBytes), mismatchNote,
            rows);
    }
    if (!report.unusedRules.empty()) {
        std::string rows;
        for (const std::string& unused : report.unusedRules) {
            rows += fmt::format("<li><code>{}</code></li>", escapeHtml(unused));
        }
        notices += fmt::format(
            R"(<div class="notice"><strong>{} suppression rule(s) matched nothing.</strong> A rule )"
            R"(that has rotted &mdash; the function was renamed, the library was dropped &mdash; )"
            R"(looks like coverage that is not there:<ul>{}</ul></div>)",
            report.unusedRules.size(), rows);
    }
    if (report.mismatchCheck == analysis::MismatchCheck::Suppressed) {
        notices += fmt::format(
            R"(<div class="notice"><strong>Mismatched frees were not checked.</strong> The target )"
            R"(allocates with <code>new</code> but our <code>operator delete</code> never ran, so )"
            R"(it must define its own &mdash; a static libstdc++, or a custom global operator. )"
            R"(Every new/free pairing we could derive would be an artefact of that gap rather )"
            R"(than a bug, so the whole check was suppressed. Leak detection is )"
            R"(unaffected.</div>)");
    }
    if (report.suppressedMismatches > 0) {
        notices += fmt::format(
            R"(<div class="notice">{} further mismatched free(s) were counted but not listed. )"
            R"(A wrong pairing is usually the same line of code executed repeatedly, so the )"
            R"(listed ones are very likely to cover every distinct cause.</div>)",
            report.suppressedMismatches);
    }

    const std::string body = report.groups.empty()
                                 ? R"(<tr><td colspan="5" class="empty">Nothing leaked. </td></tr>)"
                                 : std::string{};

    return fmt::format(
        R"(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>LeakHunter report &ndash; {command}</title>
<style>{styles}</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>LeakHunter report</h1>
    <div class="sub">
      <code>{command}</code> &middot; pid {pid} &middot; exit {exitCode} &middot; {generatedAt}
    </div>
    {verdict}
  </header>

  <section class="cards">
    <div class="card"><div class="label">Total allocations</div><div class="value">{allocations}</div>
      <div class="hint">{allocatedBytes} requested</div></div>
    <div class="card"><div class="label">Total freed</div><div class="value">{deallocations}</div>
      <div class="hint">{freedBytes} released</div></div>
    <div class="card"><div class="label">Memory leaked</div><div class="value danger">{leakedBytes}</div>
      <div class="hint">{leakPercent} of all bytes</div></div>
    <div class="card"><div class="label">Leaks</div><div class="value">{leakCount}</div>
      <div class="hint">{groupCount} distinct site(s)</div></div>
    <div class="card"><div class="label">Peak live</div><div class="value">{peak}</div>
      <div class="hint">high-water mark</div></div>
  </section>
  {notices}

  <h2>Leaks by function</h2>
  <div class="toolbar">
    <input id="filter" type="search" placeholder="Filter by function, module or file&hellip;">
  </div>
  <div class="tablewrap">
    <table>
      <thead>
        <tr>
          <th data-key="totalBytes" aria-sort="descending">Bytes leaked</th>
          <th data-key="count">Leaks</th>
          <th data-key="function">Function</th>
          <th data-key="threadCount">Threads</th>
          <th data-key="frameCount">Stack</th>
        </tr>
      </thead>
      <tbody id="groups-body">{emptyBody}</tbody>
    </table>
  </div>

  <section id="mismatch-section" class="hidden">
    <h2>Mismatched frees</h2>
    <p class="sub">Each block below was returned through the wrong entry point. The memory was
    released, so it is not a leak &mdash; it is undefined behaviour, and it tends to keep working
    until an allocator change or a new destructor turns it into a crash. The stack shown is where
    the block was <em>allocated</em>.</p>
    <div id="mismatch-body"></div>
  </section>

  <footer>
    Generated by leakhunter {version} &middot; click a row to expand its stack trace &middot;
    the highlighted frame is the one blamed for the allocation.
  </footer>
</div>
<script id="leakhunter-data" type="application/json">{payload}</script>
<script>{script}</script>
</body>
</html>
)",
        fmt::arg("command", escapeHtml(report.targetCommand)),
        fmt::arg("styles", kStyles),
        fmt::arg("script", kScript),
        fmt::arg("payload", payload),
        fmt::arg("pid", stats.pid),
        fmt::arg("exitCode", report.process.exitCode),
        fmt::arg("generatedAt", report.generatedAtIso8601),
        fmt::arg("verdict", verdict),
        fmt::arg("allocations", stats.totalAllocations),
        fmt::arg("allocatedBytes", formatBytes(stats.totalBytesAllocated)),
        fmt::arg("deallocations", stats.totalDeallocations),
        fmt::arg("freedBytes", formatBytes(stats.totalBytesFreed)),
        fmt::arg("leakedBytes", formatBytes(report.leakedBytes)),
        fmt::arg("leakPercent",
                 stats.totalBytesAllocated == 0
                     ? std::string{"0.0%"}
                     : fmt::format("{:.1f}%", 100.0 * static_cast<double>(report.leakedBytes) /
                                                  static_cast<double>(stats.totalBytesAllocated))),
        fmt::arg("leakCount", report.leakCount),
        fmt::arg("groupCount", report.groups.size()),
        fmt::arg("peak", formatBytes(stats.peakLiveBytes)),
        fmt::arg("notices", notices),
        fmt::arg("emptyBody", body),
        fmt::arg("version", report.toolVersion));
}

Status HtmlReportGenerator::generate(const analysis::LeakReport& report,
                                     const std::filesystem::path& outputPath) {
    std::error_code ec;
    if (outputPath.has_parent_path()) {
        std::filesystem::create_directories(outputPath.parent_path(), ec);
        if (ec) {
            return Error{fmt::format("cannot create '{}': {}", outputPath.parent_path().string(),
                                     ec.message())};
        }
    }

    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
        return Error{fmt::format("cannot write '{}'", outputPath.string())};
    }

    output << render(report);
    if (!output) {
        return Error{fmt::format("failed while writing '{}'", outputPath.string())};
    }

    log::debug("wrote {}", outputPath.string());
    return {};
}

}  // namespace leakhunter::report
