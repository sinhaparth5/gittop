#include "ui/workspace_panel.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "ui/glyphs.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

namespace gittop::ui {
namespace {

using namespace ftxui;  // NOLINT: the dom DSL reads badly when qualified.

constexpr int kNameCells = 24;
constexpr int kStateCells = 14;

Element Row(Elements parts, bool selected, Box* box) {
  Element row = hbox(std::move(parts));
  if (selected) {
    row = std::move(row) | bgcolor(theme().surface_alt) | focus;
  }
  if (box != nullptr) {
    row = std::move(row) | reflect(*box);
  }
  return row;
}

Element WorktreeRow(const model::Worktree& tree, bool selected, int width, Box* box) {
  const Theme& t = theme();
  const GlyphSet& g = glyphs();

  // The glyph carries the state as well as the colour, as every status does.
  Element state;
  if (tree.missing) {
    state = text(Fit(g.gone + std::string(" missing"), kStateCells)) | color(t.warning);
  } else if (tree.changes > 0) {
    state = text(Fit(g.unstaged + std::string(" ") + std::to_string(tree.changes) +
                         (tree.changes == 1 ? " change" : " changes"),
                     kStateCells)) |
            color(t.unstaged);
  } else {
    state = text(Fit("clean", kStateCells)) | color(t.text_faint);
  }

  std::string tags;
  if (tree.main) {
    tags += "main ";
  }
  if (tree.locked) {
    tags += "locked ";
  }

  const std::string branch =
      tree.missing ? "" : tree.branch.empty() ? "no commits" : tree.branch;
  const int path_cells = std::max(8, width - kNameCells - kStateCells - 20);
  return Row(
      {
          text(selected ? g.cursor : " ") | color(t.accent),
          text(" "),
          text(tree.current ? g.branch : tree.detached ? g.detached : g.bullet) | bold |
              color(tree.current ? t.staged : t.text_faint),
          text("  "),
          text(Fit(branch, kNameCells)) | bold |
              color(selected || tree.current ? t.text : t.text_dim),
          text("  "),
          state,
          text(Fit(tags, 8)) | color(t.accent),
          text(Truncate(tree.path, path_cells)) | color(t.text_faint),
          filler(),
      },
      selected, box);
}

Element SubmoduleRow(const model::Submodule& module, bool selected, int width, Box* box) {
  const Theme& t = theme();
  const GlyphSet& g = glyphs();

  // Behind HEAD's record is the ordinary reason to press `u`, so it is named;
  // changes inside the submodule are a different thing and say so.
  Element state;
  if (!module.cloned) {
    state = text(Fit(g.absent + std::string(" not cloned"), kStateCells)) | color(t.warning);
  } else if (!module.recorded.empty() && module.recorded != module.checked_out) {
    state = text(Fit(g.unstaged + std::string(" moved"), kStateCells)) | color(t.unstaged);
  } else if (module.dirty) {
    state = text(Fit(g.unstaged + std::string(" dirty"), kStateCells)) | color(t.unstaged);
  } else {
    state = text(Fit("in sync", kStateCells)) | color(t.text_faint);
  }

  // Recorded, then what is checked out: the arrow reads "HEAD wants this, the
  // tree has that", and the two are only worth both printing when they differ.
  std::string ids = module.recorded.empty() ? "-------" : module.recorded;
  if (module.cloned && module.checked_out != module.recorded) {
    ids += " " + std::string(g.arrow_right) + " " + module.checked_out;
  }
  const int url_cells = std::max(8, width - kNameCells - kStateCells - 30);
  return Row(
      {
          text(selected ? g.cursor : " ") | color(t.accent),
          text(" "),
          text(g.node) | color(module.cloned ? t.text_faint : t.warning),
          text("  "),
          text(Fit(module.path, kNameCells)) | bold | color(selected ? t.text : t.text_dim),
          text("  "),
          state,
          text(Fit(ids, 18)) | color(t.text_faint),
          text(Truncate(SafeUrl(module.url), url_cells)) | color(t.text_faint),
          filler(),
      },
      selected, box);
}

Element Empty(const std::string& what, const std::string& hint) {
  const Theme& t = theme();
  return vbox({
      text(""),
      hbox({filler(), text(what) | color(t.text), filler()}),
      hbox({filler(), text(hint) | color(t.text_faint), filler()}),
      text(""),
  });
}

}  // namespace

Element WorkspacePanel(const model::Workspace& workspace, int selected, int width,
                       const std::string& add_key, std::vector<Box>* row_boxes) {
  if (row_boxes != nullptr) {
    row_boxes->assign(workspace.rows(), Box());
  }
  const auto box = [row_boxes](std::size_t i) {
    return row_boxes != nullptr ? &(*row_boxes)[i] : nullptr;
  };

  const std::size_t trees = workspace.worktrees.size();
  Elements tree_rows;
  for (std::size_t i = 0; i < trees; ++i) {
    tree_rows.push_back(WorktreeRow(workspace.worktrees[i], static_cast<int>(i) == selected,
                                    width, box(i)));
  }
  Elements module_rows;
  for (std::size_t i = 0; i < workspace.submodules.size(); ++i) {
    module_rows.push_back(SubmoduleRow(workspace.submodules[i],
                                       static_cast<int>(trees + i) == selected, width,
                                       box(trees + i)));
  }

  // The panel holding the cursor is the focused one, so which list the keys
  // act on is answered by the border rather than left to be worked out.
  const bool in_trees = selected < static_cast<int>(trees);
  const auto count = [](std::size_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
  };
  Element worktrees =
      Panel("WORKTREES",
            tree_rows.size() > 1
                ? Scrollable(vbox(std::move(tree_rows)))
                : vbox({vbox(std::move(tree_rows)),
                        Empty("no other worktrees",
                              add_key.empty() ? ""
                                              : add_key + " checks a branch out beside this one")}),
            {.focused = in_trees, .note = count(trees, "worktree", "worktrees")});
  Element submodules =
      Panel("SUBMODULES",
            module_rows.empty() ? Empty("no submodules", "this repository records none")
                                : Scrollable(vbox(std::move(module_rows))),
            {.focused = !in_trees,
             .note = count(workspace.submodules.size(), "submodule", "submodules")});
  return vbox({std::move(worktrees) | flex, std::move(submodules) | flex}) | flex;
}

}  // namespace gittop::ui
