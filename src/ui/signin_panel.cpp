#include "ui/signin_panel.hpp"

#include <string>
#include <utility>

#include "ui/glyphs.hpp"
#include "ui/panels.hpp"
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

namespace gittop::ui {
namespace {

using ftxui::Element;
using ftxui::Elements;
using ftxui::bold;
using ftxui::color;
using ftxui::filler;
using ftxui::flex;
using ftxui::hbox;
using ftxui::separator;
using ftxui::size;
using ftxui::text;
using ftxui::vbox;
using ftxui::WIDTH;
using ftxui::GREATER_THAN;
using model::Provider;

// Wide enough for a GitHub device URL and a token page URL with its query
// string, which are the two longest things this pane ever has to print without
// being allowed to shorten them: a truncated URL is not a URL.
constexpr int kPaneCells = 74;

std::string ProviderGlyph(Provider provider) {
  const GlyphSet& g = glyphs();
  switch (provider) {
    case Provider::GitHub:
      return g.github;
    case Provider::GitLab:
      return g.gitlab;
    case Provider::Unknown:
      break;
  }
  return g.provider_unknown;
}

Element Line(const std::string& body, ftxui::Color tint) {
  return hbox({text("  "), text(body) | color(tint)});
}

// A URL is printed whole or not at all. Everything else in gittop truncates
// with an ellipsis, which is right for a commit summary and wrong here — a URL
// with its tail replaced by "…" is one the user cannot type, and this pane
// exists precisely so that they can.
Element Url(const std::string& url) {
  const Theme& t = theme();
  return hbox({text("     "), text(url) | color(t.accent) | flex});
}

Element Step(int number, const std::string& body) {
  const Theme& t = theme();
  return hbox({
      text("  " + std::to_string(number) + ". ") | color(t.text_dim),
      text(body) | color(t.text),
  });
}

// The code is the one thing on this pane the user has to copy by eye, so it is
// given the widest spacing and the strongest colour rather than being set as
// ordinary text. Letter-spacing it is not decoration: "WDJB" reads wrong about
// as often as it reads right at terminal font sizes.
Element UserCode(const std::string& code) {
  const Theme& t = theme();
  std::string spaced;
  for (const char c : code) {
    if (!spaced.empty()) {
      spaced.push_back(' ');
    }
    spaced.push_back(c);
  }
  return hbox({text("     "), text(spaced) | bold | color(t.accent)});
}

std::string Countdown(std::int64_t expires_at, std::int64_t now) {
  if (expires_at <= 0 || now <= 0 || expires_at <= now) {
    return {};
  }
  const std::int64_t left = expires_at - now;
  const std::int64_t minutes = left / 60;
  const std::int64_t seconds = left % 60;
  std::string out = std::to_string(minutes) + ":";
  if (seconds < 10) {
    out.push_back('0');
  }
  return out + std::to_string(seconds);
}

Element Title(const SignInView& view) {
  const Theme& t = theme();
  const GlyphSet& g = glyphs();
  return hbox({
      text(std::string(" ") + g.key + " ") | color(t.accent),
      text("sign in") | bold | color(t.accent),
      text("  "),
      text(ProviderGlyph(view.provider)) | color(t.text_dim),
      text(" " + view.host) | color(t.text_dim),
  });
}

Elements StartingRows(const SignInView& view, int frame) {
  const Theme& t = theme();
  return {hbox({
      text("  "),
      text(SpinnerFrame(frame) + " ") | color(t.accent),
      text("Asking " + view.host + " for a code" + glyphs().ellipsis) | color(t.text_dim),
  })};
}

Elements WaitingRows(const SignInView& view, int frame) {
  const Theme& t = theme();
  Elements rows;

  rows.push_back(Step(1, "Open this page:"));
  rows.push_back(Url(view.verification_uri));
  rows.push_back(text(""));
  rows.push_back(Step(2, "Enter this code:"));
  rows.push_back(UserCode(view.user_code));
  rows.push_back(text(""));

  const std::string left = Countdown(view.expires_at, view.now);
  Elements status{
      text("  "),
      text(SpinnerFrame(frame) + " ") | color(t.accent),
      text("waiting for approval") | color(t.text_dim),
  };
  if (!left.empty()) {
    status.push_back(filler());
    status.push_back(text("expires in " + left + "  ") | color(t.text_dim));
  }
  rows.push_back(hbox(std::move(status)));

  if (view.browser_failed) {
    rows.push_back(Line("Could not open a browser here — the URL above is the whole of it.",
                        t.text_dim));
  }
  return rows;
}

Elements PasteRows(const SignInView& view, Element input) {
  const Theme& t = theme();
  Elements rows;

  // Why the user is being asked to paste rather than approve. Two different
  // reasons reach this stage and they are not interchangeable: no application
  // registered is a thing they can fix, and a device request that was refused
  // is a thing that already went wrong. Printing the first when the second
  // happened would send them off configuring a client_id they already have.
  if (view.error.empty()) {
    rows.push_back(Line("No OAuth application is registered for this host, so the", t.text_dim));
    rows.push_back(Line("one-click sign-in is not available here.", t.text_dim));
  } else {
    rows.push_back(hbox({
        text("  "),
        text(std::string(glyphs().warning) + " ") | color(t.warning),
        text(view.error) | color(t.text),
    }));
    if (!view.hint.empty()) {
      rows.push_back(Line(view.hint, t.text_dim));
    }
  }
  rows.push_back(text(""));
  rows.push_back(Step(1, "Create a token — the name and scopes are already filled in:"));
  rows.push_back(Url(view.token_page_url));
  rows.push_back(text(""));
  rows.push_back(Step(2, "Paste it here:"));
  rows.push_back(text(""));
  rows.push_back(hbox({
      text(std::string("     ") + glyphs().prompt + " ") | color(t.accent),
      std::move(input) | flex,
  }));
  return rows;
}

Elements GrantedRows(const SignInView& view) {
  const Theme& t = theme();
  const GlyphSet& g = glyphs();
  Elements rows;

  rows.push_back(hbox({
      text("  "),
      text(std::string(g.check) + " ") | color(t.success),
      text("Signed in to " + view.host) | color(t.text),
  }));
  rows.push_back(text(""));

  if (!view.saved_to.empty()) {
    // Truncated where the URLs above are not: a path here is telling the user
    // which file to go and edit, and one that runs off the edge tears the
    // border instead of saying it ran out of room. A URL has to stay typable;
    // this only has to stay recognisable.
    rows.push_back(Line("Saved to " + Truncate(view.saved_to, kPaneCells - 12), t.text_dim));
    rows.push_back(Line("The file is 0600. Delete that line to sign out.", t.text_dim));
  } else {
    // The distinction that matters on the next launch, so it is stated on this
    // one. A sign-in that quietly evaporates looks like a sign-in that failed.
    rows.push_back(hbox({
        text("  "),
        text(std::string(g.warning) + " ") | color(t.warning),
        text("Held for this session only — it was not written to disk.") | color(t.text),
    }));
    if (!view.save_error.empty()) {
      rows.push_back(Line(view.save_error, t.text_dim));
    }
  }
  return rows;
}

Elements FailedRows(const SignInView& view) {
  const Theme& t = theme();
  const GlyphSet& g = glyphs();
  Elements rows;

  rows.push_back(hbox({
      text("  "),
      text(std::string(g.cross) + " ") | color(t.danger),
      text(view.error.empty() ? "Sign-in failed." : view.error) | color(t.text),
  }));
  if (!view.hint.empty()) {
    rows.push_back(Line(view.hint, t.text_dim));
  }
  return rows;
}

Elements FooterFor(const SignInView& view) {
  Elements chips{text(" ")};

  switch (view.stage) {
    case SignInStage::Starting:
      break;
    case SignInStage::Waiting:
      chips.push_back(Chip("o", "open browser"));
      break;
    case SignInStage::Paste:
      // ctrl-o rather than o: the field below is focused and taking letters,
      // and one of them going missing out of a pasted token would be a failure
      // with no visible cause.
      chips.push_back(Chip("ctrl-o", "open browser"));
      chips.push_back(Chip("enter", "sign in"));
      break;
    case SignInStage::Granted:
      chips.push_back(Chip("enter", "done"));
      break;
    case SignInStage::Failed:
      chips.push_back(Chip("r", "try again"));
      break;
  }

  chips.push_back(filler());
  chips.push_back(Chip("esc", view.stage == SignInStage::Granted ? "close" : "cancel"));
  return chips;
}

}  // namespace

Element SignInPane(const SignInView& view, Element paste_input, int frame) {
  const Theme& t = theme();

  Elements rows{Title(view), separator() | color(t.border)};

  Elements body;
  switch (view.stage) {
    case SignInStage::Starting:
      body = StartingRows(view, frame);
      break;
    case SignInStage::Waiting:
      body = WaitingRows(view, frame);
      break;
    case SignInStage::Paste:
      body = PasteRows(view, std::move(paste_input));
      break;
    case SignInStage::Granted:
      body = GrantedRows(view);
      break;
    case SignInStage::Failed:
      body = FailedRows(view);
      break;
  }
  for (Element& row : body) {
    rows.push_back(std::move(row));
  }

  rows.push_back(separator() | color(t.border));
  rows.push_back(hbox(FooterFor(view)));

  return vbox(std::move(rows)) | PaneFrame() | size(WIDTH, GREATER_THAN, kPaneCells);
}

}  // namespace gittop::ui
