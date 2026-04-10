/*!
 * @file test_sm64_integration.cpp
 * Tests for the libsm64 integration safety checks.
 *
 * Two suites:
 *  - SM64SyncTest: write_mario_pos_to_target boundary handling.
 *  - SM64ActorSweepTest: the dynamic-actor-collision walker (test_sweep) against
 *    a synthetic EE memory buffer. Exercises happy paths plus every defensive
 *    branch (cycles, broken meshes, NaN transforms, OOB pointers, etc.).
 */

#include <cmath>
#include <cstring>
#include <vector>

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

// ============================================================================
// SM64ActorSweepTest: dynamic actor-collision walker
// ============================================================================
//
// These tests build a synthetic EE memory buffer that mimics the bits of GOAL
// runtime layout that LibSM64Manager::test_sweep() walks: an *active-pool*
// symbol pointing to a process tree of process-drawables, each holding a
// collide-shape root with a collide-shape-prim-{mesh,group} hierarchy that
// references collide-meshes.
//
// All offsets here MUST stay in sync with libsm64_integration.cpp's `namespace
// ac` constants. They are duplicated rather than #included because those
// constants live in an anonymous namespace inside the .cpp.
class SM64ActorSweepTest : public ::testing::Test {
 protected:
  // Must be > MIN_HEAP_ADDR (0x100000 = 1 MB) since valid_basic_ptr() rejects
  // anything below that. Allocate 2 MB and bump-allocate above 0x100000.
  static constexpr u32 MEM_SIZE = 2 * 1024 * 1024;
  static constexpr u32 HEAP_BASE = 0x100000 + 0x200;  // first allocatable byte
  static constexpr u32 FALSE_VAL = 0x40;              // arbitrary "#f" address

  // Distinct, plausible-looking type pointers. type_is_descendant() will hit
  // the cur==needle short-circuit on the first iteration so we don't need to
  // build a parent chain. They MUST satisfy valid_basic_ptr (>= MIN_HEAP_ADDR
  // == 0x100000, and (addr & 7) == 4) because the walker validates the type
  // tag of every popped node before doing the descendant check.
  static constexpr u32 PD_TYPE = 0x100004;     // process-drawable type
  static constexpr u32 CS_TYPE = 0x100014;     // collide-shape type
  static constexpr u32 PM_TYPE = 0x100024;     // collide-shape-prim-mesh type
  static constexpr u32 PG_TYPE = 0x100034;     // collide-shape-prim-group type
  static constexpr u32 OTHER_TYPE = 0x100044;  // some unrelated basic type
  // Mesh basic-ptr type sentinel (the walker doesn't type-check meshes, but
  // alloc_basic stores something at the type tag — make it safe).
  static constexpr u32 MESH_TYPE = 0x100054;

  // Runtime offsets (declared GOAL offset minus 4 for boxed types).
  static constexpr u32 PTREE_BROTHER_OFF = 12;
  static constexpr u32 PTREE_CHILD_OFF = 16;
  static constexpr u32 PDRAW_ROOT_OFF = 108;
  static constexpr u32 PDRAW_NODE_LIST_OFF = 112;  // process-drawable.node-list (basic)
  static constexpr u32 CSHAPE_TRANS_OFF = 12;
  static constexpr u32 CSHAPE_QUAT_OFF = 28;
  static constexpr u32 CSHAPE_ROOT_PRIM_OFF = 156;
  static constexpr u32 PRIM_TRANSFORM_INDEX_OFF = 8;  // collide-shape-prim.transform-index (int8)
  static constexpr u32 PRIM_MESH_MESH_OFF = 68;
  static constexpr u32 PRIM_GROUP_NUM_PRIMS_OFF = 68;
  static constexpr u32 PRIM_GROUP_PRIM_ARRAY_OFF = 76;
  static constexpr u32 MESH_NUM_TRIS_OFF = 4;
  static constexpr u32 MESH_NUM_VERTS_OFF = 8;
  static constexpr u32 MESH_VERTEX_DATA_OFF = 12;
  static constexpr u32 MESH_TRIS_OFF = 28;
  static constexpr u32 MESH_TRI_SIZE = 8;
  // cspace-array (process-drawable.node-list) layout. It's a basic; length
  // sits at runtime offset 0 (declared 4) and the inline cspace data starts
  // at runtime offset 12 (declared 16). Each cspace is 32 bytes; bone is at
  // structure offset 16.
  static constexpr u32 CSPACE_ARRAY_DATA_OFF = 12;
  static constexpr u32 CSPACE_SIZE = 32;
  static constexpr u32 CSPACE_BONE_OFF = 16;
  static constexpr u32 CSPACE_ARRAY_TYPE = 0x100064;  // sentinel basic type

  std::vector<u8> mem;
  u32 cursor = 0;

