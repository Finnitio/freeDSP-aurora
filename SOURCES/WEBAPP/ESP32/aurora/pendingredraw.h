#ifndef PENDINGREDRAW_H_
#define PENDINGREDRAW_H_

//==============================================================================
/*! The display redraw loop() still owes. loop() never waits for the I2C bus:
 *  serve() tries to draw and keeps the request if the bus is busy (the worker
 *  runs a Preset load or FIR upload), so the redraw happens on a later loop()
 *  pass. The newest request wins. Pure logic, used by loop() only, so it can
 *  be tested on the host.
 */
class PendingRedraw
{
public:
  enum Screen { NONE, SWITCHING_PRESET, UI };

  void request( Screen s ) { pending = s; }

  //----------------------------------------------------------------------------
  /*! tryDraw( Screen ) draws without waiting and returns false if it could not
   *  get the bus.
   */
  template <class TryDraw>
  void serve( TryDraw tryDraw )
  {
    if( pending != NONE && tryDraw( pending ) )
      pending = NONE;
  }

private:
  Screen pending = NONE;
};

#endif
