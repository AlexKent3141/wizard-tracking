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
  int hand_x, hand_y;
};

void update(struct state* s)
{
  ++s->counter;
}

void render_hand(LEAP_HAND hand)
{
  // Determine the height of the hand relative to the observer.
  int x = hand.type == eLeapHandType_Left ? max_x / 5 : 3 * max_x / 5;

  // Assuming a maximum range either side of 20cm.
  float point_x = hand.palm.position.x;
  float point_y = hand.palm.position.z;
  float half = max_y / 2;

  int y;
  if (point_y > 0)
  {
    float scaled = (half - point_y) / 100.0f;
    y = half - scaled * half;
  }
  else
  {
    float scaled = (point_y - half) / 100.0f;
    y = half + scaled * half;
  }

  mvprintw(y, x, "%.*s", max_x / 5, "----------------------");

  // Draw the hand using this centre line. Going to assume a square area.
  float mm_per_pixel = 200.0f / max_y;

  // Start with getting finger tips in the right coordinate system.
  for (int d = 0; d < 5; d++)
  {
    LEAP_VECTOR tip = hand.digits[d].distal.next_joint;
    float y_diff = (point_y - tip.z) / mm_per_pixel;
    float x_diff = (tip.x - point_x) / mm_per_pixel;
    float tip_y = y - y_diff;
    float tip_x = x + max_x / 10 + x_diff;
    mvwaddch(stdscr, tip_y, tip_x, '*');
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

  // Note: expected size is 23x91
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
