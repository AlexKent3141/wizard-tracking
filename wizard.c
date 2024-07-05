#include "LeapC.h"
#include "ncurses.h"

#include "assert.h"
#include "math.h"
#include "stdatomic.h"
#include "stdlib.h"
#include "string.h"
#include "threads.h"

// The scale represents the vertical height that we are rendering in mm.
#define SCALE 300.0f

#define PI 3.14159
#define DOT(a, b) (a.x * b.x + a.y * b.y + a.z * b.z)
#define VEC(a, b) { .x=a.x - b.x, .y=a.y - b.y, .z=a.z - b.z }
#define MIN(a, b) (a < b ? a : b)
#define MAX(a, b) (a > b ? a : b)
#define SGN(a) (a > 0 ? 1 : -1)
#define WRAP(y, x) s->life[y < 0 ? y + max_y : y >= max_y ? y - max_y : y][x < 0 ? x + max_x : x >= max_x ? x - max_x : x]

#define CLOAK_FABRIC_PAIR 1
#define CLOAK_FOLD_PAIR 2
#define LIFE_PAIR 3
#define HAND_PAIR 4

uint32_t seed = 0xDEADBEEF;
uint32_t latest_cloak_seed[2];
int latest_cloak_y[2];

uint32_t xorshift(uint32_t x)
{
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return x;
}

int max_x, max_y;
atomic_int quit = 0;

mtx_t tracking_mtx;
atomic_int received_tracking = 0;

struct tracking_data
{
  uint32_t num_hands;
  LEAP_HAND hands[2];
} latest_tracking;

int poll_tracking(void* data)
{
  LEAP_CONNECTION conn;
  assert(LeapCreateConnection(NULL, &conn) == eLeapRS_Success);
  assert(LeapOpenConnection(conn) == eLeapRS_Success);

  while (!quit)
  {
    LEAP_CONNECTION_MESSAGE msg;
    eLeapRS res = LeapPollConnection(conn, 100, &msg);
    if (res != eLeapRS_Success) continue;

    // Only interested in the latest tracking event.
    if (msg.type == eLeapEventType_Tracking)
    {
      assert(mtx_lock(&tracking_mtx) == thrd_success);

      int num_hands = msg.tracking_event->nHands;
      latest_tracking.num_hands = num_hands;
      memcpy(
        latest_tracking.hands,
        msg.tracking_event->pHands,
        num_hands * sizeof(LEAP_HAND));

      assert(mtx_unlock(&tracking_mtx) == thrd_success);
      received_tracking = 1;
    }
  }

  return 0;
}

enum pose
{
  PINCH,
  GRAB,
  PALM
};

struct action
{
  enum pose poses[2];
  int done[2];
};

void next_action(struct action* a)
{
  for (int i = 0; i < 2; i++)
  {
    a->poses[i] = (seed = xorshift(seed)) % 3;
    a->done[i] = 0;
  }
}

int is_grab(LEAP_HAND h)
{
  return h.grab_strength > 0.6f;
}

int is_pinch(LEAP_HAND h)
{
  return h.pinch_strength > 0.6f;
}

float angle_between(LEAP_VECTOR v1, LEAP_VECTOR v2)
{
  float angle = acos(DOT(v1, v2) / (sqrt(DOT(v1, v1)) * sqrt(DOT(v2, v2))));
  if (SGN(v1.y) != SGN(v2.y)) angle *= -1;
  return angle;
}

int is_palm(LEAP_HAND h)
{
  // Check that each finger is roughly straight.
  // Done by checking the angle between the bones of each digit.
  // Skipping the thumb.
  int palm_like = 1;
  for (int d = 1; d < 5 && palm_like; d++)
  {
    const LEAP_BONE b1 = h.digits[d].bones[1];
    const LEAP_BONE b2 = h.digits[d].bones[2];

    const LEAP_VECTOR v1 = VEC(b1.next_joint, b1.prev_joint);
    const LEAP_VECTOR v2 = VEC(b2.next_joint, b2.prev_joint);

    palm_like &= angle_between(v1, v2) < PI / 12;
  }

  return palm_like;
}

