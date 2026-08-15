#include "config/config.hpp"

#include <sys/stat.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace gittop::config {
namespace {

std::string Env(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string{value};
}

std::string Trim(const std::string& in) {
  const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  std::size_t begin = 0;
  std::size_t end = in.size();
  while (begin < end && is_space(static_cast<unsigned char>(in[begin]))) {
    ++begin;
  }
  while (end > begin && is_space(static_cast<unsigned char>(in[end - 1]))) {
    --end;
  }
  return in.substr(begin, end - begin);
}

// Splits a table header on dots that are not inside quotes, so
// hosts."github.com" comes back as {"hosts", "github.com"} rather than three
// segments. Returns false on an unterminated quote.
bool SplitPath(const std::string& in, std::vector<std::string>* out) {
  std::string current;
  bool quoted = false;
  bool saw_quote = false;

  for (std::size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    if (c == '"') {
      quoted = !quoted;
      saw_quote = true;
      continue;
    }
    if (c == '.' && !quoted) {
      const std::string segment = saw_quote ? current : Trim(current);
      if (segment.empty()) {
        return false;
      }
      out->push_back(segment);
      current.clear();
      saw_quote = false;
      continue;
    }
    current.push_back(c);
  }
  if (quoted) {
    return false;
  }
  const std::string segment = saw_quote ? current : Trim(current);
  if (segment.empty()) {
    return false;
  }
  out->push_back(segment);
  return true;
}

// Values are quoted strings, bare integers, or bare bools. Escapes are limited
// to the ones a token or a URL can actually contain.
bool ParseValue(const std::string& raw, std::string* out, std::string* error) {
  const std::string in = Trim(raw);
  if (in.empty()) {
    *error = "empty value";
    return false;
  }

  if (in.front() == '"') {
    if (in.size() < 2 || in.back() != '"') {
      *error = "unterminated string";
      return false;
    }
    std::string decoded;
    for (std::size_t i = 1; i + 1 < in.size(); ++i) {
      if (in[i] == '\\' && i + 2 < in.size()) {
        switch (in[i + 1]) {
          case 'n': decoded.push_back('\n'); ++i; continue;
          case 't': decoded.push_back('\t'); ++i; continue;
          case '"': decoded.push_back('"'); ++i; continue;
          case '\\': decoded.push_back('\\'); ++i; continue;
          default: break;
        }
      }
      decoded.push_back(in[i]);
    }
    *out = decoded;
    return true;
  }

  if (in == "true" || in == "false") {
    *out = in;
    return true;
  }

  std::size_t index = 0;
  if (in[index] == '-' || in[index] == '+') {
    ++index;
  }
  if (index >= in.size()) {
    *error = "value must be a quoted string, an integer, or a bool";
    return false;
  }
  for (std::size_t i = index; i < in.size(); ++i) {
    if (std::isdigit(static_cast<unsigned char>(in[i])) == 0 && in[i] != '_') {
      *error = "value must be a quoted string, an integer, or a bool";
      return false;
    }
  }
  *out = in;
  return true;
}

// Comments run to end of line, except inside a quoted string where a '#' is
// just a character. Tokens contain '#' often enough for this to matter.
std::string StripComment(const std::string& line) {
  bool quoted = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '"' && (i == 0 || line[i - 1] != '\\')) {
      quoted = !quoted;
    } else if (line[i] == '#' && !quoted) {
      return line.substr(0, i);
    }
  }
  return line;
}

