#include "skate3_guest_probe.h"

#include <fstream>

#include "skate3_native_scene_state.h"
#include "native/skate3_native_guest_read.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace skate3::guest_probe {

namespace {

// Two regions, because the thing being looked for lives in either.
//
// The image and its static data hold the name strings and the globals that
// registration writes into. The heap holds what the front end builds at
// runtime - the observed front-end manager was 0x43A88F80, which is why the
// lower region is scanned at all.
//
// Not the whole address space: most of it is unmapped, and every unmapped
// chunk is a failed copy to no purpose.
struct Region {
  std::uint32_t begin;
  std::uint32_t end;
};
constexpr Region kRegions[] = {
    {0x82000000u, 0x8A000000u},  // image + static data
    {0x40000000u, 0x50000000u},  // guest heap
};

// Read in chunks through the guarded copy: streaming can leave holes in the
// range, and a fault must skip a chunk rather than end the search.
constexpr std::size_t kChunk = 64 * 1024;

std::uint32_t LoadBe32(const std::uint8_t* bytes) {
  return (static_cast<std::uint32_t>(bytes[0]) << 24) |
         (static_cast<std::uint32_t>(bytes[1]) << 16) |
         (static_cast<std::uint32_t>(bytes[2]) << 8) |
         static_cast<std::uint32_t>(bytes[3]);
}

// Scans the image, handing each chunk to `visit(chunk_address, data, size)`.
// `overlap` keeps a match that straddles a chunk boundary findable.
template <typename Visit>
void ScanImage(std::size_t overlap, Visit visit) {
  std::uint8_t* base = native_scene::g_guest_base.load(std::memory_order_relaxed);
  if (base == nullptr) {
    return;
  }
  std::vector<std::uint8_t> buffer(kChunk + overlap);
  for (const Region& region : kRegions) {
    for (std::uint32_t address = region.begin; address < region.end;
         address += static_cast<std::uint32_t>(kChunk)) {
      const std::size_t want =
          std::min<std::size_t>(kChunk + overlap, region.end - address);
      if (!native_scene::GuestTryCopy(buffer.data(), base + address, want)) {
        continue;  // not mapped right now; the next chunk may be
      }
      if (!visit(address, buffer.data(), want)) {
        return;
      }
    }
  }
}

}  // namespace

std::vector<std::uint32_t> FindString(std::string_view text,
                                      std::size_t limit) {
  std::vector<std::uint32_t> found;
  if (text.empty() || limit == 0) {
    return found;
  }
  ScanImage(text.size(), [&](std::uint32_t address, const std::uint8_t* data,
                             std::size_t size) {
    const std::uint8_t* begin = data;
    const std::uint8_t* end = data + size;
    while (true) {
      const std::uint8_t* hit = std::search(
          begin, end, text.begin(), text.end());
      if (hit == end) {
        break;
      }
      found.push_back(address +
                      static_cast<std::uint32_t>(hit - data));
      if (found.size() >= limit) {
        return false;
      }
      begin = hit + 1;
    }
    return true;
  });
  return found;
}

std::vector<std::uint32_t> FindU32(std::uint32_t value, std::size_t limit) {
  std::vector<std::uint32_t> found;
  if (limit == 0) {
    return found;
  }
  ScanImage(4, [&](std::uint32_t address, const std::uint8_t* data,
                   std::size_t size) {
    if (size < 4) {
      return true;
    }
    // Word-aligned only: a pointer in a table is aligned, and unaligned hits
    // are almost always instruction bytes that happen to match.
    for (std::size_t offset = 0; offset + 4 <= size; offset += 4) {
      if (LoadBe32(data + offset) == value) {
        found.push_back(address + static_cast<std::uint32_t>(offset));
        if (found.size() >= limit) {
          return false;
        }
      }
    }
    return true;
  });
  return found;
}

bool ReadU32(std::uint32_t address, std::uint32_t& out) {
  std::uint8_t* base = native_scene::g_guest_base.load(std::memory_order_relaxed);
  if (base == nullptr) {
    return false;
  }
  std::array<std::uint8_t, 4> bytes{};
  if (!native_scene::GuestTryCopy(bytes.data(), base + address, bytes.size())) {
    return false;
  }
  out = LoadBe32(bytes.data());
  return true;
}

std::string ReadCString(std::uint32_t address, std::size_t max_length) {
  std::uint8_t* base = native_scene::g_guest_base.load(std::memory_order_relaxed);
  if (base == nullptr || max_length == 0) {
    return {};
  }
  std::vector<std::uint8_t> bytes(std::min<std::size_t>(max_length, 512));
  if (!native_scene::GuestTryCopy(bytes.data(), base + address, bytes.size())) {
    return {};
  }
  std::string out;
  for (const std::uint8_t byte : bytes) {
    if (byte == 0) {
      break;
    }
    // Printable ASCII only. A non-text word read as a string would otherwise
    // spray control characters through the console.
    if (byte < 0x20 || byte > 0x7E) {
      return {};
    }
    out.push_back(static_cast<char>(byte));
  }
  return out;
}

}  // namespace skate3::guest_probe
