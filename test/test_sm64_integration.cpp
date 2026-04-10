/*!
 * @file test_sm64_integration.cpp
 * Tests for the libsm64 integration safety checks.
 * Validates that write_mario_pos_to_target properly handles:
 *   - null/false target pointers
 *   - out-of-bounds memory access
 *   - null root pointers inside the target process
 *   - valid writes to well-formed memory
 */

#include "gtest/gtest.h"

#include "game/libsm64/libsm64_integration.h"

class SM64SyncTest : public ::testing::Test {
 protected:
  // Simulated EE memory (small buffer for testing)
  static constexpr u32 MEM_SIZE = 4096;
  static constexpr u32 FALSE_VAL = 0x100;  // Simulated #f address

  // GOAL layout offsets, adjusted for boxed types: declared offset - 4
  // process-drawable.root :offset 112  → runtime 108
  // trs.trans            :offset 16   → runtime 12
  static constexpr u32 ROOT_FIELD_OFFSET = 108;
  static constexpr u32 TRANS_OFFSET = 12;

  u8 mem[MEM_SIZE];

  void SetUp() override { std::memset(mem, 0, MEM_SIZE); }

  // Helper: set up a valid process-drawable at target_addr with root at root_addr
  void setup_valid_target(u32 target_addr, u32 root_addr) {
    // Write root pointer into target_addr + ROOT_FIELD_OFFSET (runtime: 108)
    std::memcpy(mem + target_addr + ROOT_FIELD_OFFSET, &root_addr, 4);
  }
};

TEST_F(SM64SyncTest, NullMemReturnsfalse) {
  math::Vector3f pos(1.0f, 2.0f, 3.0f);
  EXPECT_FALSE(
      sm64::LibSM64Manager::write_mario_pos_to_target(nullptr, MEM_SIZE, FALSE_VAL, 200, pos));
}

TEST_F(SM64SyncTest, ZeroTargetPtrReturnsFalse) {
  math::Vector3f pos(1.0f, 2.0f, 3.0f);
  EXPECT_FALSE(sm64::LibSM64Manager::write_mario_pos_to_target(mem, MEM_SIZE, FALSE_VAL, 0, pos));
}

TEST_F(SM64SyncTest, FalseTargetPtrReturnsFalse) {
  math::Vector3f pos(1.0f, 2.0f, 3.0f);
  // target_ptr == #f should be rejected
  EXPECT_FALSE(
      sm64::LibSM64Manager::write_mario_pos_to_target(mem, MEM_SIZE, FALSE_VAL, FALSE_VAL, pos));
}

TEST_F(SM64SyncTest, OutOfBoundsTargetReturnsFalse) {
  math::Vector3f pos(1.0f, 2.0f, 3.0f);
  // target_ptr + 112 (108 root field + 4 bytes) > MEM_SIZE
  u32 bad_target = MEM_SIZE - 50;  // 50 bytes from end, needs 112
  EXPECT_FALSE(
      sm64::LibSM64Manager::write_mario_pos_to_target(mem, MEM_SIZE, FALSE_VAL, bad_target, pos));
}

TEST_F(SM64SyncTest, NullRootPtrReturnsFalse) {
  math::Vector3f pos(1.0f, 2.0f, 3.0f);
  u32 target_addr = 512;
  // Root pointer at target+112 is 0 (default from memset)
  EXPECT_FALSE(
      sm64::LibSM64Manager::write_mario_pos_to_target(mem, MEM_SIZE, FALSE_VAL, target_addr, pos));
}

TEST_F(SM64SyncTest, FalseRootPtrReturnsFalse) {
  math::Vector3f pos(1.0f, 2.0f, 3.0f);
  u32 target_addr = 512;
  // Set root pointer to #f
  setup_valid_target(target_addr, FALSE_VAL);
  EXPECT_FALSE(
      sm64::LibSM64Manager::write_mario_pos_to_target(mem, MEM_SIZE, FALSE_VAL, target_addr, pos));
}

TEST_F(SM64SyncTest, OutOfBoundsRootReturnsFalse) {
  math::Vector3f pos(1.0f, 2.0f, 3.0f);
  u32 target_addr = 512;
  u32 bad_root = MEM_SIZE - 10;  // root + 28 (12 + 16) > MEM_SIZE
  setup_valid_target(target_addr, bad_root);
  EXPECT_FALSE(
      sm64::LibSM64Manager::write_mario_pos_to_target(mem, MEM_SIZE, FALSE_VAL, target_addr, pos));
}

TEST_F(SM64SyncTest, ValidWriteSucceeds) {
  math::Vector3f pos(100.0f, 200.0f, 300.0f);
  u32 target_addr = 512;
  u32 root_addr = 768;
  setup_valid_target(target_addr, root_addr);

  EXPECT_TRUE(
      sm64::LibSM64Manager::write_mario_pos_to_target(mem, MEM_SIZE, FALSE_VAL, target_addr, pos));

  // Verify the position was written correctly at root_addr + 12
  float result[4];
  std::memcpy(result, mem + root_addr + TRANS_OFFSET, 16);
  EXPECT_FLOAT_EQ(result[0], 100.0f);
  EXPECT_FLOAT_EQ(result[1], 200.0f);
  EXPECT_FLOAT_EQ(result[2], 300.0f);
  EXPECT_FLOAT_EQ(result[3], 1.0f);
}

TEST_F(SM64SyncTest, ValidWriteDoesNotCorruptSurroundingMemory) {
  math::Vector3f pos(42.0f, 43.0f, 44.0f);
  u32 target_addr = 512;
  u32 root_addr = 768;
  setup_valid_target(target_addr, root_addr);

  // Put sentinel values around the trans region
  u8 sentinel = 0xAB;
  mem[root_addr + TRANS_OFFSET - 1] = sentinel;
  mem[root_addr + TRANS_OFFSET + 16] = sentinel;

  EXPECT_TRUE(
      sm64::LibSM64Manager::write_mario_pos_to_target(mem, MEM_SIZE, FALSE_VAL, target_addr, pos));

  // Sentinels should be untouched
  EXPECT_EQ(mem[root_addr + TRANS_OFFSET - 1], sentinel);
  EXPECT_EQ(mem[root_addr + TRANS_OFFSET + 16], sentinel);
}

TEST_F(SM64SyncTest, BoundaryTargetExactlyAtLimit) {
  math::Vector3f pos(1.0f, 2.0f, 3.0f);
  // target_ptr + 112 (108+4) == MEM_SIZE should succeed (check is > not >=)
  u32 target_addr = MEM_SIZE - 112;
  u32 root_addr = 600;
  setup_valid_target(target_addr, root_addr);
  EXPECT_TRUE(
      sm64::LibSM64Manager::write_mario_pos_to_target(mem, MEM_SIZE, FALSE_VAL, target_addr, pos));
}

TEST_F(SM64SyncTest, BoundaryRootExactlyAtLimit) {
  math::Vector3f pos(1.0f, 2.0f, 3.0f);
  u32 target_addr = 512;
  // root_ptr + 28 (12 + 16) == MEM_SIZE should succeed
  u32 root_addr = MEM_SIZE - 28;
  setup_valid_target(target_addr, root_addr);
  EXPECT_TRUE(
      sm64::LibSM64Manager::write_mario_pos_to_target(mem, MEM_SIZE, FALSE_VAL, target_addr, pos));
}