bool BareKeySafe(const std::string& segment) {
  if (segment.empty()) {
    return false;
  }
  for (const char c : segment) {
    const bool ok = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

std::string QuoteSegment(const std::string& segment) {
  if (BareKeySafe(segment)) {
    return segment;
  }
  std::string out = "\"";
  for (const char c : segment) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

bool LooksScalar(const std::string& value) {
  if (value == "true" || value == "false") {
    return true;
  }
  if (value.empty()) {
    return false;
  }
  std::size_t i = (value[0] == '-' || value[0] == '+') ? 1 : 0;
  if (i >= value.size()) {
    return false;
  }
  for (; i < value.size(); ++i) {
    if (std::isdigit(static_cast<unsigned char>(value[i])) == 0) {
      return false;
    }
  }
  return true;
}

std::string FormatValue(const std::string& value) {
  if (LooksScalar(value)) {
    return value;
  }
  std::string out = "\"";
  for (const char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      default: out.push_back(c); break;
    }
  }
  out.push_back('"');
  return out;
}

// Where a line's comment starts, or npos. Quote-aware for the same reason
// StripComment is: a '#' inside a token is a character, not a comment.
std::size_t CommentAt(const std::string& line) {
  bool quoted = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '"' && (i == 0 || line[i - 1] != '\\')) {
      quoted = !quoted;
    } else if (line[i] == '#' && !quoted) {
      return i;
    }
  }
  return std::string::npos;
}

// The flat dotted key a table header and a key path add up to — the same string
// Load stores, so the two cannot disagree about what a line is called.
std::string JoinKey(const std::vector<std::string>& table,
                    const std::vector<std::string>& key) {
  std::string full;
  for (const std::string& segment : table) {
    full += segment;
    full.push_back('.');
  }
  for (std::size_t i = 0; i < key.size(); ++i) {
    full += key[i];
    if (i + 1 < key.size()) {
      full.push_back('.');
    }
  }
  return full;
}

// The dotted form of a table header's segments, which is the prefix every key
// under it carries — `{"hosts", "github.com"}` is `hosts.github.com`.
std::string TablePath(const std::vector<std::string>& table) {
  std::string out;
  for (std::size_t i = 0; i < table.size(); ++i) {
    out += table[i];
    if (i + 1 < table.size()) {
      out.push_back('.');
    }
  }
  return out;
}

// The table a flat key belongs to, by the same rfind the generator groups on.
// `hosts.github.com.token` lands in `hosts.github.com`, which re-reads as the
// three segments it came from — a host with a dot in it needs no special case.
std::string TableOf(const std::string& key) {
  const std::size_t split = key.rfind('.');
  return split == std::string::npos ? std::string{} : key.substr(0, split);
}

std::string LeafOf(const std::string& key) {
  const std::size_t split = key.rfind('.');
  return split == std::string::npos ? key : key.substr(split + 1);
}

std::string TableHeader(const std::string& table) {
  std::vector<std::string> segments;
  SplitPath(table, &segments);
  std::string out = "[";
  for (std::size_t i = 0; i < segments.size(); ++i) {
    out += QuoteSegment(segments[i]);
    if (i + 1 < segments.size()) {
      out.push_back('.');
    }
  }
  out.push_back(']');
  return out;
}

constexpr const char* kTemplate = R"(# gittop configuration
#
# This file can hold API tokens, so gittop creates it 0600 and never prints a
# token back to you. An environment variable always wins over anything here, so
# CI and throwaway shells never need to write a secret to disk.
#
# Token lookup order, first hit wins:
#   1. GITTOP_TOKEN
#   2. GITHUB_TOKEN or GH_TOKEN   (GitHub hosts)
#      GITLAB_TOKEN or CI_JOB_TOKEN (GitLab hosts)
#   3. a sign-in done in this session, which is not written here until it works
#   4. the matching [hosts."..."] entry below
#
# Signing in from inside gittop writes the token it receives back into this
# file. So does every change made from the Settings tab. Those writes edit this
# file rather than replace it: your comments, your ordering and your spacing all
# survive, and only the values that actually changed are touched.
#
# client_id turns the sign-in into a browser approval instead of a copy-paste.
# It is the id of an OAuth application registered on that host with the device
# flow enabled; there is no client secret, which is what makes it safe to write
# down. Without one, signing in still works — gittop opens the host's token page
# with the scopes already filled in and takes the paste.

[hosts."github.com"]
# Needs no scopes at all for public repositories; `repo` to see private ones.
# token = "ghp_xxxxxxxxxxxxxxxxxxxx"
# client_id = "Iv1.xxxxxxxxxxxxxxxx"

[hosts."gitlab.com"]
# A personal access token with the `read_api` scope, plus `write_repository` if
# you want it to authenticate an https push as well.
# token = "glpat-xxxxxxxxxxxxxxxxxxxx"
# client_id = "xxxxxxxxxxxxxxxxxxxx"

