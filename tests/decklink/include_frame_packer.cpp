#include "output/frame_packer.hpp"

#include <type_traits>

// Proves output/frame_packer.hpp compiles in isolation.
void DecklinkIncludeCheck_FramePacker() {}

// Lock the exclusive-ownership semantics of the packer: no copy, and no move in the
// first version (no cross-thread ownership transfer requirement yet).
static_assert(!std::is_copy_constructible_v<EYEQ::FramePacker>, "FramePacker must not be copy constructible");
static_assert(!std::is_copy_assignable_v<EYEQ::FramePacker>, "FramePacker must not be copy assignable");
static_assert(!std::is_move_constructible_v<EYEQ::FramePacker>, "FramePacker must not be move constructible");
static_assert(!std::is_move_assignable_v<EYEQ::FramePacker>, "FramePacker must not be move assignable");