void check_action(struct action* a, LEAP_HAND h)
{
  int hand_index = h.type != eLeapHandType_Left;
  switch (a->poses[hand_index])
  {
    case GRAB: a->done[hand_index] |= is_grab(h); break;
    case PINCH:
    {
      // Pinch is often a sub-pose of Grab so ensure we're not in the
      // Grab state first.
      if (!is_grab(h))
      {
        a->done[hand_index] |= is_pinch(h);
      }
      break;
    }
    case PALM: a->done[hand_index] |= is_palm(h); break;
  }
}

struct state
{
  int frame_counter;
  int spell_counter;
  struct action a;
  int** life;
};

void update(struct state* s)
{
  ++s->frame_counter;

  // Check whether the current poses have been satisfied.
  if (received_tracking)
  {
    assert(mtx_lock(&tracking_mtx) == thrd_success);

    for (int i = 0; i < latest_tracking.num_hands; i++)
    {
      LEAP_HAND h = latest_tracking.hands[i];
      check_action(&s->a, h);
    }

    assert(mtx_unlock(&tracking_mtx) == thrd_success);
  }

  // If both actions have been satisfied then generate new ones.
  if (s->a.done[0] && s->a.done[1])
  {
    next_action(&s->a);

    ++s->spell_counter;

    if (s->spell_counter == 2)
    {
      // Create life.
      int side = max_y / 4;
      int start_y = (seed = xorshift(seed)) % (max_y - side);
      int start_x = (seed = xorshift(seed)) % (max_x - side);
      for (int y = 0; y < side; y++)
      {
        for (int x = 0; x < side; x++)
        {
          s->life[start_y + y][start_x + x] = (seed = xorshift(seed)) % 2;
        }
      }

      s->spell_counter = 0;
    }
  }

  // Update the GoL.
  if (s->frame_counter % 3 == 0)
  {
    static int** next_life = NULL;
    if (!next_life)
    {
      next_life = malloc(max_y * sizeof(int*));
      for (int y = 0; y < max_y; y++)
      {
        next_life[y] = malloc(max_x * sizeof(int));
      }
    }

    for (int y = 0; y < max_y; y++)
    {
      for (int x = 0; x < max_x; x++)
      {
        int alive_count =
          WRAP(y - 1, x - 1) + WRAP(y - 1, x) + WRAP(y - 1, x + 1) +
          WRAP(y, x - 1)                      + WRAP(y, x + 1) +
          WRAP(y + 1, x - 1) + WRAP(y + 1, x) + WRAP(y + 1, x + 1);

        // These rules generally die out so we get a brief "explosion".
        next_life[y][x] = s->life[y][x]
          ? alive_count > 2 && alive_count < 4
          : alive_count == 3;
      }
    }

    // Swap the buffers.
    int** temp = s->life;
    s->life = next_life;
    next_life = temp;
  }
}

void get_point_loc(
  float base_x,
  float base_y,
  float dx,
  float dy,
  float scale,
  int* tx,
  int* ty)
{
  float y_diff = dy / scale;
  float x_diff = dx / scale;
  *tx = base_x + max_x / 10 + x_diff;
  *ty = base_y - y_diff;
}

void render_bone(int x_start, int x_end, int y_start, int y_end)
{
  float num_steps = MAX(abs(x_end - x_start), abs(y_end - y_start));
  if (num_steps != 0)
  {
    float x_diff_per_step = (x_end - x_start) / num_steps;
    float y_diff_per_step = (y_end - y_start) / num_steps;

    // Pick the symbol based on the step ratio.
    char c = '|';
    if (x_diff_per_step != 0)
    {
      LEAP_VECTOR x_axis = {.x=1, .y=0, .z=0};
      LEAP_VECTOR dir = {.x=x_diff_per_step, .y=y_diff_per_step, .z=0};

      float angle = angle_between(x_axis, dir);

      const float tol = PI / 8;

      if (fabs(angle - (PI / 4)) < tol || fabs(angle + (3 * PI / 4)) < tol)
        c = '/';
      else if (fabs(angle + (PI / 4)) < tol || fabs(angle - (3 * PI / 4)) < tol)
        c = '\\';
      else if (fabs(angle) < tol || fabs(angle - PI) < tol)
        c = '-';
    }

    float x_cur = x_start + x_diff_per_step;
    float y_cur = y_start + y_diff_per_step;
    while (fabs(x_cur - (float)x_end) >= 0.5 || fabs(y_cur - (float)y_end) >= 0.5)
    {
      mvwaddch(stdscr, y_cur, x_cur, c);
      x_cur += x_diff_per_step;
      y_cur += y_diff_per_step;
    }
  }
}

