#include "LeapC.h"
#include "ncurses.h"

#include "assert.h"
#include "math.h"
#include "stdatomic.h"
#include "stdlib.h"
#include "string.h"
#include "threads.h"

#define PI 3.14159
#define DOT(a, b) (a.x * b.x + a.y * b.y + a.z * b.z)
#define VEC(a, b) { a.x - b.x, a.y - b.y, a.z - b.z }

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
    a->poses[i] = rand() % 3;
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
  return acos(DOT(v1, v2) / (sqrt(DOT(v1, v1)) * sqrt(DOT(v2, v2))));
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
  int counter;
  struct action a;
};

void update(struct state* s)
{
  ++s->counter;

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
  }
}

void get_point_loc(
  float base_x,
  float base_y,
  float dx,
  float dy,
  float scale,
  float* tx,
  float* ty)
{
  float y_diff = dy / scale;
  float x_diff = dx / scale;
  *tx = base_x + max_x / 10 + x_diff;
  *ty = base_y - y_diff;
}

void render_hand(LEAP_HAND hand)
{
  // The scale represents the vertical height that we are rendering in mm.
  const float scale = 300.0f;

  // Determine the x coordinate of the left side of the hand.
  // To accommodate the UI the hands will be fixed to columns within the view.
  int x = hand.type == eLeapHandType_Left ? max_x / 5 : 3 * max_x / 5;

  // Determine the y location we want the centre of the palm to be placed at.
  float cx = hand.palm.position.x;
  float cy = hand.palm.position.z;
  float half = max_y / 2;

  int y = cy > 0
    ? half - 2 * half * (half - cy) / scale
    : half + 2 * half * (cy - half) / scale;

  // Draw the hand using this centre line. Going to assume a square area.
  float mm_per_pixel = scale / max_y;

  // Start with getting finger tips in the right coordinate system.
  for (int d = 0; d < 5; d++)
  {
    LEAP_VECTOR tip = hand.digits[d].distal.next_joint;
    float tip_x, tip_y;
    get_point_loc(x, y, tip.x - cx, cy - tip.z, mm_per_pixel, &tip_x, &tip_y);
    mvwaddch(stdscr, tip_y, tip_x, '*');

    // Capture the positions of the bones.
    LEAP_VECTOR joint = hand.digits[d].distal.prev_joint;
    float joint_x, joint_y;
    get_point_loc(
      x, y, joint.x - cx, cy - joint.z, mm_per_pixel, &joint_x, &joint_y);
    mvwaddch(stdscr, joint_y, joint_x, '^');

    joint = hand.digits[d].intermediate.prev_joint;
    get_point_loc(
      x, y, joint.x - cx, cy - joint.z, mm_per_pixel, &joint_x, &joint_y);
    mvwaddch(stdscr, joint_y, joint_x, 'x');

    joint = hand.digits[d].proximal.prev_joint;
    get_point_loc(
      x, y, joint.x - cx, cy - joint.z, mm_per_pixel, &joint_x, &joint_y);
    mvwaddch(stdscr, joint_y, joint_x, '#');
  }
}

const char* pose_str(enum pose p)
{
  return p == PINCH ? "Pinch" : p == GRAB ? "Fist" : "Palm";
}

void render_pose(struct action a)
{
  if (!a.done[0]) mvprintw(max_y / 2, 2, pose_str(a.poses[0]));
  if (!a.done[1]) mvprintw(max_y / 2, max_x - 7, pose_str(a.poses[1]));
}

void render(const struct state* s)
{
  mvprintw(1, 2, "Frame: %d\n", s->counter);
  box(stdscr, 0, 0);

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

  getmaxyx(stdscr, max_y, max_x);

  // Kick off the tracking thread.
  mtx_init(&tracking_mtx, mtx_plain);
  thrd_t tid;
  thrd_create(&tid, &poll_tracking, NULL);

  struct state s;
  s.counter = 0;
  next_action(&s.a);
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
