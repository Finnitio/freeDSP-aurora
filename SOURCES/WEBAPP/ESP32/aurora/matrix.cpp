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
enum OpType { OP_GAIN, OP_ROUTE, OP_SOURCE, OP_BIQUAD, OP_POLARITY, OP_DELAY, OP_WAIT };

struct Op
{
  OpType type;
  MatrixChannelKind kind;
  int idx;
  double gain;   // OP_GAIN: linear
  uint32_t sel;  // OP_ROUTE: route, OP_SOURCE: source code, OP_BIQUAD: stage, OP_POLARITY: 0/1, OP_DELAY: samples
  float a[3], b[3];  // OP_BIQUAD
};

/*! Collects the model's writes; they are executed after the model call. */
class RecordPort : public MatrixPort
{
public:
  std::vector<Op> ops;
  void writeGain( MatrixChannelKind kind, int idx, double linear ) override { ops.push_back( { OP_GAIN, kind, idx, linear, 0, {}, {} } ); }
  void writeRoute( int out, int route ) override { ops.push_back( { OP_ROUTE, MATRIX_OUT, out, 0.0, (uint32_t)route, {}, {} } ); }
  void writeSource( int bus, uint32_t sel ) override { ops.push_back( { OP_SOURCE, MATRIX_BUS, bus, 0.0, sel, {}, {} } ); }
  void writePolarity( int out, bool invert ) override { ops.push_back( { OP_POLARITY, MATRIX_OUT, out, 0.0, invert ? 1u : 0u, {}, {} } ); }
  void writeDelay( int out, int samples ) override { ops.push_back( { OP_DELAY, MATRIX_OUT, out, 0.0, (uint32_t)samples, {}, {} } ); }
  void waitMs( int ms ) override { ops.push_back( { OP_WAIT, MATRIX_OUT, 0, 0.0, (uint32_t)ms, {}, {} } ); }
  void writeBiquad( MatrixChannelKind kind, int idx, int stage, const float a[3], const float b[3] ) override
  {
    Op op = { OP_BIQUAD, kind, idx, 0.0, (uint32_t)stage, {}, {} };
    memcpy( op.a, a, sizeof( op.a ) );
    memcpy( op.b, b, sizeof( op.b ) );
    ops.push_back( op );
  }
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
    if( op.type == OP_GAIN )
      writeWord( op.kind == MATRIX_BUS ? matrixMeta.busgain[op.idx] : matrixMeta.outgain[op.idx], convertTo824( op.gain ) );
    else if( op.type == OP_ROUTE )
      writeWord( matrixMeta.outroute[op.idx], op.sel );
    else if( op.type == OP_WAIT )
      vTaskDelay( pdMS_TO_TICKS( op.sel ) );  // runs on the I2C worker, never in a web handler
    else if( op.type == OP_POLARITY )
      writeWord( matrixMeta.outpol[op.idx], op.sel );
    else if( op.type == OP_DELAY )
      writeWord( matrixMeta.outdly[op.idx], op.sel );
    else if( op.type == OP_BIQUAD )
    {
      const uint16_t* base = ( op.kind == MATRIX_BUS ? matrixMeta.busbiquad : matrixMeta.outbiquad );
      ADAU1452_WRITE_BIQUAD( base[op.idx * matrixMeta.nbiquad + op.sel], op.a, op.b );
    }
    else
    {
      paramInputs[op.idx].sel = op.sel;
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
  model = new MatrixModel( port, matrixMeta.nbus, matrixMeta.nout, matrixMeta.nbiquad );
}

std::string matrixCoeffs( const char* ch, int idx )
{
  if( !model )
    return "";
  ModelLock lock;
  return model->coeffsJson( ch, idx );
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
  std::string oldJson;  // changes of the previous preset that auto-save has not stored yet
  uint8_t oldPreset;
  {
    ModelLock lock;
    oldPreset = loadedPreset;
    if( model->dirty() )
      oldJson = model->savePreset();
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
  if( !oldJson.empty() )
    writePresetFile( oldPreset, oldJson );
  execOps( ops );
}

bool matrixApply( JsonObject patch, std::string& reply )
{
  if( !model )
  {
    reply = "not a matrix plugin";
    return false;
  }
  // i2cPost() does not block, so the post stays under the lock: a refused post is undone
  // before anyone else sees the model, and posts keep the order of the model changes.
  ModelLock lock;
  port.ops.clear();
  reply = model->apply( patch, millis() );
  if( !reply.empty() )
    return false;
  std::vector<Op> ops;
  ops.swap( port.ops );
  if( !ops.empty() && !i2cPost( [ops]() { execOps( ops ); } ) )
  {
    model->undo();
    reply = "busy";
    return false;
  }
  reply = model->stateJson();
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
  ModelLock lock;
  if( !model->saveDue( millis() ) )
    return;
  std::string json = model->savePreset();
  uint8_t preset = loadedPreset;
  // only a queued save counts as saved; otherwise it is retried on the next call
  if( i2cPost( [json, preset]() { writePresetFile( preset, json ); } ) )
    model->markSaved();
}
