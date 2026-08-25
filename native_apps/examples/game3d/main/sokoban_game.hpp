// sokoban_game.hpp -- grid state and Sokoban push-box rules for the clone
// in main.cpp. Pure 2D grid logic: no Jet/Renderer types, so it has no
// dependency on the ELF C++ ABI constraints beyond what main.cpp already
// documents (everything here lives in locals or lambda captures, never a
// namespace-scope instance).
#pragma once

#include <cstdint>
#include <cstring>

#include "sokoban_levels.hpp"

namespace {

enum CellType : uint8_t { CELL_VOID, CELL_WALL, CELL_FLOOR };

// Plain aggregate, always a local (or lambda-captured local) -- never a
// namespace-scope instance -- so its implicit default constructor never
// needs .init_array support. See main.cpp's __dso_handle comment for why
// that distinction matters on this loader.
struct GameState {
    int rows = 0;
    int cols = 0;
    CellType cell[kMaxRows][kMaxCols] = {};
    bool goal[kMaxRows][kMaxCols] = {};
    int8_t playerR = 0, playerC = 0;
    int8_t boxR[kMaxBoxes] = {}, boxC[kMaxBoxes] = {};
    int boxCount = 0;
    int moves = 0;
    bool solved = false;
};

struct MoveResult {
    bool moved = false;
    bool pushedBox = false;
    int boxIndex = -1;
};

inline void loadLevel(int idx, GameState &gs) {
    gs = GameState();
    const LevelDef &lvl = kLevels[idx];
    gs.rows = lvl.rowCount;
    gs.cols = (int)std::strlen(lvl.rows[0]);

    for (int r = 0; r < gs.rows; r++) {
        const char *row = lvl.rows[r];
        for (int c = 0; c < gs.cols; c++) {
            char ch = row[c];
            gs.cell[r][c] = CELL_VOID;
            gs.goal[r][c] = false;
            switch (ch) {
                case '#':
                    gs.cell[r][c] = CELL_WALL;
                    break;
                case ' ':
                    gs.cell[r][c] = CELL_FLOOR;
                    break;
                case '.':
                    gs.cell[r][c] = CELL_FLOOR;
                    gs.goal[r][c] = true;
                    break;
                case '$':
                    gs.cell[r][c] = CELL_FLOOR;
                    gs.boxR[gs.boxCount] = (int8_t)r;
                    gs.boxC[gs.boxCount] = (int8_t)c;
                    gs.boxCount++;
                    break;
                case '*':
                    gs.cell[r][c] = CELL_FLOOR;
                    gs.goal[r][c] = true;
                    gs.boxR[gs.boxCount] = (int8_t)r;
                    gs.boxC[gs.boxCount] = (int8_t)c;
                    gs.boxCount++;
                    break;
                case '@':
                    gs.cell[r][c] = CELL_FLOOR;
                    gs.playerR = (int8_t)r;
                    gs.playerC = (int8_t)c;
                    break;
                case '+':
                    gs.cell[r][c] = CELL_FLOOR;
                    gs.goal[r][c] = true;
                    gs.playerR = (int8_t)r;
                    gs.playerC = (int8_t)c;
                    break;
                default: // '~' and anything else: void
                    break;
            }
        }
    }
}

inline bool cellIsWalkable(const GameState &gs, int r, int c) {
    return r >= 0 && r < gs.rows && c >= 0 && c < gs.cols && gs.cell[r][c] == CELL_FLOOR;
}

inline int findBoxAt(const GameState &gs, int r, int c) {
    for (int i = 0; i < gs.boxCount; i++) {
        if (gs.boxR[i] == r && gs.boxC[i] == c) return i;
    }
    return -1;
}

inline bool allBoxesOnGoal(const GameState &gs) {
    for (int i = 0; i < gs.boxCount; i++) {
        if (!gs.goal[gs.boxR[i]][gs.boxC[i]]) return false;
    }
    return true;
}

// Attempts to move the player one cell in (dr, dc), pushing a box ahead of
// it if present and able to move. Leaves `gs` untouched on a blocked move.
inline MoveResult tryMove(GameState &gs, int dr, int dc) {
    MoveResult res;
    int nr = gs.playerR + dr;
    int nc = gs.playerC + dc;
    if (!cellIsWalkable(gs, nr, nc)) return res;

    int bi = findBoxAt(gs, nr, nc);
    if (bi >= 0) {
        int br = nr + dr;
        int bc = nc + dc;
        if (!cellIsWalkable(gs, br, bc) || findBoxAt(gs, br, bc) >= 0) return res;
        gs.boxR[bi] = (int8_t)br;
        gs.boxC[bi] = (int8_t)bc;
        res.pushedBox = true;
        res.boxIndex = bi;
    }

    gs.playerR = (int8_t)nr;
    gs.playerC = (int8_t)nc;
    gs.moves++;
    res.moved = true;
    if (allBoxesOnGoal(gs)) gs.solved = true;
    return res;
}

} // namespace
