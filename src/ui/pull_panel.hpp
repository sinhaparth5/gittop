#pragma once

#include <ftxui/dom/elements.hpp>
#include <vector>

#include "model/pull.hpp"
#include "model/remote.hpp"

namespace gittop::ui {

// Cursor position and whether the detail pane is open. Unlike the CI view's
// drill-down, opening this one costs nothing: everything it shows came back
// with the list, so it is a disclosure rather than a request.
struct PullView {
  int selected = 0;
  bool details_open = false;
};

// `rows` is filled during layout with the box each row landed in, so a click
// can be turned back into an index. Null when nobody is asking.
ftxui::Element PullPanel(const model::PullSnapshot& snapshot, const model::RemoteRef& ref,
                         const PullView& view, int width, int height, int frame,
                         std::vector<ftxui::Box>* rows = nullptr);

// The four boxes of the compose form, in the order they are drawn. App holds the
// selector this indexes and hands it back, which is how ctrl-v resolves "the box
// being typed into" — FTXUI will not say which of its components has focus.
enum PullField : int {
  kPullSource = 0,
  kPullTarget,
  kPullTitle,
  kPullBody,
  kPullFieldCount,
};

// Everything the compose pane draws that is not one of its input boxes.
//
// The two preconditions are separate because they are different news. A branch
// with no upstream is not on the remote at all and the create *will* be refused,
// so it blocks; a branch that is merely ahead opens fine and quietly leaves the
// unpushed commits out of it, which is worth saying and not worth refusing.
// gittop knows both from the status read it has already done, which is why this
// can be said before the request rather than relayed from a 422 afterwards.
struct PullComposeView {
  model::Provider provider = model::Provider::Unknown;
  std::string full_name;    // owner/repo — where it is about to be opened
  std::string remote_name;  // "origin", for the line that says to push there

  bool branch_on_remote = true;
  bool upstream_gone = false;
  std::string upstream;  // named only when it is gone, so the line can say which
  int ahead = 0;

  // The target defaults to the repository's default branch, which arrives with
  // the remote fetch. Nothing is guessed while it is still in flight — a form
  // pre-filled with "main" on a repository whose default is "master" is worse
  // than an empty box, because it looks answered.
  bool default_branch_known = true;

  bool sending = false;
  std::string error;
  std::string hint;

  int field = kPullTitle;
  std::string push_key;  // whatever push is bound to, for "push it first"
};

// Takes the four already-rendered inputs, for the reason PassphrasePane does:
// it is the one way a file under ui/ gets a live Input without depending on the
// component layer. Nothing here is a secret; the shape is the same.
ftxui::Element PullComposePane(const PullComposeView& view, ftxui::Element source,
                               ftxui::Element target, ftxui::Element title,
                               ftxui::Element body);

}  // namespace gittop::ui