# Self-hosted instances: name the host and say which API it speaks. gittop
# guesses from the hostname when it contains "github" or "gitlab", so this is
# only needed for a host named something else entirely.
# [hosts."git.example.com"]
# provider = "gitlab"          # "github" or "gitlab"
# api = "https://git.example.com/api/v4"
# token = "glpat-xxxxxxxxxxxxxxxxxxxx"
# client_id = "xxxxxxxxxxxxxxxxxxxx"

[pipelines]
# The CI view re-reads runs on a timer while it is the view on screen, and only
# when there is a token: sixty anonymous requests an hour does not survive a
# twenty-second poll. It also stops on its own below a fifth of the remaining
# budget, and says so rather than looking stuck.
# auto_refresh = true
# refresh_seconds = 20         # clamped to 10 … 3600

[layout]
# Which tabs exist and in what order. The digit keys are positional, so this
# also decides what 1 through 9 and 0 reach and what each tab prints as its
# number. Any view left out is simply not there — a good way to lose the three
# network tabs on a repository that has no remote worth watching.
# One or more of: status history branches graph diff stashes remote ci pulls
#                 settings
# views = "status history branches graph diff stashes remote ci pulls settings"

# The view to open on. Has to be one of the views above, or it is ignored:
# starting on a tab no key can reach is worse than starting on the first one.
# start_view = "status"

# Forces the narrow layout at any width — stat cards two by two, no activity
# heatmap. gittop switches to it under 84 columns on its own; this is for
# when you would rather have the rows back.
# compact = false

[theme]
# Everything under here can also be changed from the Settings tab (`0`), and is
# written back the moment you change it — there is no save step. The write edits
# this file in place, so these comments stay where you put them.
#
# One of: default, catppuccin, gruvbox, nord, tokyo-night, dracula, daylight.
# `t` cycles them at run time, which is the quickest way to see them all.
# name = "default"

# How much colour the terminal can show. "auto" reads NO_COLOR, then TERM and
# COLORTERM, and is nearly always right; the rest are for when it is not.
# One of: auto, truecolor, 256, 16, none.
# depth = "auto"

# Which characters gittop draws with. "auto" picks ascii when the locale is not
# UTF-8 or TERM says the terminal has no shapes, and unicode otherwise — it
# never picks nerd, because there is no way to ask a terminal whether its font
# has the icons and guessing wrong fills the screen with empty boxes.
# One of: auto, ascii, unicode, nerd.
# icons = "auto"

# The GitHub and GitLab marks, drawn as their real logos from a Nerd Font. This
# is separate from `icons` on purpose: those two are the only glyphs gittop
# draws that are logos, and no arrangement of geometric shapes is the octocat.
# Needs a patched font — with an ordinary one you get two empty boxes.
# logos = false

# Panel borders. One of: rounded, light, heavy, double.
# heavy falls back to light under icons = "ascii", where its characters are the
# one part of the line-drawing set a non-UTF-8 terminal tends not to have.
# border = "rounded"

# Motion: the eased bars, the spinners, the toast fade and the splash. Off makes
# each of them snap to its final state rather than removing it, so nothing on
# screen disappears — only the movement does.
# animations = true

# The startup card. Skipped automatically when stdout is not a terminal.
# splash = true

# A theme of your own is the chosen one with roles replaced — the same sixteen
# every built-in palette is written in. Anything not named here keeps its value.
# [theme.colors]
# bg = "#0b0f16"
# surface = "#131924"
# surface_alt = "#1e2636"
# surface_raised = "#273145"
# border = "#263043"
# text = "#d7dee8"
# text_dim = "#97a3b6"
# text_faint = "#5e6b7f"
# accent = "#f0883e"
# green = "#56d364"
# yellow = "#e3b341"
# blue = "#79c0ff"
# red = "#ff7b72"
# purple = "#d2a8ff"
# cyan = "#39c5cf"