  // Address of the active-pool symbol cell (= where the symbol's value lives).
  u32 active_pool_sym = 0;

  void SetUp() override {
    mem.assign(MEM_SIZE, 0);
    // Start the bump allocator above MIN_HEAP_ADDR (0x100000) so basics pass
    // valid_basic_ptr's heap-floor check.
    cursor = HEAP_BASE;

    // Allocate a 4-byte cell for the active-pool symbol value. This one is
    // referenced by address (not as a basic) so it doesn't need the heap floor.
    // But keeping it within mem is easiest.
    active_pool_sym = alloc_raw(4, 4);
  }

  // Bump-allocate `size` bytes aligned to `align`. Returns the base address.
  u32 alloc_raw(u32 size, u32 align = 16) {
    cursor = (cursor + align - 1) & ~(align - 1);
    EXPECT_LE(cursor + size, MEM_SIZE) << "test fixture exhausted mock memory";
    u32 r = cursor;
    cursor += size;
    return r;
  }

  // Allocate a "basic" GOAL object: reserves `size` bytes for the body plus
  // 4 bytes prefix that holds the type tag. Returns the basic_ptr (the body
  // address). The body must be aligned so that (basic_ptr & 0x7) == 4 — that's
  // what valid_basic_ptr() in the production code requires. So we align the
  // type-tag address to 8, then basic_ptr = type_addr + 4 satisfies the check.
  u32 alloc_basic(u32 size, u32 type_ptr, u32 align = 16) {
    (void)align;
    u32 type_addr = alloc_raw(size + 4, 8);
    u32 basic_ptr = type_addr + 4;
    write_u32(type_addr, type_ptr);
    return basic_ptr;
  }

  void write_u32(u32 addr, u32 v) {
    ASSERT_LE(addr + 4, MEM_SIZE);
    std::memcpy(mem.data() + addr, &v, 4);
  }
  void write_f32(u32 addr, float v) {
    ASSERT_LE(addr + 4, MEM_SIZE);
    std::memcpy(mem.data() + addr, &v, 4);
  }
  void write_u8(u32 addr, u8 v) {
    ASSERT_LE(addr + 1, MEM_SIZE);
    mem[addr] = v;
  }
  void write_vec4(u32 addr, float x, float y, float z, float w = 1.0f) {
    write_f32(addr + 0, x);
    write_f32(addr + 4, y);
    write_f32(addr + 8, z);
    write_f32(addr + 12, w);
  }

  // ---- High-level builders ------------------------------------------------

  // Make a 1-tri collide-mesh with 3 unit vertices. Returns the mesh ptr.
  // The vertex data lives in a separate allocation so we can also corrupt
  // the vertex_data pointer in tests.
  u32 build_simple_mesh(u32 num_tris = 1, u32 num_verts = 3) {
    // Vertex buffer: num_verts * 16 bytes.
    u32 vbuf = alloc_raw(num_verts * 16, 16);
    for (u32 i = 0; i < num_verts; i++) {
      write_vec4(vbuf + i * 16, (float)i, 0.0f, 0.0f, 1.0f);
    }
    // Mesh body: large enough to hold the inline tri array.
    u32 body_size = MESH_TRIS_OFF + num_tris * MESH_TRI_SIZE + 16;
    u32 mesh_ptr = alloc_basic(body_size, /*type=*/0x200);  // mesh isn't type-checked
    write_u32(mesh_ptr + MESH_NUM_TRIS_OFF, num_tris);
    write_u32(mesh_ptr + MESH_NUM_VERTS_OFF, num_verts);
    write_u32(mesh_ptr + MESH_VERTEX_DATA_OFF, vbuf);
    // Triangles: indices 0,1,2 by default; pat=0
    for (u32 i = 0; i < num_tris; i++) {
      u32 tri = mesh_ptr + MESH_TRIS_OFF + i * MESH_TRI_SIZE;
      write_u8(tri + 0, 0);
      write_u8(tri + 1, 1);
      write_u8(tri + 2, 2);
      write_u8(tri + 3, 0);
      write_u32(tri + 4, 0);
    }
    return mesh_ptr;
  }

  // Wrap a mesh in a collide-shape-prim-mesh basic. Returns the prim ptr.
  // `transform_index` defaults to -2 (= no joint, use root transform). Pass
  // a non-negative value to attach the prim to a specific bone in the
  // process-drawable's node-list (cspace-array).
  u32 build_prim_mesh(u32 mesh_ptr, int8_t transform_index = -2) {
    u32 prim = alloc_basic(PRIM_MESH_MESH_OFF + 16, PM_TYPE);
    write_u32(prim + PRIM_MESH_MESH_OFF, mesh_ptr);
    write_u8(prim + PRIM_TRANSFORM_INDEX_OFF, static_cast<u8>(transform_index));
    return prim;
  }

