#include <stdio.h>
#include <stdint.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <math.h>

#ifndef _WIN32
#include <unistd.h>
#endif

// ideas:
//
//   - TODO: figure out why things only work for bigger puzzles if
//           moves which are forced have zero cost.
// 
//   - TODO: replace options with command-line-switches
//
//   - TODO: detect bad thin regions of freespace that could never be filled
//
//   - TODO: bidirectional search?
//

//////////////////////////////////////////////////////////////////////

// Positions are 8-bit integers with 4 bits each for y, x.
enum {

  // Number to represent "not found"
  INVALID_POS = 0xff,
  
  // Maximum # of colors in a puzzle
  MAX_COLORS = 15,
  
  // Maximum valid size of a puzzle
  MAX_SIZE = 15,
  
  // Maximum # cells in a valid puzzle -- since we just use bit
  // shifting to do x/y, need to allocate space for 1 unused column.
  MAX_CELLS = (MAX_SIZE+1)*MAX_SIZE,

  // One million(ish) bytes
  MEGABYTE = 1024*1024,
  
};

// Various cell types, all but freespace have a color
enum {
  TYPE_FREE = 0, // Free space
  TYPE_PATH = 1, // Path between init & goal
  TYPE_INIT = 2, // Starting point
  TYPE_GOAL = 3  // Goal position
};

// Enumerate cardinal directions so we can loop over them
// RIGHT is increasing x, DOWN is increasing y.
enum {
  DIR_LEFT  = 0,
  DIR_RIGHT = 1,
  DIR_UP    = 2,
  DIR_DOWN  = 3
};

// Search termination results
enum {
  SEARCH_ACTIVE = 0,
  SEARCH_SUCCESS = 1,
  SEARCH_UNREACHABLE = 2,
  SEARCH_FULL = 3
};

// Represent the contents of a cell on the game board
typedef uint8_t cell_t;

// Represent a position within the game board
typedef uint8_t pos_t;

// Match color characters to ANSI color codes
typedef struct color_lookup_struct {
  char input_char;   // Color character
  char display_char; // Punctuation a la nethack
  int  ansi_code;    // ANSI color code
} color_lookup;

// Options for this program
typedef struct options_struct {
  int animate_solution;
  int color_display;
  int prevent_self_touching;
  int most_constrained_color;
  int check_freespace_regions;
  int search_astar_like;
  double max_storage_mb;
} options_t;

// Static information about a puzzle layout -- anything that does not
// change as the puzzle is solved is stored here.
typedef struct game_info_struct {

  // Need to quickly look these goal pos for each color
  // to check whether finished.
  pos_t  goal_pos[MAX_COLORS];

  // Index in color_dict table of codes
  int    color_ids[MAX_COLORS];

  // Length/width of game board
  size_t size;

  // Number of colors present
  size_t num_colors;
  
} game_info_t;

// Incremental game state structure for solving -- this is what gets
// written as the search progresses, one state per search node
typedef struct game_state_struct {

  // State of each cell in the world; a little wasteful to duplicate,
  // since only one changes on each move, but necessary for BFS or A*
  // (would not be needed for depth-first search).
  cell_t   cells[MAX_CELLS];

  // Current position of each color
  pos_t    cur_pos[MAX_COLORS];

  // How many free cells?
  uint8_t  num_free;

  // Bitflag indicating whether each color has been completed or not
  // (cur_pos is adjacent to goal_pos).
  uint16_t completed;
  
} game_state_t;

// Disjoint-set data structure for connected component analysis of free
// space (see region_ functions).
typedef struct region_struct {
  // Parent index (or INVALID_POS) if no non-free space
  pos_t parent;
  // Rank (see wikipedia article).
  uint8_t rank;
} region_t;

// Search node for A* / BFS.
typedef struct tree_node_struct {
  game_state_t state;              // Current game state
  double cost_to_come;             // Cost to come (ignored for BFS)
  double cost_to_go;               // Heuristic cost (ignored for BFS)
  struct tree_node_struct* parent; // Parent of this node (may be NULL)
} tree_node_t;

// Strategy is to pre-allocate a big block of nodes in advance, and
// hand them out in order.
typedef struct node_storage_struct {
  tree_node_t* start; // Allocated block
  size_t capacity;    // How many nodes did we allocate?
  size_t count;       // How many did we give out?
} node_storage_t;

// Data structure for heap based priority queue
typedef struct heapq_struct {
  tree_node_t** start; // Array of node pointers
  size_t capacity;     // Maximum allowable queue size
  size_t count;        // Number enqueued
} heapq_t;

// First in, first-out queue implemented as an array of pointers.
typedef struct fifo_struct {
  tree_node_t** start; // Array of node pointers
  size_t capacity;     // Maximum number of things to enqueue ever
  size_t count;        // Total enqueued (next one will go into start[count])
  size_t next;         // Next index to dequeue
} fifo_t;

// Union struct for passing around queues.
typedef union queue_union {
  heapq_t heapq;
  fifo_t  fifo;
} queue_t;

// Function pointers for either type of queue
queue_t (*queue_create)(size_t) = 0;
void (*queue_enqueue)(queue_t*, tree_node_t*) = 0;
tree_node_t* (*queue_deque)(queue_t*) = 0;
void (*queue_destroy)(queue_t*) = 0;
int (*queue_empty)(const queue_t*) = 0;
const tree_node_t* (*queue_peek)(const queue_t*) = 0;

//////////////////////////////////////////////////////////////////////

const char* BLOCK_CHAR = "#";

// For visualizing cardinal directions ordered by the enum above.
const char DIR_CHARS[4] = "<>^v";