[keys]
# Rebind an action to one or more keys, space separated. A key is a single
# character, or one of: enter esc tab backtab space up down left right home end
# pageup pagedown insert delete backspace f1 … f12 — optionally prefixed with
# ctrl- or alt-. An empty value unbinds the action entirely.
#
# An action keeps the view it belongs to: moving `discard` to another key does
# not make it fire outside the status view.
#
# quit = "q esc"
# help = "?"
# reload = "r"
# theme = "t"
# next_view = "tab"
# prev_view = "backtab"
#
# The view keys are slots, not names: view_3 is "the third tab", whichever view
# [layout] views puts there.
# view_1 = "1"
# view_2 = "2"
# view_3 = "3"
# view_4 = "4"
# view_5 = "5"
# view_6 = "6"
# view_7 = "7"
# view_8 = "8"
# view_9 = "9"
# down = "j down"
# up = "k up"
# first = "g home"
# last = "G end"
# page_down = "ctrl-d pagedown"
# page_up = "ctrl-u pageup"
# open = "enter"
# filter = "/"
# next_remote = "R"
# fetch = "f"
# pull = "p"
# push = "P"
# stash_save = "S"
# rebase = "B"
# operation = "o"
# toggle_stage = "space"
# stage = "s"
# unstage = "u"
# stage_all = "a"
# discard = "d"
# commit = "c"
#
# Diff view only.
# diff_switch = "s"
# diff_next_file = "]"
# diff_prev_file = "["
#
# Stash view only. `p` here shadows pull, on purpose.
# stash_apply = "a"
# stash_pop = "p"
# stash_drop = "d"
#
# Branches view only.
# prune = "x"
#
# CI view only.
# ci_ref = "b"
# pan_left = "h left"
# pan_right = "l right"
# bucket_day = "d"
# bucket_week = "w"
# bucket_month = "m"
# graph_oldest = "g"
# graph_newest = "G"
)";

}  // namespace

std::string Config::DefaultPath() {
  const std::string explicit_path = Env("GITTOP_CONFIG");
  if (!explicit_path.empty()) {
    return explicit_path;
  }

  const std::string xdg = Env("XDG_CONFIG_HOME");
  if (!xdg.empty()) {
    return (std::filesystem::path(xdg) / "gittop" / "config.toml").string();
  }

  const std::string home = Env("HOME");
  if (home.empty()) {
    return "gittop.toml";  // no home to speak of; stay relative rather than guess
  }
  return (std::filesystem::path(home) / ".config" / "gittop" / "config.toml").string();
}

Config Config::Load(const std::string& path, std::string* error) {
  Config config;

  std::ifstream file(path);
  if (!file) {
    return config;  // absent is the normal case, not a failure
  }

  config.loaded_ = true;

  std::vector<std::string> table;
  std::string line;
  int line_number = 0;

  while (std::getline(file, line)) {
    ++line_number;
    // A CRLF file would otherwise carry its '\r' into every preserved line and
    // grow a second one on the way back out. Parsing never saw it — Trim eats
    // it as whitespace — so dropping it here changes nothing but the rewrite.
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    config.source_.push_back(line);
    const std::string trimmed = Trim(StripComment(line));
    if (trimmed.empty()) {
      continue;
    }

    const auto fail = [&](const std::string& what) {
      if (error != nullptr && error->empty()) {
        *error = path + ":" + std::to_string(line_number) + ": " + what;
      }
    };

    if (trimmed.front() == '[') {
      if (trimmed.back() != ']') {
        fail("unterminated table header");
        continue;
      }
      std::vector<std::string> parsed;
      if (!SplitPath(Trim(trimmed.substr(1, trimmed.size() - 2)), &parsed)) {
        fail("malformed table header");
        continue;
      }
      table = std::move(parsed);
      continue;
    }

    const std::size_t equals = trimmed.find('=');
    if (equals == std::string::npos) {
      fail("expected key = value");
      continue;
    }

    std::vector<std::string> key_path;
    if (!SplitPath(Trim(trimmed.substr(0, equals)), &key_path)) {
      fail("malformed key");
      continue;
    }

    std::string value;
    std::string value_error;
    if (!ParseValue(trimmed.substr(equals + 1), &value, &value_error)) {
      fail(value_error);
      continue;
    }

    std::string full;
    for (const std::string& segment : table) {
      full += segment;
      full.push_back('.');
    }
    for (std::size_t i = 0; i < key_path.size(); ++i) {
      full += key_path[i];
      if (i + 1 < key_path.size()) {
        full.push_back('.');
      }
    }
    config.values_[full] = std::move(value);
  }

  return config;
}