  // Build a 64-byte 16-byte-aligned bone structure with the given world
  // translation and identity rotation. Returns the bone struct address.
  // The matrix is column-major: col0/col1/col2 = identity basis, col3 = trans.
  u32 build_bone(float tx, float ty, float tz) {
    u32 bone = alloc_raw(64, 16);
    EXPECT_EQ(bone & 0xF, 0u) << "bone must be 16-byte aligned";
    // col0 = (1,0,0,0)
    write_f32(bone + 0, 1.0f); write_f32(bone + 4, 0.0f);
    write_f32(bone + 8, 0.0f); write_f32(bone + 12, 0.0f);
    // col1 = (0,1,0,0)
    write_f32(bone + 16, 0.0f); write_f32(bone + 20, 1.0f);
    write_f32(bone + 24, 0.0f); write_f32(bone + 28, 0.0f);
    // col2 = (0,0,1,0)
    write_f32(bone + 32, 0.0f); write_f32(bone + 36, 0.0f);
    write_f32(bone + 40, 1.0f); write_f32(bone + 44, 0.0f);
    // col3 = (tx,ty,tz,1)
    write_f32(bone + 48, tx); write_f32(bone + 52, ty);
    write_f32(bone + 56, tz); write_f32(bone + 60, 1.0f);
    return bone;
  }

  // Build a cspace-array basic of the given length, with each cspace's bone
  // pointer set to a freshly allocated bone at the supplied translation.
  // bone_translations.size() == array length.
  u32 build_cspace_array(const std::vector<std::array<float, 3>>& bone_translations) {
    u32 len = static_cast<u32>(bone_translations.size());
    u32 body_size = CSPACE_ARRAY_DATA_OFF + len * CSPACE_SIZE + 16;
    u32 arr = alloc_basic(body_size, CSPACE_ARRAY_TYPE);
    write_u32(arr + 0, len);  // length at runtime offset 0
    for (u32 i = 0; i < len; i++) {
      u32 cspace = arr + CSPACE_ARRAY_DATA_OFF + i * CSPACE_SIZE;
      u32 bone =
          build_bone(bone_translations[i][0], bone_translations[i][1], bone_translations[i][2]);
      write_u32(cspace + CSPACE_BONE_OFF, bone);
    }
    return arr;
  }

  // Build a process-drawable with both a collide-shape root and a cspace-array
  // node-list. Returns the pd address.
  u32 build_process_drawable_with_node_list(u32 cs_ptr, u32 cspace_array) {
    u32 size = PDRAW_NODE_LIST_OFF + 16;
    u32 pd = alloc_basic(size, PD_TYPE);
    write_u32(pd + PDRAW_ROOT_OFF, cs_ptr);
    write_u32(pd + PDRAW_NODE_LIST_OFF, cspace_array);
    return pd;
  }

  // Wrap a list of prim ptrs (mesh or group) into a collide-shape-prim-group.
  u32 build_prim_group(const std::vector<u32>& children) {
    u32 size = PRIM_GROUP_PRIM_ARRAY_OFF + (u32)children.size() * 4 + 16;
    u32 grp = alloc_basic(size, PG_TYPE);
    write_u32(grp + PRIM_GROUP_NUM_PRIMS_OFF, (u32)children.size());
    for (size_t i = 0; i < children.size(); i++) {
      write_u32(grp + PRIM_GROUP_PRIM_ARRAY_OFF + (u32)i * 4, children[i]);
    }
    return grp;
  }

  // Build a collide-shape with the given root prim and identity transform.
  u32 build_collide_shape(u32 root_prim, float tx = 0.0f, float ty = 0.0f, float tz = 0.0f) {
    u32 size = CSHAPE_ROOT_PRIM_OFF + 16;
    u32 cs = alloc_basic(size, CS_TYPE);
    write_vec4(cs + CSHAPE_TRANS_OFF, tx, ty, tz, 1.0f);
    write_vec4(cs + CSHAPE_QUAT_OFF, 0.0f, 0.0f, 0.0f, 1.0f);  // identity quat
    write_u32(cs + CSHAPE_ROOT_PRIM_OFF, root_prim);
    return cs;
  }

  // Build a process-drawable basic with the given collide-shape root.
  u32 build_process_drawable(u32 cs_ptr) {
    u32 size = PDRAW_ROOT_OFF + 16;
    u32 pd = alloc_basic(size, PD_TYPE);
    write_u32(pd + PDRAW_ROOT_OFF, cs_ptr);
    return pd;
  }

  // Build a non-PD basic so we can test the walker skips it cleanly.
  u32 build_other_node() {
    u32 size = PDRAW_ROOT_OFF + 16;
    return alloc_basic(size, OTHER_TYPE);
  }

