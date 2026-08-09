#include "ui/theme.hpp"

namespace gittop::ui {
namespace {

using ftxui::Color;

// The one palette. Phase 7 turns this into a table of named themes loaded from
// config; until then every panel already goes through the token names, so that
// change stays inside this file.
const Theme kDefault{
    .bg = Color::RGB(0x0d, 0x11, 0x17),
    .surface = Color::RGB(0x16, 0x1b, 0x22),
    .surface_alt = Color::RGB(0x21, 0x26, 0x2d),

    .border = Color::RGB(0x33, 0x3e, 0x58),
    .border_focus = Color::RGB(0xf0, 0x88, 0x3e),

    .text = Color::RGB(0xc9, 0xd1, 0xd9),
    .text_dim = Color::RGB(0x8b, 0x94, 0x9e),
    .text_faint = Color::RGB(0x6e, 0x76, 0x81),

    .accent = Color::RGB(0xf0, 0x88, 0x3e),

    .staged = Color::RGB(0x3f, 0xb9, 0x50),
    .unstaged = Color::RGB(0xd2, 0x99, 0x22),
    .untracked = Color::RGB(0x58, 0xa6, 0xff),
    .conflict = Color::RGB(0xf8, 0x51, 0x49),

    .success = Color::RGB(0x3f, 0xb9, 0x50),
    .warning = Color::RGB(0xd2, 0x99, 0x22),
    .danger = Color::RGB(0xf8, 0x51, 0x49),
};

}  // namespace

const Theme& theme() {
  return kDefault;
}

}  // namespace gittop::ui
