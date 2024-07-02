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

struct state
{
  int counter;
};

void update(struct state* s)
{
  ++s->counter;
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
  float scale = 150.0f;

  // Determine the height of the hand relative to the observer.
  int x = hand.type == eLeapHandType_Left ? max_x / 5 : 3 * max_x / 5;

  // Assuming a maximum range either side of 20cm.
  float point_x = hand.palm.position.x;
  float point_y = hand.palm.position.z;
  float half = max_y / 2;

  int y;
  if (point_y > 0)
  {
    float scaled = (half - point_y) / scale;
    y = half - scaled * half;
  }
  else
  {
    float scaled = (point_y - half) / scale;
    y = half + scaled * half;
  }

  // Draw the hand using this centre line. Going to assume a square area.
  float mm_per_pixel = 2 * scale / max_y;

  // Start with getting finger tips in the right coordinate system.
  for (int d = 0; d < 5; d++)
  {
    LEAP_VECTOR tip = hand.digits[d].distal.next_joint;
    float tip_x, tip_y;
    get_point_loc(x, y, tip.x - point_x, point_y - tip.z, mm_per_pixel, &tip_x, &tip_y);
    mvwaddch(stdscr, tip_y, tip_x, '*');

    // Capture the positions of the bones.
    LEAP_VECTOR joint = hand.digits[d].distal.prev_joint;
    float joint_x, joint_y;
    get_point_loc(
      x, y, joint.x - point_x, point_y - joint.z, mm_per_pixel, &joint_x, &joint_y);
    mvwaddch(stdscr, joint_y, joint_x, '^');

    joint = hand.digits[d].intermediate.prev_joint;
    get_point_loc(
      x, y, joint.x - point_x, point_y - joint.z, mm_per_pixel, &joint_x, &joint_y);
    mvwaddch(stdscr, joint_y, joint_x, 'x');

    joint = hand.digits[d].proximal.prev_joint;
    get_point_loc(
      x, y, joint.x - point_x, point_y - joint.z, mm_per_pixel, &joint_x, &joint_y);
    mvwaddch(stdscr, joint_y, joint_x, '#');
  }
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

  struct state s = {};
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
