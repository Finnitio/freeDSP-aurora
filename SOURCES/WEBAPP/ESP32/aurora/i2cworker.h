#ifndef I2CWORKER_H_
#define I2CWORKER_H_

#include <functional>

/*! One worker task runs all slow I2C work (DSP, DAC, add-ons). Web handlers and
 *  loop() queue jobs with i2cPost() and return. Every single I2C access, from
 *  any task, additionally holds the bus mutex (I2cLock), so display, volume and
 *  add-on traffic can never interleave with a DSP write on the wire.
 */

/*! Creates the bus mutex and the worker task. Call once, before the first I2C access. */
void i2cInit( void );

/*! Queues a job for the worker. Returns false if the queue is full (job dropped). */
bool i2cPost( std::function<void()> job );

void i2cLock( void );
void i2cUnlock( void );

/*! Holds the (recursive) bus mutex for the scope. */
class I2cLock
{
public:
  I2cLock( void ) { i2cLock(); }
  ~I2cLock( void ) { i2cUnlock(); }
};

#endif
