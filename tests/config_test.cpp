#include <doctest/doctest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "scratch_dir.hpp"

using gittop::config::Config;

namespace {

std::string Slurp(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

}  // namespace

TEST_CASE("reading strings, ints, bools and quoted host keys") {
  ScratchDir dir;
  const auto file = dir.path() / "config.toml";
  std::ofstream(file) << "[theme]\nname = \"nord\"\n[ci]\ninterval = 30\n"
                         "[hosts.\"gitlab.internal\"]\nprovider = \"gitlab\"\nlogos = true\n";
  std::string error;
  const Config config = Config::Load(file.string(), &error);
  CHECK(error.empty());
  CHECK(config.Get("theme.name") == "nord");
  CHECK(config.GetInt("ci.interval", 0) == 30);
  CHECK(config.HostValue("gitlab.internal", "provider") == "gitlab");
  CHECK(config.GetBool("hosts.gitlab.internal.logos", false));
}

TEST_CASE("a save edits the file in place rather than regenerating it") {
  ScratchDir dir;
  const auto file = dir.path() / "config.toml";
  const std::string original =
      "# written by hand\n"
      "[theme]\n"
      "name   =   \"default\"   # my favourite\n"
      "border = \"rounded\"\n"
      "\n"
      "[keys]\n"
      "quit = \"q\"\n";
  std::ofstream(file) << original;

  std::string error;
  Config config = Config::Load(file.string(), &error);
  REQUIRE(error.empty());
  config.Set("theme.name", "nord");
  config.Set("theme.logos", "true");
  REQUIRE(config.Save(file.string(), &error));

  const std::string saved = Slurp(file);
  CHECK(saved.find("# written by hand\n") != std::string::npos);
  CHECK(saved.find("border = \"rounded\"\n") != std::string::npos);  // untouched, byte for byte
  CHECK(saved.find("nord") != std::string::npos);
  CHECK(saved.find("default") == std::string::npos);

  // A new key is spliced in under its own table, not appended after [keys],
  // where it would read back as keys.logos.
  const Config reread = Config::Load(file.string(), &error);
  CHECK(reread.Get("theme.logos") == "true");
  CHECK_FALSE(reread.Has("keys.logos"));
  CHECK(reread.Get("keys.quit") == "q");
}

TEST_CASE("a missing file is an empty config, not an error") {
  ScratchDir dir;
  std::string error;
  const Config config = Config::Load((dir.path() / "absent.toml").string(), &error);
  CHECK(error.empty());
  CHECK(config.empty());
}

TEST_CASE("arrays of tables number their elements, and a save keeps them") {
  ScratchDir dir;
  const auto file = dir.path() / "config.toml";
  const std::string original =
      "[theme]\nname = \"nord\"\n\n"
      "[[commands]]\nkey = \"ctrl-f\"\nrun = [\"git\", \"fetch\", \"{remote}\"]  # note\n\n"
      "[[commands]]\nkey = \"F\"\nrun = [ \"echo\", \"a \\\"b\\\", c\", ]\nconfirm = true\n";
  std::ofstream(file) << original;

  std::string error;
  Config config = Config::Load(file.string(), &error);
  CHECK(error.empty());
  CHECK(config.TableCount("commands") == 2);
  CHECK(config.Get("commands.0.key") == "ctrl-f");
  CHECK(config.GetList("commands.0.run") ==
        std::vector<std::string>{"git", "fetch", "{remote}"});
  CHECK(config.GetList("commands.1.run") == std::vector<std::string>{"echo", "a \"b\", c"});
  CHECK(config.GetBool("commands.1.confirm", false));

  config.Set("theme.name", "dracula");
  REQUIRE(config.Save(file.string(), &error));
  std::string expected = original;
  expected.replace(expected.find("nord"), 4, "dracula");
  CHECK(Slurp(file) == expected);

  std::ofstream(file) << "[x]\nrun = [\"git\", 3]\n";
  Config::Load(file.string(), &error);
  CHECK(error.find("quoted strings") != std::string::npos);
}
