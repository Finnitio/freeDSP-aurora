// Host tests for the deferred display redraw in loop().
// Build & run: g++ -std=c++11 -Wall -o test_pendingredraw SOURCES/WEBAPP/ESP32/test/test_pendingredraw.cpp && ./test_pendingredraw
#include <cassert>
#include <cstdio>
#include <vector>
#include "../aurora/pendingredraw.h"

// Stands in for the display: records what was drawn, fails while the bus is busy.
struct FakeDisplay
{
  bool busBusy = false;
  std::vector<PendingRedraw::Screen> drawn;

  bool draw( PendingRedraw::Screen s )
  {
    if( busBusy )
      return false;
    drawn.push_back( s );
    return true;
  }
};

static void serve( PendingRedraw& redraw, FakeDisplay& display )
{
  redraw.serve( [&display]( PendingRedraw::Screen s ) { return display.draw( s ); } );
}

static void nothingRequestedDrawsNothing()
{
  PendingRedraw redraw;
  FakeDisplay display;
  serve( redraw, display );
  assert( display.drawn.empty() );
}

static void requestIsDrawnOnceWhenBusIsFree()
{
  PendingRedraw redraw;
  FakeDisplay display;
  redraw.request( PendingRedraw::UI );
  serve( redraw, display );
  serve( redraw, display );
  assert( display.drawn.size() == 1 );
  assert( display.drawn[0] == PendingRedraw::UI );
}

// The worker holds the bus for a whole preset load or FIR upload: the redraw
// waits and is drawn on the first serve() after the job, never lost.
static void redrawWhileBusIsBusyIsDrawnLater()
{
  PendingRedraw redraw;
  FakeDisplay display;
  display.busBusy = true;
  redraw.request( PendingRedraw::UI );
  serve( redraw, display );
  serve( redraw, display );
  assert( display.drawn.empty() );
  display.busBusy = false;
  serve( redraw, display );
  serve( redraw, display );
  assert( display.drawn.size() == 1 );
  assert( display.drawn[0] == PendingRedraw::UI );
}

// "Switching preset" is shown, then the finished switch asks for the main UI.
// If the bus was busy all along, only the newest screen is drawn.
static void newestRequestWins()
{
  PendingRedraw redraw;
  FakeDisplay display;
  display.busBusy = true;
  redraw.request( PendingRedraw::SWITCHING_PRESET );
  serve( redraw, display );
  redraw.request( PendingRedraw::UI );
  display.busBusy = false;
  serve( redraw, display );
  assert( display.drawn.size() == 1 );
  assert( display.drawn[0] == PendingRedraw::UI );
}

static void switchingScreenIsDrawnWhenBusIsFree()
{
  PendingRedraw redraw;
  FakeDisplay display;
  redraw.request( PendingRedraw::SWITCHING_PRESET );
  serve( redraw, display );
  redraw.request( PendingRedraw::UI );
  serve( redraw, display );
  assert( display.drawn.size() == 2 );
  assert( display.drawn[0] == PendingRedraw::SWITCHING_PRESET );
  assert( display.drawn[1] == PendingRedraw::UI );
}

int main()
{
  nothingRequestedDrawsNothing();
  requestIsDrawnOnceWhenBusIsFree();
  redrawWhileBusIsBusyIsDrawnLater();
  newestRequestWins();
  switchingScreenIsDrawnWhenBusIsFree();
  printf( "test_pendingredraw: all passed\n" );
  return 0;
}