// x, y coordinates for each direction
const int DIR_DELTA[4][2] = {
  { -1, 0 },
  {  1, 0 },
  {  0, -1 },
  {  0, 1 }
};

// Look-up table mapping characters in puzzle definitions to ANSI
// colors.
const color_lookup color_dict[MAX_COLORS] = {
  { 'R', 'o', 101 }, // red
  { 'B', '+', 104 }, // blue
  { 'Y', '&', 103 }, // yellow
  { 'G', '*',  42 }, // green
  { 'O', 'x',  43 }, // orange
  { 'C', '%', 106 }, // cyan
  { 'M', '?', 105 }, // magenta
  { 'm', 'v',  41 }, // maroon
  { 'P', '^',  45 }, // purple
  { 'A', '=', 100 }, // gray
  { 'W', '~', 107 }, // white
  { 'g', '.', 102 }, // bright green
  { 'w', '-', 47 }, // beige
  { 'b', '"', 44 }, // dark blue
  { 'c', ',', 46 }, // dark cyan
};

// Global options struct gets setup during main
options_t g_options;

//////////////////////////////////////////////////////////////////////
// Peform lookup in color_dict above

int get_color_id(char c) {
  for (int i=0; i<MAX_COLORS; ++i) {
    if (color_dict[i].input_char == c) {
      return i;
    }
  }
  return -1;
}

//////////////////////////////////////////////////////////////////////
// Detect whether terminal supports color & cursor commands

int terminal_has_color() {

#ifdef _WIN32

  return 0;

#else
  
  if (!isatty(STDOUT_FILENO)) {
    return 0;
  } 

  char* term = getenv("TERM");
  if (!term) { return 0; }
  
  return strstr(term, "xterm") == term || strstr(term, "rxvt") == term;

#endif
  
}

//////////////////////////////////////////////////////////////////////
// Emit color string for index into color_dict table above

const char* set_color_str(int id) {
  if (g_options.color_display) {
    static char buf[256];
    snprintf(buf, 256, "\033[30;%dm", color_dict[id].ansi_code);
    return buf; 
  } else {
    return "";
  }
}

//////////////////////////////////////////////////////////////////////
// Reset terminal color to default

const char* reset_color_str() {
  if (g_options.color_display) {
    return "\033[0m";
  } else {
    return "";
  }
}

//////////////////////////////////////////////////////////////////////
// Clear screen and set cursor pos to 0,0

const char* unprint_board(const game_info_t* info) {
  if (g_options.color_display) {
    static char buf[256];
    snprintf(buf, 256, "\033[%zuA\033[%zuD",
             info->size+2, info->size+2);
    return buf;
  } else {
    return "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n";
  }
}

//////////////////////////////////////////////////////////////////////
// Create a delay

void delay_seconds(double s) {
#ifdef _WIN32
  // TODO: find win32 equivalent of usleep?
#else
  usleep((size_t)(s * 1e6));
#endif
}

//////////////////////////////////////////////////////////////////////
// Create a 8-bit position from 2 4-bit x,y coordinates

pos_t pos_from_coords(pos_t x, pos_t y) {
  return ((y & 0xf) << 4) | (x & 0xf);
}

//////////////////////////////////////////////////////////////////////
// Split 8-bit position into 4-bit x & y coords

void pos_get_coords(pos_t p, int* x, int* y) {
  *x = p & 0xf;
  *y = (p >> 4) & 0xf;
}

//////////////////////////////////////////////////////////////////////
// Are the two positions a single cardinal direction move apart?

int pos_is_adjacent(pos_t a, pos_t b) {
  int diff = a - b;
  diff = diff < 0 ? -diff : diff;
  return diff == 1 || diff == 16;
}

//////////////////////////////////////////////////////////////////////
// Get in-bounds neighbors of given x, y position

int get_neighbors(const game_info_t* info,
                  int x, int y, pos_t neighbors[4]) {

  int num_neighbors = 0;

  for (int neighbor_dir=0; neighbor_dir<4; ++neighbor_dir) {

    // Get x, y
    int neighbor_x = x + DIR_DELTA[neighbor_dir][0];
    int neighbor_y = y + DIR_DELTA[neighbor_dir][1];

    if (neighbor_x >= 0 && neighbor_x < (int)info->size &&
        neighbor_y >= 0 && neighbor_y < (int)info->size) {

      neighbors[num_neighbors++] = pos_from_coords(neighbor_x, neighbor_y);

    }

  }

  return num_neighbors;

}

//////////////////////////////////////////////////////////////////////
// Get in-bounds neighbors of given 8-bit position

int pos_get_neighbors(const game_info_t* info,
                      pos_t p, pos_t neighbors[4]) {

  // Split pos into x & y
  int x, y;
  pos_get_coords(p, &x, &y);

  return get_neighbors(info, x, y, neighbors);

}


//////////////////////////////////////////////////////////////////////
// Create a cell from a 2-bit type, a 4-bit color, and a 2-bit
// direction.

cell_t cell_create(uint8_t type, uint8_t color, uint8_t dir) {
  return ((color & 0xf) << 4) | ((dir & 0x3) << 2) | (type & 0x3);
}

//////////////////////////////////////////////////////////////////////
// Get the type from a cell value

uint8_t cell_get_type(cell_t c) {
  return c & 0x3;
}

//////////////////////////////////////////////////////////////////////
// Get the direction from a cell value

uint8_t cell_get_direction(cell_t c) {
  return (c >> 2) & 0x3;
}

//////////////////////////////////////////////////////////////////////
// Get the color from a cell value

