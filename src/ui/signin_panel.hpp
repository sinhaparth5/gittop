#pragma once

#include <cstdint>
#include <ftxui/dom/elements.hpp>
#include <string>

#include "model/remote.hpp"

namespace gittop::ui {

// Where a sign-in has got to. Two routes reach Granted: the device flow, and
// pasting a personal access token. They are one pane rather than two because
// from the user's side they are one question — "let gittop see more than an
// anonymous client can" — and the fallback is only reached by the first one
// being unavailable, which is not a decision worth making them take.
enum class SignInStage {
  Starting,  // asking the provider for a device code
  Waiting,   // code is on screen, polling for approval
  Paste,     // no device flow here: the guided token route
  Granted,
  Failed,
};

// Everything the pane draws. No token and no device code: the first is a secret
// and the second is the bearer of one for the length of the flow, so neither
// crosses into ui/ — the same rule PassphrasePane is built around.
struct SignInView {
  SignInStage stage = SignInStage::Starting;
  model::Provider provider = model::Provider::Unknown;
  std::string host;

  std::string user_code;         // the characters to type in
  std::string verification_uri;  // where to type them
  std::string token_page_url;    // the Paste stage's deep link

  std::int64_t expires_at = 0;  // unix seconds, 0 when the provider did not say
  std::int64_t now = 0;         // passed in rather than read here, so the pane stays pure

  // Where the token ended up. `saved_to` is a path and never a value; an empty
  // path with Granted means it lives only in this process, which the pane says
  // outright rather than leaving to be discovered on the next launch.
  std::string saved_to;
  std::string save_error;

  bool browser_failed = false;
  std::string error;
  std::string hint;
};

// `paste_input` is the already-rendered Input for the Paste stage, in password
// mode, for the reason PassphrasePane takes one: this function then cannot read
// what was typed even in principle.
ftxui::Element SignInPane(const SignInView& view, ftxui::Element paste_input, int frame);

}  // namespace gittop::ui
