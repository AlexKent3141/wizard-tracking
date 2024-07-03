#include "assert.h"
#include "LeapC.h"
#include "ncurses.h"
#include "stdatomic.h"
#include "stdlib.h"
#include "string.h"
#include "threads.h"

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
  GRAB
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
    a->poses[i] = rand() % 2;
    a->done[i] = 0;
  }
}

void check_action(struct action* a, LEAP_HAND h)
{
  int hand_index = h.type != eLeapHandType_Left;
  switch (a->poses[hand_index])
  {
    case GRAB: a->done[hand_index] |= h.grab_strength > 0.6f; break;
    case PINCH:
    {
      // Pinch is often a sub-pose of Grab so ensure we're not in the
      // Grab state first.
      if (h.grab_strength < 0.6f)
      {
        a->done[hand_index] |= h.pinch_strength > 0.6f;
      }
      break;
    }
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
  float scale = 300.0f;

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

void render_pose(struct action a)
{
  if (!a.done[0]) mvprintw(max_y / 2, 2, a.poses[0] == PINCH ? "Pinch" : "Fist");
  if (!a.done[1]) mvprintw(max_y / 2, max_x - 7, a.poses[1] == PINCH ? "Pinch" : "Fist");
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