uint8_t cell_get_color(cell_t c) {
  return (c >> 4) & 0xf;
}

//////////////////////////////////////////////////////////////////////
// Consider whether the given move is valid.

int game_can_move(const game_info_t* info,
                  const game_state_t* state,
                  int color, int dir) {

  // Make sure color is valid
  assert(color < info->num_colors);

  assert(!(state->completed & (1 << color)));

  // Get cur pos x, y
  int cur_x, cur_y;
  pos_get_coords(state->cur_pos[color], &cur_x, &cur_y);

  // Get new x, y
  int new_x = cur_x + DIR_DELTA[dir][0];
  int new_y = cur_y + DIR_DELTA[dir][1];

  // If outside bounds, not legal
  if (new_x < 0 || new_x >= info->size ||
      new_y < 0 || new_y >= info->size) {
    return 0;
  }

  // Create a new position
  pos_t new_pos = pos_from_coords(new_x, new_y);
  assert( new_pos < MAX_CELLS );

  // Must be empty (TYPE_FREE)
  if (state->cells[new_pos]) {
    return 0;
  }

  if (g_options.prevent_self_touching) {
    
    pos_t neighbors[4];
    int num_neighbors = get_neighbors(info, new_x, new_y, neighbors);

    // All puzzles are designed so that a new path segment is adjacent
    // to at most one path segment of the same color -- the predecessor
    // to the new segment. We check this by iterating over the
    // neighbors.
    for (int n=0; n<num_neighbors; ++n) {

      // Assemble position
      pos_t neighbor_pos = neighbors[n];

      // If valid non-empty cell and not cur_pos and not goal_pos and
      // has our color, then fail
      if (state->cells[neighbor_pos] &&
          neighbor_pos != state->cur_pos[color] &&
          neighbor_pos != info->goal_pos[color] &&
          cell_get_color(state->cells[neighbor_pos]) == color) {
        return 0;
      }
    
    }

  }
  
  // It's valid
  return 1;

}


//////////////////////////////////////////////////////////////////////
// Print out game board

void game_print(const game_info_t* info,
                const game_state_t* state) {

  printf("%s", BLOCK_CHAR);
  for (size_t x=0; x<info->size; ++x) {
    printf("%s", BLOCK_CHAR);
  }
  printf("%s\n", BLOCK_CHAR);

  for (size_t y=0; y<info->size; ++y) {
    printf("%s", BLOCK_CHAR);
    for (size_t x=0; x<info->size; ++x) {
      cell_t cell = state->cells[pos_from_coords(x, y)];
      int type = cell_get_type(cell);
      int color = cell_get_color(cell);
      int dir = cell_get_direction(cell);
      int id = info->color_ids[color];
      char c = color_dict[id].display_char;
      switch (type) {
      case TYPE_FREE:
        printf(" ");
        break;
      case TYPE_PATH:
        printf("%s%c%s",
               set_color_str(id),
               g_options.color_display ? DIR_CHARS[dir] : c,
               reset_color_str());
        break;
      default:
        printf("%s%c%s",
               set_color_str(info->color_ids[color]),
               g_options.color_display ? (type == TYPE_INIT ? 'o' : 'O') : c,
               reset_color_str());
      }
    }
    printf("%s\n", BLOCK_CHAR);
  }

  printf("%s", BLOCK_CHAR);
  for (size_t x=0; x<info->size; ++x) {
    printf("%s", BLOCK_CHAR);
  }
  printf("%s\n", BLOCK_CHAR);
  
}

//////////////////////////////////////////////////////////////////////
// Update the game state to make the given move.

double game_make_move(const game_info_t* info,
                      game_state_t* state, 
                      int color, int dir) {

  // Make sure valid color
  assert(color < info->num_colors);

  // Get current x, y
  int cur_x, cur_y;
  pos_get_coords(state->cur_pos[color], &cur_x, &cur_y);

  // Assemble new x, y
  int new_x = cur_x + DIR_DELTA[dir][0];
  int new_y = cur_y + DIR_DELTA[dir][1];

  // Make sure valid
  assert( new_x >= 0 && new_x < info->size &&
          new_y >= 0 && new_y < info->size );

  // Make position
  pos_t new_pos = pos_from_coords(new_x, new_y);
  assert( new_pos < MAX_CELLS );

  // Make sure it's empty
  assert( state->cells[new_pos] == 0 );

  // Update the cell with the new cell value
  cell_t move = cell_create(TYPE_PATH, color, dir);

  // Update cells and new pos
  state->cells[new_pos] = move;
  state->cur_pos[color] = new_pos;
  --state->num_free;

  double action_cost = 1;

  if (pos_is_adjacent(new_pos, info->goal_pos[color])) {
    
    state->completed |= (1 << color);
    action_cost = 0;
    
  } else {
  
    pos_t neighbors[4];
    int num_neighbors = get_neighbors(info, new_x, new_y, neighbors);
    int num_free = 0;

    for (int n=0; n<num_neighbors; ++n) {
      if (state->cells[neighbors[n]] == 0) { ++num_free; }
    }

    if (num_free == 1) {
      action_cost = 0;
    } else if (num_free == 2) {
      action_cost = 1;
    }

  }
  
  return action_cost;

}

//////////////////////////////////////////////////////////////////////
// Read game board from text file