  // Allocate a 4-byte "ppointer slot" that holds a pointer to `target` and
  // return the slot's address. The walker dereferences child/brother fields
  // through one level of indirection (ppointer → actual node).
  u32 make_ppointer(u32 target) {
    u32 slot = alloc_raw(4, 4);
    write_u32(slot, target);
    return slot;
  }

  // Stitch a list of nodes into a process tree by setting child/brother.
  // The first node becomes the active pool's child; siblings are linked
  // through the brother chain. child/brother are PPOINTERS — slots holding
  // the address of the actual basic ptr — so the walker's deref_ppointer
  // path can resolve them.
  void install_children(const std::vector<u32>& siblings) {
    u32 root = alloc_basic(PTREE_CHILD_OFF + 16, OTHER_TYPE);
    write_u32(active_pool_sym, root);
    if (siblings.empty()) {
      write_u32(root + PTREE_CHILD_OFF, 0);
      return;
    }
    write_u32(root + PTREE_CHILD_OFF, make_ppointer(siblings[0]));
    for (size_t i = 0; i + 1 < siblings.size(); i++) {
      write_u32(siblings[i] + PTREE_BROTHER_OFF, make_ppointer(siblings[i + 1]));
    }
  }

  // Convenience: run test_sweep with the standard parameters.
  sm64::LibSM64Manager::TestSweepResult run_sweep(bool dry_run = true) {
    return sm64::LibSM64Manager::instance().test_sweep(
        mem.data(), MEM_SIZE, FALSE_VAL, active_pool_sym, PD_TYPE, CS_TYPE, PM_TYPE, PG_TYPE,
        dry_run);
  }
};

// ---- Defensive bail-outs ----------------------------------------------------

TEST_F(SM64ActorSweepTest, NullEEMemBailsCleanly) {
  auto r = sm64::LibSM64Manager::instance().test_sweep(nullptr, MEM_SIZE, FALSE_VAL,
                                                        active_pool_sym, PD_TYPE, CS_TYPE, PM_TYPE,
                                                        PG_TYPE, true);
  EXPECT_EQ(r.process_tree_nodes_visited, 0);
  EXPECT_EQ(r.meshes_found, 0);
  EXPECT_EQ(r.errors, 0);
}

TEST_F(SM64ActorSweepTest, TooSmallMemBailsCleanly) {
  // mem_size < 1024 should be rejected up-front.
  auto r = sm64::LibSM64Manager::instance().test_sweep(mem.data(), 100, FALSE_VAL, active_pool_sym,
                                                        PD_TYPE, CS_TYPE, PM_TYPE, PG_TYPE, true);
  EXPECT_EQ(r.process_tree_nodes_visited, 0);
}

TEST_F(SM64ActorSweepTest, ZeroFalseValBailsCleanly) {
  auto r = sm64::LibSM64Manager::instance().test_sweep(mem.data(), MEM_SIZE, /*false_val=*/0,
                                                        active_pool_sym, PD_TYPE, CS_TYPE, PM_TYPE,
                                                        PG_TYPE, true);
  EXPECT_EQ(r.process_tree_nodes_visited, 0);
}

TEST_F(SM64ActorSweepTest, MissingTypePointersBailCleanly) {
  // Any of the type pointers being 0 should bail.
  auto r = sm64::LibSM64Manager::instance().test_sweep(mem.data(), MEM_SIZE, FALSE_VAL,
                                                        active_pool_sym, /*pd=*/0, CS_TYPE,
                                                        PM_TYPE, PG_TYPE, true);
  EXPECT_EQ(r.process_tree_nodes_visited, 0);
}

TEST_F(SM64ActorSweepTest, EmptyActivePoolWalksZeroNodes) {
  // Active pool symbol value is 0 (default-zeroed mem).
  auto r = run_sweep();
  EXPECT_EQ(r.process_tree_nodes_visited, 0);
  EXPECT_EQ(r.meshes_found, 0);
  EXPECT_EQ(r.errors, 0);
}

TEST_F(SM64ActorSweepTest, ActivePoolWithNoChildrenWalksZeroNodes) {
  install_children({});
  auto r = run_sweep();
  EXPECT_EQ(r.process_tree_nodes_visited, 0);
  EXPECT_EQ(r.meshes_found, 0);
}

// ---- Happy paths ------------------------------------------------------------

TEST_F(SM64ActorSweepTest, SinglePDWithPrimMeshFindsOneMesh) {
  u32 mesh = build_simple_mesh();
  u32 prim = build_prim_mesh(mesh);
  u32 cs = build_collide_shape(prim);
  u32 pd = build_process_drawable(cs);
  install_children({pd});

  auto r = run_sweep();
  EXPECT_EQ(r.process_tree_nodes_visited, 1);
  EXPECT_EQ(r.process_drawables_seen, 1);
  EXPECT_EQ(r.meshes_found, 1);
  EXPECT_EQ(r.triangles_extracted, 1);
  EXPECT_EQ(r.errors, 0);
}

