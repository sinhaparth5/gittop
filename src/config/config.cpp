#include "config/config.hpp"

#include <sys/stat.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
# file. That rewrite is generated from the settings gittop is holding, so
# comments you add here do not survive it.
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
# Everything under here can also be changed from the Settings tab (`0`), and
# saved back from the row that says so. Saving regenerates this file from the
# settings gittop is holding, which means these comments do not survive it.
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

  std::vector<std::string> table;
  std::string line;
  int line_number = 0;

  while (std::getline(file, line)) {
    ++line_number;
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

  // Grouped by table so the written file reads like one a person would write,
  // and so re-reading it produces the same key set.
  std::map<std::string, std::vector<std::pair<std::string, std::string>>> tables;
  for (const auto& [key, value] : values_) {
    const std::size_t split = key.rfind('.');
    const std::string table = split == std::string::npos ? std::string{} : key.substr(0, split);
    const std::string leaf = split == std::string::npos ? key : key.substr(split + 1);
    tables[table].emplace_back(leaf, value);
  }

  std::ostringstream out;
  out << "# gittop configuration\n";
  for (const auto& [table, entries] : tables) {
    if (!table.empty()) {
      std::vector<std::string> segments;
      SplitPath(table, &segments);
      out << "\n[";
      for (std::size_t i = 0; i < segments.size(); ++i) {
        out << QuoteSegment(segments[i]);
        if (i + 1 < segments.size()) {
          out << '.';
        }
      }
      out << "]\n";
    }
    for (const auto& [leaf, value] : entries) {
      out << QuoteSegment(leaf) << " = " << FormatValue(value) << '\n';
    }
  }

  std::ofstream file(path, std::ios::trunc);
  if (!file) {
    if (error != nullptr) {
      *error = "cannot write " + path;
    }
    return false;
  }
  file << out.str();
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