void game_read(const char* filename,
               game_info_t* info,
               game_state_t* state) {

  FILE* fp = fopen(filename, "r");

  if (!fp) {
    fprintf(stderr, "error opening %s\n", filename);
    exit(1);
  }

  memset(info, 0, sizeof(game_info_t));
  memset(state, 0, sizeof(game_state_t));
  
  memset(state->cur_pos, 0xff, sizeof(state->cur_pos));
  memset(info->goal_pos,  0xff, sizeof(info->goal_pos));

  size_t y=0;

  char buf[MAX_SIZE+1];

  char color_lookup[MAX_COLORS];

  memset(color_lookup, 0, MAX_COLORS);

  while (info->size == 0 || y < info->size) {

    char* s = fgets(buf, MAX_SIZE, fp);
    size_t l = strlen(s);
    
    if (!s || s[l-1] != '\n') {
      fprintf(stderr, "%s:%zu: unexpected EOF\n", filename, y+1);
      exit(1);
    }

    if (info->size == 0) {
      if (l < 6) {
        fprintf(stderr, "%s:1: expected at least 5 characters before newline\n",
                filename);
        exit(1);
      } else if (l-1 > MAX_SIZE) {
        fprintf(stderr, "%s:1: size too big!\n", filename);
        exit(1);
      }
      info->size = l-1;
    } else if (l != info->size + 1) {
      fprintf(stderr, "%s:%zu: wrong number of characters before newline "
              "(expected %zu, but got %zu)\n",
              filename, y+1,
              info->size, l-1);
    }

    for (size_t x=0; x<info->size; ++x) {
      
      char c = s[x];
      
      if (isalpha(c)) {

        pos_t pos = pos_from_coords(x, y);
        assert(pos < MAX_CELLS);
        
        // find it
        int color;
        for (color=0; color<info->num_colors; ++color) {
          if (color_lookup[color] == c) {
            break;
          }
        }

        
        if (color == info->num_colors) {

          if (info->num_colors == MAX_COLORS) {
            fprintf(stderr, "%s:%zu: can't use color %c"
                    "- too many colors!\n",
                    filename, y+1, c);
            exit(1);
          }

          int id = get_color_id(c);
          if (id < 0) {
            fprintf(stderr, "%s:%zu: unrecognized color %c\n",
                    filename, y+1, c);
            exit(1);
          }

          info->color_ids[color] = id;
          
          
          ++info->num_colors;
          color_lookup[color] = c;
          state->cur_pos[color] = pos;
          state->cells[pos] = cell_create(TYPE_INIT, color, 0);

        } else {

          if (info->goal_pos[color] != INVALID_POS) {
            fprintf(stderr, "%s:%zu too many %c already!\n",
                    filename, y+1, c);
            exit(1);
          }
          info->goal_pos[color] = pos;
          state->cells[pos] = cell_create(TYPE_GOAL, color, 0);

        }
      } else {

        ++state->num_free;

      }
    }
    
    ++y;
  }

  for (size_t color=0; color<info->num_colors; ++color) {
    if (info->goal_pos[color] == INVALID_POS) {
      game_print(info, state);
      fprintf(stderr, "\n\n%s: color %s%c%s has start but no end\n",
              filename,
              set_color_str(info->color_ids[color]),
              color_lookup[color],
              reset_color_str());
      exit(1);
    }
  }

  printf("read %zux%zu board with %zu colors from %s\n",
         info->size, info->size, info->num_colors, filename );

}


//////////////////////////////////////////////////////////////////////
// This and other connected component analysis functions from
// https://en.wikipedia.org/wiki/Disjoint-set_data_structure
//
// Create a new set with a single member:

region_t region_create(pos_t pos) {
  region_t rval;
  rval.parent = pos;
  rval.rank = 0;
  return rval;
}

//////////////////////////////////////////////////////////////////////
// Loop up root region for the given index p

pos_t region_find(region_t* regions, pos_t p) {
  if (regions[p].parent != INVALID_POS &&
      regions[p].parent != p) {
    assert(p != INVALID_POS);
    assert(p < MAX_CELLS);
    regions[p].parent = region_find(regions, regions[p].parent);
  }
  return regions[p].parent;
}

//////////////////////////////////////////////////////////////////////
// Merge two regions

void region_unite(region_t* regions,
                  pos_t a, pos_t b) {

  pos_t root_a = region_find(regions, a);
  pos_t root_b = region_find(regions, b);

  if (root_a == root_b) { return; }

  if (regions[root_a].rank < regions[root_b].rank) {
    regions[root_a].parent = root_b;
  } else if (regions[root_a].rank > regions[root_b].rank) {
    regions[root_b].parent = root_a;
  } else {
    regions[root_b].parent = root_a;
    ++regions[root_a].rank;
  }

}

//////////////////////////////////////////////////////////////////////
// Perform connected components analysis on game board. This is a
// 2-pass operation: one to build and merge the disjoint-set data
// structures, and another to re-index them so each unique region of
// free space gets its own index, starting at zero. Returns the number
// of freespace regions.

size_t game_build_regions(const game_info_t* info,
                          const game_state_t* state,
                          uint8_t rmap[MAX_CELLS]) {

  region_t regions[MAX_CELLS];
  
  // 1 pass to build regions
  for (size_t y=0; y<info->size; ++y) {
    for (size_t x=0; x<info->size; ++x) {
      pos_t pos = pos_from_coords(x, y);
      if (state->cells[pos]) {
        regions[pos] = region_create(INVALID_POS);
      } else {
        regions[pos] = region_create(pos);
        if (x) {
          pos_t pl = pos_from_coords(x-1, y);
          if (!state->cells[pl]) {
            region_unite(regions, pos, pl);
          }
        }
        if (y) {
          pos_t pu = pos_from_coords(x, y-1);
          if (!state->cells[pu]) {
            region_unite(regions, pos, pu);
          }
        }
      }
    }
  }

  uint8_t rlookup[MAX_CELLS];
  size_t rcount = 0;
  
  memset(rlookup, 0xff, sizeof(rlookup));

  // 2nd pass to order regions
  for (size_t y=0; y<info->size; ++y) {
    for (size_t x=0; x<info->size; ++x) {
      pos_t pos = pos_from_coords(x, y);
      pos_t root = region_find(regions, pos);
      if (root != INVALID_POS) {
        if (rlookup[root] == INVALID_POS) {
          rlookup[root] = rcount++;
        }
        rmap[pos] = rlookup[root];
      } else {
        rmap[pos] = INVALID_POS;
      }
    }
  }

  return rcount;

}

