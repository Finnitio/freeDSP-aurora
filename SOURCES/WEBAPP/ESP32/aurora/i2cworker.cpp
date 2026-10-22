#include <Arduino.h>

#include "i2cworker.h"

static SemaphoreHandle_t i2cMutex = NULL;
static QueueHandle_t i2cQueue = NULL;

static const int I2C_QUEUE_LEN = 32;

//==============================================================================
/*! Runs the queued jobs one after the other.
 */
static void i2cWorkerTask( void* pvParameters )
{
  while( true )
  {
    std::function<void()>* job = NULL;
    if( xQueueReceive( i2cQueue, &job, portMAX_DELAY ) == pdTRUE && job )
    {
      (*job)();
      delete job;
    }
  }
}

void i2cInit( void )
{
  if( i2cMutex )
    return;
  i2cMutex = xSemaphoreCreateRecursiveMutex();
  i2cQueue = xQueueCreate( I2C_QUEUE_LEN, sizeof( std::function<void()>* ) );
  // Jobs do SPIFFS and JSON work too, hence the big stack. Core 1 like loop().
  xTaskCreatePinnedToCore( i2cWorkerTask, "i2cWorker", 10240, NULL, 1, NULL, 1 );
}

bool i2cPost( std::function<void()> job )
{
  if( !i2cQueue )
    return false;
  std::function<void()>* p = new std::function<void()>( job );
  if( xQueueSend( i2cQueue, &p, 0 ) != pdTRUE )
  {
    delete p;
    return false;
  }
  return true;
}

void i2cLock( void )
{
  if( i2cMutex )
    xSemaphoreTakeRecursive( i2cMutex, portMAX_DELAY );
}

void i2cUnlock( void )
{
  if( i2cMutex )
    xSemaphoreGiveRecursive( i2cMutex );
}