TEST_F(SM64ActorSweepTest, SinglePDWithPrimGroupFindsAllMeshes) {
  u32 m1 = build_simple_mesh(2);
  u32 m2 = build_simple_mesh(3);
  u32 pm1 = build_prim_mesh(m1);
  u32 pm2 = build_prim_mesh(m2);
  u32 grp = build_prim_group({pm1, pm2});
  u32 cs = build_collide_shape(grp);
  u32 pd = build_process_drawable(cs);
  install_children({pd});

  auto r = run_sweep();
  EXPECT_EQ(r.meshes_found, 2);
  EXPECT_EQ(r.triangles_extracted, 5);
}

TEST_F(SM64ActorSweepTest, NestedPrimGroupsAreFlattened) {
  u32 m1 = build_simple_mesh();
  u32 m2 = build_simple_mesh();
  u32 m3 = build_simple_mesh();
  u32 pm1 = build_prim_mesh(m1);
  u32 pm2 = build_prim_mesh(m2);
  u32 pm3 = build_prim_mesh(m3);
  u32 inner = build_prim_group({pm2, pm3});
  u32 outer = build_prim_group({pm1, inner});
  u32 cs = build_collide_shape(outer);
  u32 pd = build_process_drawable(cs);
  install_children({pd});

  auto r = run_sweep();
  EXPECT_EQ(r.meshes_found, 3);
}

TEST_F(SM64ActorSweepTest, MultiplePDsAcrossSiblingsAndChildren) {
  // pd1 -> brother -> pd2; pd2 -> child -> pd3
  u32 m1 = build_simple_mesh();
  u32 m2 = build_simple_mesh();
  u32 m3 = build_simple_mesh();
  u32 pd1 = build_process_drawable(build_collide_shape(build_prim_mesh(m1)));
  u32 pd2 = build_process_drawable(build_collide_shape(build_prim_mesh(m2)));
  u32 pd3 = build_process_drawable(build_collide_shape(build_prim_mesh(m3)));
  install_children({pd1, pd2});
  // Hand-link pd3 as child of pd2 (via a ppointer slot).
  write_u32(pd2 + PTREE_CHILD_OFF, make_ppointer(pd3));

  auto r = run_sweep();
  EXPECT_EQ(r.process_drawables_seen, 3);
  EXPECT_EQ(r.meshes_found, 3);
}

// ---- Skipping unrelated nodes ----------------------------------------------

TEST_F(SM64ActorSweepTest, NonPDNodeIsSkippedButTraversed) {
  // A "process" node that is NOT a process-drawable, with a PD child.
  u32 mesh = build_simple_mesh();
  u32 pd = build_process_drawable(build_collide_shape(build_prim_mesh(mesh)));
  u32 other = build_other_node();
  write_u32(other + PTREE_CHILD_OFF, make_ppointer(pd));
  install_children({other});

  auto r = run_sweep();
  EXPECT_EQ(r.process_tree_nodes_visited, 2);
  EXPECT_EQ(r.process_drawables_seen, 1);
  EXPECT_EQ(r.meshes_found, 1);
}

TEST_F(SM64ActorSweepTest, PDWithNonCollideShapeRootIsSkipped) {
  u32 fake_root = alloc_basic(200, OTHER_TYPE);
  u32 pd = build_process_drawable(fake_root);
  install_children({pd});

  auto r = run_sweep();
  EXPECT_EQ(r.process_drawables_seen, 1);
  EXPECT_EQ(r.meshes_found, 0);
  EXPECT_EQ(r.errors, 0);
}

TEST_F(SM64ActorSweepTest, PDWithNullRootIsSkipped) {
  u32 pd = build_process_drawable(0);
  install_children({pd});
  auto r = run_sweep();
  EXPECT_EQ(r.process_drawables_seen, 1);
  EXPECT_EQ(r.meshes_found, 0);
}

TEST_F(SM64ActorSweepTest, PDWithFalseRootIsSkipped) {
  u32 pd = build_process_drawable(FALSE_VAL);
  install_children({pd});
  auto r = run_sweep();
  EXPECT_EQ(r.process_drawables_seen, 1);
  EXPECT_EQ(r.meshes_found, 0);
}

// ---- Cycle and bounds safety -----------------------------------------------

