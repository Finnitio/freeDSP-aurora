#include <Arduino.h>
#include <SPIFFS.h>
#include <vector>

#include "matrix.h"
#include "matrixmodel.h"
#include "adau1452.h"
#include "i2cworker.h"
#include "plugin.h"

namespace
{
struct Op
{
  char type;  // 'g' gain, 'r' route, 's' source
  MatrixChannelKind kind;
  int idx;
  double val;
};

/*! Collects the model's writes; they are executed after the model call. */
class RecordPort : public MatrixPort
{
public:
  std::vector<Op> ops;
  void writeGain( MatrixChannelKind kind, int idx, double linear ) override { ops.push_back( { 'g', kind, idx, linear } ); }
  void writeRoute( int out, int route ) override { ops.push_back( { 'r', MATRIX_OUT, out, (double)route } ); }
  void writeSource( int bus, uint32_t sel ) override { ops.push_back( { 's', MATRIX_BUS, bus, (double)sel } ); }
};

RecordPort port;
MatrixModel* model = NULL;
SemaphoreHandle_t modelMutex = NULL;  // the model is used by the web task, the worker and loop()
uint8_t loadedPreset = 0;             // preset the model state belongs to

class ModelLock
{
public:
  ModelLock( void ) { xSemaphoreTake( modelMutex, portMAX_DELAY ); }
  ~ModelLock( void ) { xSemaphoreGive( modelMutex ); }
};

void writeWord( uint16_t addr, uint32_t w )
{
  byte val[4] = { (byte)( w >> 24 ), (byte)( w >> 16 ), (byte)( w >> 8 ), (byte)w };
  ADAU1452_WRITE_BLOCK( addr, val, 4 );
}

void execOps( const std::vector<Op>& ops )
{
  for( size_t ii = 0; ii < ops.size(); ii++ )
  {
    const Op& op = ops[ii];
    if( op.type == 'g' )
      writeWord( op.kind == MATRIX_BUS ? matrixMeta.busgain[op.idx] : matrixMeta.outgain[op.idx], convertTo824( op.val ) );
    else if( op.type == 'r' )
      writeWord( matrixMeta.outroute[op.idx], (uint32_t)op.val );
    else
    {
      paramInputs[op.idx].sel = (uint32_t)op.val;
      setInput( op.idx );
    }
  }
}

String presetFile( uint8_t preset )
{
  return String( "/matrix.00" ) + String( preset + 1 ) + String( ".json" );
}

void writePresetFile( uint8_t preset, const std::string& json )
{
  File f = SPIFFS.open( presetFile( preset ), "w" );
  if( !f )
  {
    Serial.println( F("[ERROR] matrix: cannot write preset file") );
    return;
  }
  f.write( (const uint8_t*)json.data(), json.size() );
  f.close();
}
}  // namespace

bool isMatrixPlugin( void )
{
  return pluginKind == "matrix";
}

void matrixInit( void )
{
  if( !isMatrixPlugin() )
    return;
  modelMutex = xSemaphoreCreateMutex();
  model = new MatrixModel( port, matrixMeta.nbus, matrixMeta.nout );
}

void matrixLoadPreset( void )
{
  if( !model )
    return;

  std::string json;
  File f = SPIFFS.open( presetFile( currentPreset ), "r" );
  if( f )
  {
    json.resize( f.size() );
    f.read( (uint8_t*)&json[0], json.size() );
    f.close();
  }

  std::vector<Op> ops;
  {
    ModelLock lock;
    if( model->dirty() )  // changes of the previous preset that auto-save has not stored yet
      writePresetFile( loadedPreset, model->savePreset() );
    port.ops.clear();
    std::string err = json.empty() ? "no preset file" : model->loadPreset( json );
    if( !err.empty() )
    {
      Serial.print( F("matrix: using defaults: ") );
      Serial.println( err.c_str() );
      model->loadDefaults();
    }
    loadedPreset = currentPreset;
    ops.swap( port.ops );
  }
  execOps( ops );
}

bool matrixApply( JsonObject patch, std::string& reply )
{
  if( !model )
  {
    reply = "not a matrix plugin";
    return false;
  }
  std::vector<Op> ops;
  {
    ModelLock lock;
    port.ops.clear();
    reply = model->apply( patch, millis() );
    ops.swap( port.ops );
    if( reply.empty() )
      reply = model->stateJson();
    else
      return false;
  }
  if( !ops.empty() && !i2cPost( [ops]() { execOps( ops ); } ) )
  {
    reply = "busy";
    return false;
  }
  return true;
}

std::string matrixState( void )
{
  if( !model )
    return "{}";
  ModelLock lock;
  return model->stateJson();
}

void matrixLoop( void )
{
  if( !model )
    return;
  std::string json;
  uint8_t preset;
  {
    ModelLock lock;
    if( !model->saveDue( millis() ) )
      return;
    json = model->savePreset();
    preset = loadedPreset;
    model->markSaved();
  }
  i2cPost( [json, preset]() { writePresetFile( preset, json ); } );
}