//////////////////////////////////////////////////////////////////////
// Helper function for game_regions_ok below -- this is used to add
// the current color bit flag to the regions adjacent to the current
// or goal position. 

void _game_regions_check(const game_info_t* info,
                         const game_state_t* state,
                         const uint8_t rmap[MAX_CELLS],
                         pos_t pos,
                         uint16_t cflag,
                         uint16_t* rflags) {

  pos_t neighbors[4];
  int num_neighbors = pos_get_neighbors(info, pos, neighbors);

  for (int n=0; n<num_neighbors; ++n) {

    pos_t neighbor_pos = neighbors[n];

    // find out what region it is in
    int neighbor_region = rmap[neighbor_pos];

    // if it is in a valid region
    if (neighbor_region != INVALID_POS) {
      // add this color to the region
      rflags[neighbor_region] |= cflag;
    }
  }
  
}

//////////////////////////////////////////////////////////////////////
// Check the results of the connected-component analysis to make sure
// that every color 

int game_regions_ok(const game_info_t* info,
                    const game_state_t* state,
                    size_t rcount,
                    const uint8_t rmap[MAX_CELLS]) {

  // For each region, we have bitflags to track whether current or
  // goal position is adjacent to the region. These get initted to 0.
  uint16_t cur_rflags[rcount];
  uint16_t goal_rflags[rcount];

  memset(cur_rflags, 0, sizeof(cur_rflags));
  memset(goal_rflags, 0, sizeof(goal_rflags));

  // For each color, figure out which regions touch its current and
  // goal position, and make sure no color is "stranded"
  for (int color=0; color<info->num_colors; ++color) {

    uint16_t cflag = (1 << color);

    // No worries if completed:
    if (state->completed & cflag) {
      continue;
    }

    // Add color flag to all regions for cur_pos
    _game_regions_check(info, state, rmap,
                        state->cur_pos[color], cflag, cur_rflags);

    // Add color flag to all regions for goal_pos
    _game_regions_check(info, state, rmap,
                        info->goal_pos[color], cflag, goal_rflags);


    // Ensure this color is not "stranded" -- at least region must
    // touch each non-completed color for both current and goal.
    size_t r;

    // for each region
    for (r=0; r<rcount; ++r) {
      // see if this region touches the color
      if ((cur_rflags[r] & cflag) &&
          (goal_rflags[r] & cflag)) {
        break;
      }
    }

    // There was no region that touched both current and goal,
    // unsolvable from here.
    if (r == rcount) {
      return 0;
    }

  }

  // For each region, make sure that there is at least one color whose
  // current and goal positions touch it; otherwise, the region is
  // stranded.
  for (size_t r=0; r<rcount; ++r) {
    if (!(cur_rflags[r] & goal_rflags[r])) {
      return 0;
    }
  }

  // Everything a-ok.
  return 1;
  
}

//////////////////////////////////////////////////////////////////////
// Print connected components of freespace
                        
void game_print_regions(const game_info_t* info,
                        const game_state_t* state,
                        uint8_t rmap[MAX_CELLS]) {

  printf("%s", BLOCK_CHAR);
  for (size_t x=0; x<info->size; ++x) {
    printf("%s", BLOCK_CHAR);
  }
  printf("%s\n", BLOCK_CHAR);

  for (size_t y=0; y<info->size; ++y) {
    printf("%s", BLOCK_CHAR);
    for (size_t x=0; x<info->size; ++x) {
      pos_t pos = pos_from_coords(x, y);
      pos_t rid = rmap[pos];
      if (!state->cells[pos]) {
        assert(id != INVALID_POS);
        char c = 65 + rid % 60;
        printf("%s%c%s",
               set_color_str(rid % MAX_COLORS), c, reset_color_str());
      } else {
        assert(id == INVALID_POS);
        printf(" ");
      }
    }
    printf("%s\n", BLOCK_CHAR);
  }

  printf("%s", BLOCK_CHAR);
  for (size_t x=0; x<info->size; ++x) {
    printf("%s", BLOCK_CHAR);
  }
  printf("%s\n", BLOCK_CHAR);
  
  printf("\n");
  
}


//////////////////////////////////////////////////////////////////////
// Create simple linear allocator for search nodes.

node_storage_t node_storage_create(size_t max_nodes) {

  node_storage_t storage;
  
  storage.start = malloc(max_nodes*sizeof(tree_node_t));
  
  if (!storage.start) {
    fprintf(stderr, "unable to allocate memory for node storage!\n");
    exit(1);
  }

  storage.capacity = max_nodes;
  storage.count = 0;

  return storage;
    
}

//////////////////////////////////////////////////////////////////////
// Allocate the next tree node.

tree_node_t* node_storage_alloc(node_storage_t* storage) {
  if (storage->count >= storage->capacity) {
    return NULL;
  }
  tree_node_t* rval = storage->start + storage->count;
  ++storage->count;
  return rval;
}