TEST_F(SM64ActorSweepTest, BrotherSelfLoopTerminates) {
  u32 mesh = build_simple_mesh();
  u32 pd = build_process_drawable(build_collide_shape(build_prim_mesh(mesh)));
  install_children({pd});
  // Now point pd's brother back at itself (via a ppointer slot).
  write_u32(pd + PTREE_BROTHER_OFF, make_ppointer(pd));

  auto r = run_sweep();
  // Visited once (cycle guard prevents re-walk).
  EXPECT_EQ(r.process_tree_nodes_visited, 1);
  EXPECT_EQ(r.meshes_found, 1);
}

TEST_F(SM64ActorSweepTest, ChildSelfLoopTerminates) {
  u32 mesh = build_simple_mesh();
  u32 pd = build_process_drawable(build_collide_shape(build_prim_mesh(mesh)));
  install_children({pd});
  write_u32(pd + PTREE_CHILD_OFF, make_ppointer(pd));

  auto r = run_sweep();
  EXPECT_EQ(r.process_tree_nodes_visited, 1);
}

TEST_F(SM64ActorSweepTest, MutualBrotherCycleTerminates) {
  u32 m1 = build_simple_mesh();
  u32 m2 = build_simple_mesh();
  u32 pd1 = build_process_drawable(build_collide_shape(build_prim_mesh(m1)));
  u32 pd2 = build_process_drawable(build_collide_shape(build_prim_mesh(m2)));
  install_children({pd1, pd2});
  // Make pd2's brother point back at pd1 (already-visited), via ppointer.
  write_u32(pd2 + PTREE_BROTHER_OFF, make_ppointer(pd1));

  auto r = run_sweep();
  EXPECT_EQ(r.process_tree_nodes_visited, 2);
  EXPECT_EQ(r.meshes_found, 2);
}

TEST_F(SM64ActorSweepTest, ChildPointerOutOfBoundsIsRejected) {
  u32 mesh = build_simple_mesh();
  u32 pd = build_process_drawable(build_collide_shape(build_prim_mesh(mesh)));
  install_children({pd});
  // Plant a wildly OOB brother pointer.
  write_u32(pd + PTREE_BROTHER_OFF, MEM_SIZE + 0x1000);

  auto r = run_sweep();
  // pd is still walked. The bad brother is silently ignored.
  EXPECT_EQ(r.process_tree_nodes_visited, 1);
  EXPECT_EQ(r.meshes_found, 1);
}

TEST_F(SM64ActorSweepTest, TinyChildPointerIsRejected) {
  u32 mesh = build_simple_mesh();
  u32 pd = build_process_drawable(build_collide_shape(build_prim_mesh(mesh)));
  install_children({pd});
  // A pointer < 16 should be rejected by valid_ee_addr without read.
  // It is non-zero and not the false value, so it survives the early skip.
  write_u32(pd + PTREE_BROTHER_OFF, 8);

  auto r = run_sweep();
  EXPECT_EQ(r.meshes_found, 1);
  EXPECT_EQ(r.errors, 0);  // bad pointer is not counted as a hard error
}

// ---- Mesh corruption / blacklisting ----------------------------------------

TEST_F(SM64ActorSweepTest, MeshWithBadVertexDataIsBlacklisted) {
  u32 mesh = build_simple_mesh();
  // Corrupt the vertex_data pointer to OOB.
  write_u32(mesh + MESH_VERTEX_DATA_OFF, MEM_SIZE + 0x1000);
  u32 pd = build_process_drawable(build_collide_shape(build_prim_mesh(mesh)));
  install_children({pd});

  auto r = run_sweep();
  EXPECT_EQ(r.meshes_found, 0);
  EXPECT_EQ(r.errors, 1);
}

TEST_F(SM64ActorSweepTest, MeshWithZeroTrisIsRejected) {
  u32 mesh = build_simple_mesh(/*num_tris=*/0);
  u32 pd = build_process_drawable(build_collide_shape(build_prim_mesh(mesh)));
  install_children({pd});

  auto r = run_sweep();
  EXPECT_EQ(r.meshes_found, 0);
  // num_tris==0 returns false from extract_mesh_world → counted as error.
  EXPECT_EQ(r.errors, 1);
}

TEST_F(SM64ActorSweepTest, MeshWithTooManyTrisIsRejected) {
  u32 mesh = build_simple_mesh();
  // Spike num_tris way above MAX_TRIS_PER_MESH (512).
  write_u32(mesh + MESH_NUM_TRIS_OFF, 100000);
  u32 pd = build_process_drawable(build_collide_shape(build_prim_mesh(mesh)));
  install_children({pd});

  auto r = run_sweep();
  EXPECT_EQ(r.meshes_found, 0);
  EXPECT_EQ(r.errors, 1);
}