std::string Config::Generate() const {
  // Grouped by table so the written file reads like one a person would write,
  // and so re-reading it produces the same key set.
  std::map<std::string, std::vector<std::pair<std::string, std::string>>> tables;
  for (const auto& [key, value] : values_) {
    tables[TableOf(key)].emplace_back(LeafOf(key), value);
  }

  std::ostringstream out;
  out << "# gittop configuration\n";
  for (const auto& [table, entries] : tables) {
    if (!table.empty()) {
      out << '\n' << TableHeader(table) << '\n';
    }
    for (const auto& [leaf, value] : entries) {
      out << QuoteSegment(leaf) << " = " << FormatValue(value) << '\n';
    }
  }
  return out.str();
}

std::string Config::Rewrite() const {
  std::vector<std::string> out;
  out.reserve(source_.size() + values_.size());

  std::set<std::string> written;

  // Where a key appended to each table should land: one past the last line that
  // table already owns. Without this a new `theme.logos` would be written at the
  // end of the file, under whatever header happened to come last, and mean
  // something else entirely the next time the file was read.
  std::map<std::string, std::size_t> table_end;
  std::vector<std::string> table;

  for (const std::string& line : source_) {
    const std::string trimmed = Trim(StripComment(line));

    if (trimmed.empty()) {  // blank, or a line that is only a comment
      out.push_back(line);
      continue;
    }

    if (trimmed.front() == '[') {
      if (trimmed.back() == ']') {
        std::vector<std::string> parsed;
        if (SplitPath(Trim(trimmed.substr(1, trimmed.size() - 2)), &parsed)) {
          table = std::move(parsed);
        }
      }
      out.push_back(line);
      // Anchored even when the table has no keys yet, so the first setting
      // saved under an empty `[theme]` goes inside it rather than after it.
      table_end[TablePath(table)] = out.size();
      continue;
    }

    const std::size_t equals = trimmed.find('=');
    std::vector<std::string> key_path;
    std::string old_value;
    std::string value_error;
    // Three ways a line can be one gittop does not understand, and all three
    // keep it verbatim. Load only skipped these; Save must not treat "could not
    // read it" as "the user deleted it" and drop somebody's line on the floor.
    if (equals == std::string::npos ||
        !SplitPath(Trim(trimmed.substr(0, equals)), &key_path) ||
        !ParseValue(trimmed.substr(equals + 1), &old_value, &value_error)) {
      out.push_back(line);
      continue;
    }

    const std::string full = JoinKey(table, key_path);
    const auto it = values_.find(full);
    // Gone from `values_` means Unset removed it, so the line goes too — that
    // is what makes a sign-out actually take the token out of the file.
    // Already written means the source named the same key twice, and a second
    // copy of a line we have just rewritten would win on the next read.
    if (it == values_.end() || written.count(full) != 0) {
      continue;
    }
    written.insert(full);

    if (it->second == old_value) {
      out.push_back(line);  // unchanged, so not even the spacing moves
    } else {
      // Rebuilt from the original text left of the '=' rather than from the
      // parsed key, so `hosts."github.com".token` keeps the quoting and the
      // indentation the user chose for it.
      const std::size_t raw_equals = line.find('=');
      const std::size_t comment = CommentAt(line);
      std::string rebuilt = line.substr(0, raw_equals) + "= " + FormatValue(it->second);
      if (comment != std::string::npos && comment > raw_equals) {
        rebuilt += "  " + line.substr(comment);
      }
      out.push_back(std::move(rebuilt));
    }
    table_end[TableOf(full)] = out.size();
  }

  // Anything gittop holds that the file never mentioned. Grouped first, then
  // spliced in back-to-front so that each insertion cannot move the index the
  // next one was measured against.
  std::map<std::string, std::vector<std::string>> pending;
  for (const auto& [key, value] : values_) {
    if (written.count(key) == 0) {
      pending[TableOf(key)].push_back(QuoteSegment(LeafOf(key)) + " = " + FormatValue(value));
    }
  }

  std::map<std::size_t, std::vector<std::string>, std::greater<>> splices;
  for (auto it = pending.begin(); it != pending.end();) {
    const auto anchor = table_end.find(it->first);
    // A top-level key has to precede every header or it would be read as
    // belonging to one, so its fallback anchor is the top of the file.
    if (anchor != table_end.end()) {
      splices[anchor->second] = std::move(it->second);
      it = pending.erase(it);
    } else if (it->first.empty()) {
      splices[0] = std::move(it->second);
      it = pending.erase(it);
    } else {
      ++it;
    }
  }
  for (auto& [at, lines] : splices) {
    out.insert(out.begin() + static_cast<std::ptrdiff_t>(at), lines.begin(), lines.end());
  }

  // Whatever is left names a table the file does not have, so it brings its own
  // header along.
  for (const auto& [table_name, lines] : pending) {
    out.emplace_back();
    out.push_back(TableHeader(table_name));
    out.insert(out.end(), lines.begin(), lines.end());
  }

  std::ostringstream joined;
  for (const std::string& line : out) {
    joined << line << '\n';
  }
  return joined.str();
}

