#pragma once

// Runtime exploration of the guest address space.
//
// WHY THIS EXISTS. Driving retail's own systems from script - opening the
// character editor, suppressing a menu, forcing a game mode - means calling
// retail functions, and that means knowing their addresses IN THIS BUILD.
// Two obvious sources do not work:
//
//   - The shipped image is encrypted/compressed, so its strings cannot be
//     grepped from disk.
//   - The Hex-Rays .c export in the sibling sk3o tree disagrees about function
//     BOUNDARIES: sub_82D0AFA0 and sub_82590B50 are real in our recompiled
//     sources and absent from it. Good for names and structure, not addresses.
//
// CORRECTION, and it matters: the DECRYPTED DUMP beside that export is this
// exact build, and can be read straight from disk. Verified before being
// trusted - the bytes at 0x825FF4C8 decode to the instructions the recompiled
// source shows for sub_825FF4C8, and the strings at 0x8220EF34 sit exactly
// where the constants in skate3_retail_ui_hooks.cpp already assumed. Read it
// with tools/guest_image.pl (words / str / find / xref / screens); it answers
// most of what this file answers without launching the game, which is how the
// progression unlock flag was found.
//
// This still exists for what the dump CANNOT show: anything the game builds at
// runtime. Heap objects, the front-end manager, and the binding registry -
// which the front end fills in by name ("CAC_GetNumItems",
// "GetEditSkaterOptions"), so the names are in guest memory and the tables
// pointing at them are reachable only from a running game.
//
// Deliberately READ ONLY. Searching and reading cannot corrupt a running game;
// a poke primitive exposed to the console could, and nothing here needs one.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace skate3::guest_probe {

// Guest addresses whose bytes are `text`. Scans the image region only - the
// executable and its static data, not the heap - because that is where
// registered names live and it keeps a console search to a bounded cost.
std::vector<std::uint32_t> FindString(std::string_view text,
                                      std::size_t limit);

// Guest addresses holding the 32-bit big-endian value `value`. Used to walk
// backwards from a string to whatever points at it: a name's address appearing
// in a table is how a binding table is recognised.
std::vector<std::uint32_t> FindU32(std::uint32_t value, std::size_t limit);

bool ReadU32(std::uint32_t address, std::uint32_t& out);

// NUL-terminated string at `address`, empty when unreadable or not plausibly
// text. Plausibility matters: a random word read as a string prints control
// characters into a console.
std::string ReadCString(std::uint32_t address, std::size_t max_length);

}  // namespace skate3::guest_probe
