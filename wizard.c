#include "ncurses.h"
#include "stdlib.h"

int main()
{
  initscr();
  cbreak();
  noecho();
  timeout(0);

  int counter = 0;
  for (;;)
  {
    int key = wgetch(stdscr);
    if (key == 'q') break;

    napms(100);
    ++counter;

    mvprintw(1, 2, "Frame: %d\n", counter);
    box(stdscr, 0, 0);
  }

  endwin();

  return EXIT_SUCCESS;
}