void render_cloak(int x_left, int y_start, uint32_t s)
{
  // From palm centre down draw the cloak.
  // Getting some nice results by following the cloak downwards in continuous strands.

  // Unless the seed changes this function will always draw the same cloak.
  for (int line = 0; line < 100; line++)
  {
    int is_fold = line % 5 == 0;
    int x = x_left + (max_x / 20) + (((s = xorshift(s)) % 100) / 100.0f) * (max_x / 10);
    attron(COLOR_PAIR(is_fold ? CLOAK_FOLD_PAIR : CLOAK_FABRIC_PAIR));
    for (int y = y_start; y < y_start + max_y / 3; y++)
    {
      // Pick a move direction.
      static char cloak_chars[] = { '\\', '/', '|' };
      static int x_offsets[] = { 1, -1, 0 };
      int dir = (s = xorshift(s)) % 3;

      mvwaddch(stdscr, y, x, cloak_chars[dir]);

      x += x_offsets[dir];
    }

    attroff(COLOR_PAIR(is_fold ? CLOAK_FOLD_PAIR : CLOAK_FABRIC_PAIR));
  }
}

void render_hand(LEAP_HAND hand)
{
  // Determine the x coordinate of the left side of the hand.
  // To accommodate the UI the hands will be fixed to columns within the view.
  int x = hand.type == eLeapHandType_Left ? max_x / 5 : 3 * max_x / 5;

  // Determine the y location we want the centre of the palm to be placed at.
  float cx = hand.palm.position.x;
  float cy = hand.palm.position.z;
  float half = max_y / 2;

  int y = cy > 0
    ? half - 2 * half * (half - cy) / SCALE
    : half + 2 * half * (cy - half) / SCALE;

  // Draw the hand using this centre line. Going to assume a square area.
  float mm_per_pixel = SCALE / max_y;

  // Start with getting finger tips in the right coordinate system.
  attron(COLOR_PAIR(HAND_PAIR));
  attron(A_BOLD);
  for (int d = 0; d < 5; d++)
  {
    LEAP_VECTOR tip = hand.digits[d].distal.next_joint;
    int tip_x, tip_y;
    get_point_loc(x, y, tip.x - cx, cy - tip.z, mm_per_pixel, &tip_x, &tip_y);
    mvwaddch(stdscr, tip_y, tip_x, '*');

    // Capture the ends of the bones.
    LEAP_VECTOR joint = hand.digits[d].distal.prev_joint;
    int distal_prev_x, distal_prev_y;
    get_point_loc(
      x, y, joint.x - cx, cy - joint.z, mm_per_pixel, &distal_prev_x, &distal_prev_y);
    mvwaddch(stdscr, distal_prev_y, distal_prev_x, '^');

    int intermediate_prev_x, intermediate_prev_y;
    joint = hand.digits[d].intermediate.prev_joint;
    get_point_loc(
      x, y, joint.x - cx, cy - joint.z, mm_per_pixel, &intermediate_prev_x, &intermediate_prev_y);
    mvwaddch(stdscr, intermediate_prev_y, intermediate_prev_x, 'x');

    int proximal_prev_x, proximal_prev_y;
    joint = hand.digits[d].proximal.prev_joint;
    get_point_loc(
      x, y, joint.x - cx, cy - joint.z, mm_per_pixel, &proximal_prev_x, &proximal_prev_y);
    mvwaddch(stdscr, proximal_prev_y, proximal_prev_x, '#');

    // Draw the bone themselves.
    render_bone(
      proximal_prev_x, intermediate_prev_x, proximal_prev_y, intermediate_prev_y);
    render_bone(
      intermediate_prev_x, distal_prev_x, intermediate_prev_y, distal_prev_y);
    render_bone(
      distal_prev_x, tip_x, distal_prev_y, tip_y);
  }

  attroff(A_BOLD);
  attroff(COLOR_PAIR(HAND_PAIR));

  // If the hand has moved far enough then update the cloak seed.
  int chirality= hand.type == eLeapHandType_Left;
  if (abs(y - latest_cloak_y[chirality]) > 1)
  {
    latest_cloak_seed[chirality] = seed = xorshift(seed);
    latest_cloak_y[chirality] = y;
  }

  render_cloak(x, y, latest_cloak_seed[chirality]);
}

