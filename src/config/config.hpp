#pragma once

#include <map>
#include <string>
#include <vector>

namespace gittop::config {

// A deliberately small TOML reader: comments, table headers with dotted and
// quoted segments, and `key = value` where value is a quoted string, an
// integer, or a bool. That is the whole language gittop's config speaks.
//
// It is a strict subset rather than a lookalike, so the files it reads are also
// valid TOML. Swapping in a real parser later is then a drop-in rather than a
// migration of everyone's config file.
class Config {
 public:
  // Returns the path a config would be read from, whether or not one is there:
  //   $GITTOP_CONFIG, else $XDG_CONFIG_HOME/gittop/config.toml,
  //   else $HOME/.config/gittop/config.toml.
  static std::string DefaultPath();

  // A missing file is not an error: it yields an empty config and leaves
  // `error` alone. Only a file that exists and does not parse sets it.
  static Config Load(const std::string& path, std::string* error);

  // Writes every key currently held. Creates parent directories 0700 and the
  // file 0600, because this is where a token can end up. Returns false and
  // fills `error` on any failure.
  //
  // A config that was loaded from a file is *edited*, not regenerated: comments,
  // blank lines, key order and the user's own spacing all survive, and only the
  // values that actually changed are rewritten. That is what makes it safe to
  // call this on every keystroke — the settings page now does, and the old
  // "press save" row existed precisely because a regenerating write ate a
  // hand-written file's comments the first time somebody pressed `t`.
  //
  // A config that was never on disk has no layout to preserve, so it falls back
  // to generating one grouped by table.
  bool Save(const std::string& path, std::string* error) const;

  // Writes a commented starting point with every token line left commented out.
  // Refuses to touch an existing file, so it can never eat a real config.
  static bool WriteTemplate(const std::string& path, std::string* error);

  // Keys are the full dotted path with quotes stripped: hosts.github.com.token.
  std::string Get(const std::string& key, const std::string& fallback = {}) const;
  bool Has(const std::string& key) const;
  int GetInt(const std::string& key, int fallback) const;
  bool GetBool(const std::string& key, bool fallback) const;
  void Set(const std::string& key, std::string value);

  // Removes a key entirely. Distinct from setting it empty, which would write
  // `token = ""` back out and leave a line that reads like a configured secret
  // with the secret missing. Returns whether there was one to remove, so a
  // sign-out can tell "cleared it" from "there was nothing on disk".
  bool Unset(const std::string& key);

  // Per-host settings live under hosts."<host>". The host is a table key rather
  // than part of the key path so a self-hosted instance needs no code change.
  std::string HostValue(const std::string& host, const std::string& field) const;

  bool empty() const { return values_.empty(); }
  const std::map<std::string, std::string>& values() const { return values_; }

 private:
  // Regenerates the file from `values_` alone, grouped back into tables. The
  // shape a config gets when there was nothing on disk to preserve.
  std::string Generate() const;

  // Replays `source_` and swaps in the values that changed, which is the only
  // way a comment survives a write.
  std::string Rewrite() const;

  std::map<std::string, std::string> values_;

  // The file exactly as Load read it, one entry per line, so Save can edit it
  // rather than replace it. Empty for a config that was never on disk — which
  // is what `loaded_` distinguishes from a file that was there and was empty.
  std::vector<std::string> source_;
  bool loaded_ = false;
};

}  // namespace gittop::config