//////////////////////////////////////////////////////////////////////
// De-allocate a tree node -- note that we can only safely de-allocate
// the last node that was allocated. Of course we only check this with
// an assert, tho.

void node_storage_unalloc(node_storage_t* storage,
                          const tree_node_t* n) {

  assert( storage->count && n == storage->start + storage->count - 1 );
  --storage->count;

}

//////////////////////////////////////////////////////////////////////
// Free the memory allocated for this.

void node_storage_destroy(node_storage_t* storage) {
  free(storage->start);
}

//////////////////////////////////////////////////////////////////////
// Compare total cost for nodes, used by heap functions below.

int node_compare(const tree_node_t* a,
                 const tree_node_t* b) {

  double af = a->cost_to_come + a->cost_to_go;
  double bf = b->cost_to_come + b->cost_to_go;

  if (af != bf) {
    return af < bf ? -1 : 1;
  } else {
    return a < b ? -1 : 1;
  }

}

//////////////////////////////////////////////////////////////////////
// Create a binary heap to store the given # of nodes

queue_t heapq_create(size_t max_nodes) {
  queue_t rval;
  rval.heapq.start = malloc(sizeof(tree_node_t*) * max_nodes);
  if (!rval.heapq.start) {
    fprintf(stderr, "out of memory creating heapq!\n");
    exit(1);
  }
  rval.heapq.count = 0;
  rval.heapq.capacity = max_nodes;
  return rval;
}

//////////////////////////////////////////////////////////////////////
// Indexing macros for heaps

#define HEAPQ_PARENT_INDEX(i) (((i)-1)/2)
#define HEAPQ_LCHILD_INDEX(i) ((2*(i))+1)

//////////////////////////////////////////////////////////////////////
// For debugging, not used presently

int heapq_valid(const queue_t* q) {
  for (size_t i=1; i<q->heapq.count; ++i) {
    const tree_node_t* tc = q->heapq.start[i];
    const tree_node_t* tp = q->heapq.start[HEAPQ_PARENT_INDEX(i)];
    if (node_compare(tp, tc) > 0) {
      return 0;
    }
  }
  return 1;
}

//////////////////////////////////////////////////////////////////////
// Is heap queue empty?

int heapq_empty(const queue_t* q) {
  return q->heapq.count == 0;
}

//////////////////////////////////////////////////////////////////////
// Peek at the next item to be removed

const tree_node_t* heapq_peek(const queue_t* q) {
  assert(!heapq_empty(q));
  return q->heapq.start[0];
}

//////////////////////////////////////////////////////////////////////
// Enqueue a node onto the heap

void heapq_enqueue(queue_t* q, tree_node_t* node) {

  assert(q->heapq.count < q->heapq.capacity);

  size_t i = q->heapq.count++;
  size_t pi = HEAPQ_PARENT_INDEX(i);
  
  q->heapq.start[i] = node;
  
  while (i > 0 && node_compare(q->heapq.start[pi], q->heapq.start[i]) > 0) {
    tree_node_t* tmp = q->heapq.start[pi];
    q->heapq.start[pi] = q->heapq.start[i];
    q->heapq.start[i] = tmp;
    i = pi;
    pi = HEAPQ_PARENT_INDEX(i);
  }
                      
}

//////////////////////////////////////////////////////////////////////
// Helper function used for dequeueing

void _heapq_repair(queue_t* q, size_t i) {

  size_t li = HEAPQ_LCHILD_INDEX(i);
  size_t ri = li + 1;
  size_t smallest = i;

  if (li < q->heapq.count &&
      node_compare(q->heapq.start[i], q->heapq.start[li]) > 0) {
    smallest = li;
  }

  if (ri < q->heapq.count &&
      node_compare(q->heapq.start[smallest], q->heapq.start[ri]) > 0) {
    smallest = ri;
  }

  if (smallest != i){
    tree_node_t* tmp = q->heapq.start[i];
    q->heapq.start[i] = q->heapq.start[smallest];
    q->heapq.start[smallest] = tmp;
    _heapq_repair(q, smallest);
  }    

}

//////////////////////////////////////////////////////////////////////
// Pop a node off the heap

tree_node_t* heapq_deque(queue_t* q) {

  assert(!heapq_empty(q));

  tree_node_t* rval = q->heapq.start[0];
  --q->heapq.count;

  if (q->heapq.count) {
    q->heapq.start[0] = q->heapq.start[q->heapq.count];
    _heapq_repair(q, 0);
  }
  
  return rval;
  
}

//////////////////////////////////////////////////////////////////////
// Free memory allocated for heap

void heapq_destroy(queue_t* q) {
  free(q->heapq.start);
}

//////////////////////////////////////////////////////////////////////
// FIFO via flat array

queue_t fifo_create(size_t max_nodes) {
  queue_t rval;
  rval.fifo.start = malloc(sizeof(tree_node_t*) * max_nodes);
  if (!rval.fifo.start) {
    fprintf(stderr, "out of memory creating fifo!\n");
    exit(1);
  }
  rval.fifo.count = 0;
  rval.fifo.capacity = max_nodes;
  rval.fifo.next = 0;
  return rval;
}

//////////////////////////////////////////////////////////////////////
// Check if empty

int fifo_empty(const queue_t* q) {
  return q->fifo.next == q->fifo.count;
}

//////////////////////////////////////////////////////////////////////
// Push node into FIFO

void fifo_enqueue(queue_t* q, tree_node_t* n) {
  assert(q->fifo.count < q->fifo.capacity);
  q->fifo.start[q->fifo.count++] = n;
}

//////////////////////////////////////////////////////////////////////
// Peek at current FIFO node

