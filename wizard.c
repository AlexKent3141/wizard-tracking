#include "LeapC.h"
#include "ncurses.h"
#include "stdlib.h"

int max_x, max_y;

struct state
{
  int counter;
  int hand_x, hand_y;
};

void update(struct state* s)
{
  ++s->counter;
  s->hand_x = rand() % max_x;
  s->hand_y = rand() % max_y;
}

void render(const struct state* s)
{
  mvprintw(1, 2, "Frame: %d\n", s->counter);
  box(stdscr, 0, 0);

  mvwaddch(stdscr, s->hand_x, s->hand_y, '#');
}

int main()
{
  initscr();
  cbreak();
  noecho();
  timeout(0);

  getmaxyx(stdscr, max_x, max_y);

  struct state s = {};
  for (;;)
  {
    int key = wgetch(stdscr);
    if (key == 'q') break;

    napms(100);

    update(&s);

    clear();
    render(&s);
    refresh();
  }

  endwin();

  return EXIT_SUCCESS;
}