TEST_F(SM64ActorSweepTest, MeshWithTooManyVertsIsRejected) {
  u32 mesh = build_simple_mesh();
  write_u32(mesh + MESH_NUM_VERTS_OFF, 100000);
  u32 pd = build_process_drawable(build_collide_shape(build_prim_mesh(mesh)));
  install_children({pd});

  auto r = run_sweep();
  EXPECT_EQ(r.meshes_found, 0);
  EXPECT_EQ(r.errors, 1);
}

// ---- Garbage transforms -----------------------------------------------------

TEST_F(SM64ActorSweepTest, NaNTranslationSkipsActor) {
  u32 mesh = build_simple_mesh();
  u32 cs = build_collide_shape(build_prim_mesh(mesh));
  // Overwrite trans with NaN.
  float nan_v = std::nanf("");
  write_vec4(cs + CSHAPE_TRANS_OFF, nan_v, nan_v, nan_v, 1.0f);
  u32 pd = build_process_drawable(cs);
  install_children({pd});

  auto r = run_sweep();
  EXPECT_EQ(r.process_drawables_seen, 1);
  // The actor is skipped before mesh extraction.
  EXPECT_EQ(r.meshes_found, 0);
  EXPECT_EQ(r.errors, 0);
}

TEST_F(SM64ActorSweepTest, GarbageQuaternionFallsBackToIdentity) {
  u32 mesh = build_simple_mesh();
  u32 cs = build_collide_shape(build_prim_mesh(mesh));
  // Non-unit quaternion (way out of [0.5, 1.5] range): falls back to identity,
  // mesh extraction still succeeds.
  write_vec4(cs + CSHAPE_QUAT_OFF, 100.0f, 100.0f, 100.0f, 100.0f);
  u32 pd = build_process_drawable(cs);
  install_children({pd});

  auto r = run_sweep();
  EXPECT_EQ(r.meshes_found, 1);
}

// ---- Mass / stress ----------------------------------------------------------

TEST_F(SM64ActorSweepTest, ManyActorsAllProcessed) {
  std::vector<u32> pds;
  for (int i = 0; i < 16; i++) {
    u32 m = build_simple_mesh(2);
    pds.push_back(build_process_drawable(build_collide_shape(build_prim_mesh(m))));
  }
  install_children(pds);

  auto r = run_sweep();
  EXPECT_EQ(r.process_drawables_seen, 16);
  EXPECT_EQ(r.meshes_found, 16);
  EXPECT_EQ(r.triangles_extracted, 32);
}

// ---- Bone-attached prims (ropebridge scenario) -----------------------------

// A ropebridge-style actor: one process-drawable, one cspace-array (the
// node-list) with several bones at known world translations, and a
// collide-shape whose root prim is a prim-group of N prim-meshes, each
// pointing at a distinct bone via transform-index. The sweep should pull
// each prim's world position from the corresponding bone matrix instead of
// falling back to the actor root, and report N successful bone lookups.
TEST_F(SM64ActorSweepTest, RopebridgeBoneAttachedPrimsUseBoneTransforms) {
  // 5 bones at distinct positions along the X axis, at Y=200, Z=300.
  std::vector<std::array<float, 3>> bone_trans = {
      {{0.0f, 200.0f, 300.0f}},
      {{1000.0f, 200.0f, 300.0f}},
      {{2000.0f, 200.0f, 300.0f}},
      {{3000.0f, 200.0f, 300.0f}},
      {{4000.0f, 200.0f, 300.0f}},
  };
  u32 cspace_arr = build_cspace_array(bone_trans);

  // 3 prim-meshes attached to bones 1, 2, 3 (skipping bone 0 = root).
  u32 m1 = build_simple_mesh();
  u32 m2 = build_simple_mesh();
  u32 m3 = build_simple_mesh();
  u32 pm1 = build_prim_mesh(m1, /*transform_index=*/1);
  u32 pm2 = build_prim_mesh(m2, /*transform_index=*/2);
  u32 pm3 = build_prim_mesh(m3, /*transform_index=*/3);
  u32 grp = build_prim_group({pm1, pm2, pm3});

  // Actor root sits far from any bone so we'd notice if we accidentally
  // fell back to it.
  u32 cs = build_collide_shape(grp, /*tx=*/-99999.0f, /*ty=*/-99999.0f, /*tz=*/-99999.0f);
  u32 pd = build_process_drawable_with_node_list(cs, cspace_arr);
  install_children({pd});

  auto r = run_sweep();
  EXPECT_EQ(r.process_drawables_seen, 1);
  EXPECT_EQ(r.meshes_found, 3);
  EXPECT_EQ(r.errors, 0);

  EXPECT_EQ(r.bone_lookups_attempted, 3);
  EXPECT_EQ(r.bone_lookups_succeeded, 3);
  EXPECT_EQ(r.bone_lookups_fell_back, 0);

  // Each captured prim should have its bone's world translation, NOT the
  // actor root translation (-99999, -99999, -99999).
  ASSERT_EQ(r.captured_prims.size(), 3u);
  // Order matches prim-group iteration: pm1 → bone 1, pm2 → bone 2, pm3 → bone 3.
  for (size_t i = 0; i < 3; i++) {
    int bone_idx = (int)i + 1;
    EXPECT_TRUE(r.captured_prims[i].used_bone)
        << "prim " << i << " should have used bone path";
    EXPECT_EQ(r.captured_prims[i].transform_index, bone_idx);
    EXPECT_FLOAT_EQ(r.captured_prims[i].pos[0], bone_trans[bone_idx][0]);
    EXPECT_FLOAT_EQ(r.captured_prims[i].pos[1], bone_trans[bone_idx][1]);
    EXPECT_FLOAT_EQ(r.captured_prims[i].pos[2], bone_trans[bone_idx][2]);
  }
}