const tree_node_t* fifo_peek(const queue_t* q) {
  assert(!fifo_empty(q));
  return q->fifo.start[q->fifo.next];
}

//////////////////////////////////////////////////////////////////////
// Dequeue node from FIFO

tree_node_t* fifo_deque(queue_t* q) {
  assert(!fifo_empty(q));
  return q->fifo.start[q->fifo.next++];
}

//////////////////////////////////////////////////////////////////////
// De-allocate storage for FIFO

void fifo_destroy(queue_t* q) {
  free(q->fifo.start);
}

//////////////////////////////////////////////////////////////////////
// Call this before calling the generic queue functions above.

void queue_setup() {

  if (g_options.search_astar_like) {

    queue_create = heapq_create;
    queue_enqueue = heapq_enqueue;
    queue_deque = heapq_deque;
    queue_destroy = heapq_destroy;
    queue_empty = heapq_empty;
    queue_peek = heapq_peek;

  } else {

    queue_create = fifo_create;
    queue_enqueue = fifo_enqueue;
    queue_deque = fifo_deque;
    queue_destroy = fifo_destroy;
    queue_empty = fifo_empty;
    queue_peek = fifo_peek;

  }

}

//////////////////////////////////////////////////////////////////////
// Create a node from the linear allocator. This does not properly set
// the cost to come and cost to go, those need to be finished later by
// node_update_costs.

tree_node_t* node_create(node_storage_t* storage,
                         tree_node_t* parent,
                         const game_info_t* info,
                         const game_state_t* state) {
  
  tree_node_t* rval = node_storage_alloc(storage);
  if (!rval) { return 0; }

  rval->parent = parent;
  rval->cost_to_come = 0;
  rval->cost_to_go = 0;

  memcpy(&(rval->state), state, sizeof(game_state_t));
  
  return rval;

}

//////////////////////////////////////////////////////////////////////
// Update the cost-to-come and cost-to-go for a node after a
// successful move has been made.

void node_update_costs(const game_info_t* info,
                       tree_node_t* n,
                       size_t action_cost) {

  // update cost to come
  if (n->parent) {
 
    n->cost_to_come = n->parent->cost_to_come + action_cost;

  } else {

    n->cost_to_come = 0;

  }
  
  n->cost_to_go = n->state.num_free;
  
}

//////////////////////////////////////////////////////////////////////
// Pick the next color to move deterministically

int game_next_move_color(const game_info_t* info,
                         const game_state_t* state) {

  size_t best_color = -1;
  int best_free = 4;

  for (size_t color=0; color<info->num_colors; ++color) {

    if (state->completed & (1 << color)) { continue; }

    if (g_options.most_constrained_color) {

      pos_t neighbors[4];

      int num_neighbors = pos_get_neighbors(info, state->cur_pos[color], neighbors);
      int num_free = 0;

      for (int n=0; n<num_neighbors; ++n) {
        if (state->cells[neighbors[n]] == 0) { ++num_free; }
      }

      if (num_free < best_free) {
        best_free = num_free;
        best_color = color;
      }

    } else {

      return color;

    }
    
  }

  assert(best_color < info->num_colors);

  return best_color;

}

//////////////////////////////////////////////////////////////////////
// Animate the solution by printing out boards in reverse order,
// following parent pointers back from solution to root.

void game_animate_solution(const game_info_t* info,
                           const tree_node_t* node) {

  if (node->parent) {
    game_animate_solution(info, node->parent);
  }

  printf("%s", unprint_board(info));
  game_print(info, &node->state);
  fflush(stdout);

  delay_seconds(0.1);
  
}

//////////////////////////////////////////////////////////////////////
// Peforms A* or BFS search

void game_search(const game_info_t* info,
                 const game_state_t* init_state) {

  size_t max_nodes = floor( g_options.max_storage_mb * MEGABYTE /
                            sizeof(tree_node_t) );


  node_storage_t storage = node_storage_create(max_nodes);

  tree_node_t* root = node_create(&storage, NULL, info, init_state);
  node_update_costs(info, root, 0);

  printf("will search up to %'zu nodes (%'g MB)\n",
         max_nodes, max_nodes*(double)sizeof(tree_node_t)/MEGABYTE);
  
  printf("heuristic at start is %'g\n\n",
         root->cost_to_go);

  game_print(info, init_state);

  queue_t q = queue_create(max_nodes);
  queue_enqueue(&q, root);

  int result = SEARCH_ACTIVE;
  const tree_node_t* solution_node = NULL;

  uint8_t rmap[MAX_CELLS];

  while (result == SEARCH_ACTIVE) {

    if (queue_empty(&q)) {
      result = SEARCH_UNREACHABLE;
      break;
    }

    tree_node_t* n = queue_deque(&q);
    assert(n);

    game_state_t* parent_state = &n->state;
    
    int color = game_next_move_color(info, parent_state);
      
    for (int dir=0; dir<4; ++dir) {

      if (game_can_move(info, &n->state, color, dir)) {
          
        tree_node_t* child = node_create(&storage, n, info,
                                         parent_state);

        if (!child) {

          result = SEARCH_FULL;
          break;
          
        }

        size_t action_cost = game_make_move(info, &child->state, color, dir);
        node_update_costs(info, child, action_cost);

        const game_state_t* child_state = &child->state;
          
        if ( child_state->num_free == 0 && 
             ( queue_empty(&q) ||
               node_compare(child, queue_peek(&q) ) <= 0) ) {          
          
          result = SEARCH_SUCCESS;
          solution_node = child;
          
          break;
      
        }

        if (g_options.check_freespace_regions) {

          size_t rcount = game_build_regions(info, child_state, rmap);

          if (!game_regions_ok(info, child_state, rcount, rmap)) {
            node_storage_unalloc(&storage, child);
            continue;
          }

        }
          
        queue_enqueue(&q, child);

      } // if can move

    } // for each dir

  } // while search active

  if (result == SEARCH_SUCCESS) {
    assert(solution_node);
    if (!g_options.animate_solution) {
      printf("\n");
      game_print(info, &solution_node->state);
    } else {
      delay_seconds(1.0);
      game_animate_solution(info, solution_node);
    }
    printf("\n");
  }  

  const char* result_str;

  if (result == SEARCH_SUCCESS) {
    result_str = "successful";
  } else if (result == SEARCH_FULL) {
    result_str = "ran out of memory";
  } else {
    result_str = "unsolvable";
  }

  double storage_mb = (storage.count * (double)sizeof(tree_node_t) / MEGABYTE);

  printf("search %s after %'zu nodes (%'g MB)\n",
         result_str,
         storage.count, storage_mb);

  if (result == SEARCH_SUCCESS) {
    
    assert(solution_node);

    printf("final cost to come=%'g, cost to go=%'g\n",
           solution_node->cost_to_come,
           solution_node->cost_to_go);

  }

  node_storage_destroy(&storage);
  queue_destroy(&q);
  
}

