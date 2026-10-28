#include <Arduino.h>
#include <atomic>

#include "i2cworker.h"

static SemaphoreHandle_t i2cMutex = NULL;
static QueueHandle_t i2cQueue = NULL;

static const int I2C_QUEUE_LEN = 32;
// Jobs do SPIFFS and JSON work too (preset load parses the matrix preset), hence
// the big stack. The high-water mark logged below tells whether it is big enough.
static const uint32_t I2C_WORKER_STACK = 10240;
static std::atomic<int> i2cPending( 0 );  // queued + running jobs

//==============================================================================
/*! Runs the queued jobs one after the other.
 */
static void i2cWorkerTask( void* pvParameters )
{
  UBaseType_t lowestFree = UINT32_MAX;
  while( true )
  {
    std::function<void()>* job = NULL;
    if( xQueueReceive( i2cQueue, &job, portMAX_DELAY ) == pdTRUE && job )
    {
      (*job)();
      delete job;
      i2cPending--;

      // Logged whenever a job used more stack than every job before it, so the
      // last line after a preset load or FIR upload is the worst case so far.
      const UBaseType_t free = uxTaskGetStackHighWaterMark( NULL );  // bytes on ESP32
      if( free < lowestFree )
      {
        lowestFree = free;
        Serial.printf( "[I2C] worker stack high-water: %u of %u bytes never used\n",
                       (unsigned)free, (unsigned)I2C_WORKER_STACK );
      }
    }
  }
}

void i2cInit( void )
{
  if( i2cMutex )
    return;
  i2cMutex = xSemaphoreCreateRecursiveMutex();
  i2cQueue = xQueueCreate( I2C_QUEUE_LEN, sizeof( std::function<void()>* ) );
  // Core 1 like loop().
  xTaskCreatePinnedToCore( i2cWorkerTask, "i2cWorker", I2C_WORKER_STACK, NULL, 1, NULL, 1 );
}

bool i2cPost( std::function<void()> job )
{
  if( !i2cQueue )
    return false;
  std::function<void()>* p = new std::function<void()>( job );
  i2cPending++;
  if( xQueueSend( i2cQueue, &p, 0 ) != pdTRUE )
  {
    i2cPending--;
    delete p;
    Serial.println( F("[ERROR] I2C queue full, job dropped") );
    return false;
  }
  return true;
}

bool i2cIdle( void )
{
  return i2cPending == 0;
}

void i2cLock( void )
{
  if( i2cMutex )
    xSemaphoreTakeRecursive( i2cMutex, portMAX_DELAY );
}

bool i2cTryLock( void )
{
  if( !i2cMutex )
    return true;
  return xSemaphoreTakeRecursive( i2cMutex, 0 ) == pdTRUE;
}

void i2cUnlock( void )
{
  if( i2cMutex )
    xSemaphoreGiveRecursive( i2cMutex );
}