bool Config::Save(const std::string& path, std::string* error) const {
  const std::filesystem::path target(path);
  const std::filesystem::path parent = target.parent_path();

  std::error_code ec;
  if (!parent.empty() && !std::filesystem::exists(parent)) {
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      if (error != nullptr) {
        *error = "cannot create " + parent.string() + ": " + ec.message();
      }
      return false;
    }
    // A token's directory has no business being group- or world-readable.
    ::chmod(parent.c_str(), S_IRWXU);
  }

  const std::string body = loaded_ ? Rewrite() : Generate();

  std::ofstream file(path, std::ios::trunc);
  if (!file) {
    if (error != nullptr) {
      *error = "cannot write " + path;
    }
    return false;
  }
  file << body;
  file.close();
  if (!file) {
    if (error != nullptr) {
      *error = "failed while writing " + path;
    }
    return false;
  }

  if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0 && error != nullptr) {
    *error = "wrote " + path + " but could not set it to 0600";
    return false;
  }
  return true;
}

bool Config::WriteTemplate(const std::string& path, std::string* error) {
  if (std::filesystem::exists(path)) {
    if (error != nullptr) {
      *error = path + " already exists";
    }
    return false;
  }

  const std::filesystem::path parent = std::filesystem::path(path).parent_path();
  if (!parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      if (error != nullptr) {
        *error = "cannot create " + parent.string() + ": " + ec.message();
      }
      return false;
    }
    ::chmod(parent.c_str(), S_IRWXU);
  }

  std::ofstream file(path, std::ios::trunc);
  if (!file) {
    if (error != nullptr) {
      *error = "cannot write " + path;
    }
    return false;
  }
  file << kTemplate;
  file.close();

  ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
  return true;
}

bool Config::Has(const std::string& key) const {
  return values_.find(key) != values_.end();
}

std::string Config::Get(const std::string& key, const std::string& fallback) const {
  const auto it = values_.find(key);
  return it == values_.end() ? fallback : it->second;
}

int Config::GetInt(const std::string& key, int fallback) const {
  const auto it = values_.find(key);
  if (it == values_.end()) {
    return fallback;
  }
  try {
    return std::stoi(it->second);
  } catch (const std::exception&) {
    return fallback;
  }
}

bool Config::GetBool(const std::string& key, bool fallback) const {
  const auto it = values_.find(key);
  if (it == values_.end()) {
    return fallback;
  }
  if (it->second == "true") {
    return true;
  }
  if (it->second == "false") {
    return false;
  }
  return fallback;
}

void Config::Set(const std::string& key, std::string value) {
  values_[key] = std::move(value);
}

bool Config::Unset(const std::string& key) {
  return values_.erase(key) > 0;
}

std::string Config::HostValue(const std::string& host, const std::string& field) const {
  return Get("hosts." + host + "." + field);
}

}  // namespace gittop::config