// If the bone pointer is null (skeleton not initialized) the walker must
// fall back to the actor root, NOT crash, NOT spuriously claim success.
TEST_F(SM64ActorSweepTest, BoneAttachedPrimWithNullBoneFallsBackToRoot) {
  // cspace-array length 3, but bone[1] is null.
  u32 len = 3;
  u32 body_size = CSPACE_ARRAY_DATA_OFF + len * CSPACE_SIZE + 16;
  u32 arr = alloc_basic(body_size, CSPACE_ARRAY_TYPE);
  write_u32(arr + 0, len);
  // bone[0] and bone[2] valid, bone[1] = 0
  u32 b0 = build_bone(10.0f, 20.0f, 30.0f);
  u32 b2 = build_bone(50.0f, 60.0f, 70.0f);
  write_u32(arr + CSPACE_ARRAY_DATA_OFF + 0 * CSPACE_SIZE + CSPACE_BONE_OFF, b0);
  write_u32(arr + CSPACE_ARRAY_DATA_OFF + 1 * CSPACE_SIZE + CSPACE_BONE_OFF, 0);
  write_u32(arr + CSPACE_ARRAY_DATA_OFF + 2 * CSPACE_SIZE + CSPACE_BONE_OFF, b2);

  u32 mesh = build_simple_mesh();
  u32 prim = build_prim_mesh(mesh, /*transform_index=*/1);
  u32 cs = build_collide_shape(prim, 7.0f, 8.0f, 9.0f);
  u32 pd = build_process_drawable_with_node_list(cs, arr);
  install_children({pd});

  auto r = run_sweep();
  EXPECT_EQ(r.bone_lookups_attempted, 1);
  EXPECT_EQ(r.bone_lookups_succeeded, 0);
  EXPECT_EQ(r.bone_lookups_fell_back, 1);
  ASSERT_EQ(r.captured_prims.size(), 1u);
  EXPECT_FALSE(r.captured_prims[0].used_bone);
  EXPECT_FLOAT_EQ(r.captured_prims[0].pos[0], 7.0f);
  EXPECT_FLOAT_EQ(r.captured_prims[0].pos[1], 8.0f);
  EXPECT_FLOAT_EQ(r.captured_prims[0].pos[2], 9.0f);
}

// transform_index past the end of the cspace-array must fall back, never
// read OOB.
TEST_F(SM64ActorSweepTest, BoneAttachedPrimWithOOBIndexFallsBackToRoot) {
  std::vector<std::array<float, 3>> bones = {
      {{1.0f, 2.0f, 3.0f}},
      {{4.0f, 5.0f, 6.0f}},
  };
  u32 arr = build_cspace_array(bones);

  u32 mesh = build_simple_mesh();
  u32 prim = build_prim_mesh(mesh, /*transform_index=*/5);  // > length-1
  u32 cs = build_collide_shape(prim, 100.0f, 200.0f, 300.0f);
  u32 pd = build_process_drawable_with_node_list(cs, arr);
  install_children({pd});

  auto r = run_sweep();
  EXPECT_EQ(r.bone_lookups_fell_back, 1);
  ASSERT_EQ(r.captured_prims.size(), 1u);
  EXPECT_FALSE(r.captured_prims[0].used_bone);
  EXPECT_FLOAT_EQ(r.captured_prims[0].pos[0], 100.0f);
}

TEST_F(SM64ActorSweepTest, RepeatedSweepsAreIdempotent) {
  u32 mesh = build_simple_mesh();
  u32 pd = build_process_drawable(build_collide_shape(build_prim_mesh(mesh)));
  install_children({pd});

  // Each call gets a fresh tracked map (test_sweep is stateless wrt the
  // singleton's state), so results should be identical across runs.
  for (int i = 0; i < 5; i++) {
    auto r = run_sweep();
    EXPECT_EQ(r.meshes_found, 1);
    EXPECT_EQ(r.errors, 0);
  }
}
