// sokoban_levels.hpp -- static level data for the Sokoban clone in main.cpp.
//
// Standard Sokoban notation, plus '~' for cells outside the room shape
// (never drawn, never walkable) so a non-rectangular room doesn't get
// confused with plain floor the way a bare space would:
//   '#' wall     ' ' floor     '.' goal
//   '$' box      '*' box on goal
//   '@' player   '+' player on goal
//   '~' void (outside the room)
//
// Every level below has been verified solvable by a BFS over (player, box
// set) state space, not just eyeballed -- see the push sequence noted in
// each comment for the intended solution.
#pragma once

namespace {

struct LevelDef {
    const char *const *rows;
    int rowCount;
};

// Tutorial: push the box straight down onto the goal (2x DOWN). A
// wall-less "floating island" -- the level boundary is just where the
// floor stops, rounded off at the corners; sokoban_render.hpp gives the
// exposed rim its own side faces (isIslandEdgeCell, sokoban_game.hpp) so
// it reads as a solid slab rather than a paper-thin tile. Kept small (7x7)
// on purpose: a bigger grid means more tiles fighting for the same screen
// area, which makes the player cube read smaller, not bigger.
const char *const kLevel1[] = {
    "~     ~",
    "       ",
    "   @   ",
    "   $   ",
    "       ",
    "   .   ",
    "~     ~",
};

// Two boxes, each needs the player to walk around it to push from the
// north side, then 4x DOWN per box.
const char *const kLevel2[] = {
    "#########",
    "#       #",
    "#  $ $  #",
    "#       #",
    "#  @    #",
    "#       #",
    "#  . .  #",
    "#       #",
    "#########",
};

// Same idea, with an interior pillar (two wall cells) to route around.
const char *const kLevel3[] = {
    "#########",
    "#       #",
    "#  $    #",
    "#   #   #",
    "#  @  $ #",
    "#   #   #",
    "#  .  . #",
    "#       #",
    "#########",
};

const LevelDef kLevels[] = {
    {kLevel1, 7},
    {kLevel2, 9},
    {kLevel3, 9},
};
constexpr int kLevelCount = sizeof(kLevels) / sizeof(kLevels[0]);

// Bounds for GameState's fixed-size grid/box arrays (sokoban_game.hpp);
// kept alongside the level data since they must cover it.
constexpr int kMaxRows = 16;
constexpr int kMaxCols = 16;
constexpr int kMaxBoxes = 4;

} // namespace