const char* pose_str(enum pose p)
{
  return p == PINCH ? "Pinch" : p == GRAB ? "Fist" : "Palm";
}

void render_pose(struct action a)
{
  const char* s = pose_str(a.poses[0]);
  if (!a.done[0]) mvprintw(5, 3 * max_x / 10 - strlen(s) / 2, "%s", s);

  s = pose_str(a.poses[1]);
  if (!a.done[1]) mvprintw(5, 7 * max_x / 10 - strlen(s), "%s", s);
}

void render(const struct state* s)
{
  mvprintw(1, 2, "Frame: %d\n", s->frame_counter);
  box(stdscr, 0, 0);

  // Render GoL state.
  attron(COLOR_PAIR(LIFE_PAIR));
  for (int y = 0; y < max_y; y++)
  {
    for (int x = 0; x < max_x; x++)
    {
      if (s->life[y][x])
      {
        mvwaddch(stdscr, y, x, '#');
      }
    }
  }

  attroff(COLOR_PAIR(LIFE_PAIR));

  // Latest tracking data.
  if (received_tracking)
  {
    assert(mtx_lock(&tracking_mtx) == thrd_success);
    mvprintw(2, 2, "Tracking: %d\n", latest_tracking.num_hands);

    for (int i = 0; i < latest_tracking.num_hands; i++)
    {
      render_hand(latest_tracking.hands[i]);
    }

    assert(mtx_unlock(&tracking_mtx) == thrd_success);
  }

  // Render next target pose.
  render_pose(s->a);

  mvprintw(3, 2, "Size: %d %d\n", max_y, max_x);
}

int main()
{
  initscr();
  cbreak();
  noecho();
  timeout(0);

  // Initialise colours.
  if (has_colors() == FALSE)
  {
    endwin();
    puts("Your terminal does not support colour");
    return EXIT_FAILURE;
  }

  start_color();
  init_pair(CLOAK_FABRIC_PAIR, COLOR_BLUE, COLOR_BLACK);
  init_pair(CLOAK_FOLD_PAIR, COLOR_GREEN, COLOR_BLACK);
  init_pair(LIFE_PAIR, COLOR_RED, COLOR_RED);
  init_pair(HAND_PAIR, COLOR_BLUE, COLOR_WHITE);

  getmaxyx(stdscr, max_y, max_x);

  // Kick off the tracking thread.
  mtx_init(&tracking_mtx, mtx_plain);
  thrd_t tid;
  thrd_create(&tid, &poll_tracking, NULL);

  struct state s;
  s.frame_counter = 0;
  s.spell_counter = 0;
  next_action(&s.a);

  // Allocate GoL arrays.
  s.life = malloc(max_y * sizeof(int*));
  for (int y = 0; y < max_y; y++)
  {
    s.life[y] = malloc(max_x * sizeof(int));
    memset(s.life[y], 0, max_x * sizeof(int));
  }

  while (!quit)
  {
    int key = wgetch(stdscr);
    quit = key == 'q';

    napms(100);

    update(&s);

    clear();
    render(&s);
    refresh();
  }

  thrd_join(tid, NULL);

  endwin();

  return EXIT_SUCCESS;
}
