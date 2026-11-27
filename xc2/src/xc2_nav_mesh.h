// SPDX-License-Identifier: GPL-3.0-or-later
// Asynchronous host-side XC2 NavMesh loader (.winvhe / .winvda from the player's own bf2.arh / bf2.ard)
// and A* walkable pathfinder. Runs entirely on a background worker thread so the render thread never stalls.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace xc2::nav {

struct NavVec3 {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

struct PathResult {
    std::string map_res;
    std::vector<NavVec3> points;  // 3D world-space walkable path from player -> primary objective
    float path_len = 0.f;         // total walkable length in meters
    bool loaded = false;          // true once the map's NavMesh is ready
    bool reachable = false;       // true if A* found a connected walkable path
    size_t poly_count = 0;
};

struct LvbLandmark {
    NavVec3 pos;
    uint32_t hash_id = 0;
    std::string name;  // e.g. "landmark_ma17a_001"
};

// Initialize / shut down the background worker (safe to call multiple times).
void mesh_init(const std::string& romfs_dir);
void mesh_shutdown();

// Non-blocking: submits the latest (map_res, player_pos, goal_pos) to the background worker if moved enough,
// and copies the latest completed PathResult snapshot for the render thread.
void mesh_update_and_get(const std::string& map_res, NavVec3 player, NavVec3 goal, PathResult& out);

// Cached lightweight lookup of /gmk/<map_res>_landmark.lvb from bf2.arh/bf2.ard (for cross-map nearest skip-travel hints).
bool mesh_get_map_landmarks(const std::string& map_res, std::vector<LvbLandmark>& out);

}  // namespace xc2::nav