void usage(FILE* fp, int exitcode) {

  fprintf(fp,
          "usage: flow_solver [OPTIONS] BOARD.txt\n"
          "\n"
          "OPTIONS:\n"
          "\n"
          "  -A, --no-animation      Disable animating solution\n"
#ifndef _WIN32          
          "  -C, --color             Use ANSI color even if TERM not set or not a tty\n"
#endif          
          "  -s, --noself            Disable self-touch test\n"
          "  -r, --noregions         Disable freespace region analysis\n"
          "  -c, --constrained       Select next move by most constrained color\n"
          "  -b, --bfs               Run breadth-first search\n"
          "  -m, --max-storage NUM   Restrict storage to NUM MB (default %'g)\n"
          "  -h, --help              See this help text\n\n",
          g_options.max_storage_mb);

  exit(exitcode);
  
}

//////////////////////////////////////////////////////////////////////
// Check file exists

int exists(const char* fn) {

  FILE* fp = fopen(fn, "r");
  
  if (fp) {
    fclose(fp);
    return 1;
  } else {
    return 0;
  }

}

//////////////////////////////////////////////////////////////////////
// Parse command-line options

int parse_options(int argc, char** argv) {

  int input_arg = -1;

  if (argc < 2) {
    fprintf(stderr, "not enough args!\n\n");
    usage(stderr, 1);
  }
  
  for (int i=1; i<argc; ++i) {
    
    const char* opt = argv[i];
    
    if (!strcmp(opt, "-A") || !strcmp(opt, "--no-animation")) {
      g_options.animate_solution = 0;
#ifndef _WIN32      
    } else if (!strcmp(opt, "-C") || !strcmp(opt, "--color")) {
      g_options.color_display = 1;
#endif
    } else if (!strcmp(opt, "-s") || !strcmp(opt, "--noself")) {
      g_options.prevent_self_touching = 0;
    } else if (!strcmp(opt, "-r") || !strcmp(opt, "--noregions")) {
      g_options.check_freespace_regions = 0; 
    } else if (!strcmp(opt, "-c") || !strcmp(opt, "--constrained")) {
      g_options.most_constrained_color = 1; 
    } else if (!strcmp(opt, "-b") || !strcmp(opt, "--bfs")) {
      g_options.search_astar_like = 0;
    } else if (!strcmp(opt, "-m") || !strcmp(opt, "--max-storage")) {

      if (i+1 == argc) {
        fprintf(stderr, "-m, --max-storage needs argument\n");
        usage(stderr, 1);
      }
      
      opt = argv[++i];
      
      char* endptr;
      g_options.max_storage_mb = strtod(opt, &endptr);
      if (!endptr || *endptr || g_options.max_storage_mb <= 0) {
        fprintf(stderr, "error parsing max storage %s on command line!\n\n", opt);
        usage(stderr, 1);
      }

    } else if (!strcmp(opt, "-h") || !strcmp(opt, "--help")) {
      usage(stdout, 0);
    } else if (input_arg < 0 && exists(opt)) {
      input_arg = i;
    } else {
      if (opt[0] == '-') {
        fprintf(stderr, "unrecognized option: %s\n\n", opt);
        usage(stderr, 1);
      } else if (input_arg < 0) {
        fprintf(stderr, "error opening %s\n", opt);
        exit(1);
      } else {
        fprintf(stderr, "maximum one puzzle to read\n\n");
        usage(stderr, 1);
      }
    }
    
  }

  if (input_arg < 0) {
    fprintf(stderr, "no input file!\n\n");
    usage(stderr, 1);
  }

  return input_arg;

}

//////////////////////////////////////////////////////////////////////
// Main function

int main(int argc, char** argv) {

  setlocale(LC_NUMERIC, "");
    
  g_options.animate_solution = 1;
  g_options.color_display = terminal_has_color();
  g_options.prevent_self_touching = 1;
  g_options.check_freespace_regions = 1;
  g_options.most_constrained_color = 0;
  g_options.search_astar_like = 1;
  g_options.max_storage_mb = 512;

  int input_arg = parse_options(argc, argv);
  
  const char* input_file = argv[input_arg];
  
  queue_setup();
  
  game_info_t  info;
  game_state_t state;
  
  game_read(input_file, &info, &state);
  game_search(&info, &state);

  return 0;
  
}
