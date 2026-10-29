#include <Arduino.h>
#include <SPIFFS.h>
#include <vector>

#include "matrix.h"
#include "matrixmodel.h"
#include "filterimport.h"
#include "mastervolume.h"
#include "adau1452.h"
#include "i2cworker.h"
#include "plugin.h"
#include "addons.h"
#include "presetslots.h"

namespace
{
enum OpType { OP_GAIN, OP_ROUTE, OP_SOURCE, OP_BIQUAD, OP_POLARITY, OP_DELAY, OP_WAIT, OP_SPDIF, OP_ADDON };

struct Op
{
  OpType type;
  MatrixChannelKind kind;
  int idx;
  double gain;   // OP_GAIN: linear
  uint32_t sel;  // OP_ROUTE: route, OP_SOURCE: source code, OP_BIQUAD: stage, OP_POLARITY: 0/1, OP_DELAY: samples
  float a[3], b[3];  // OP_BIQUAD
  uint32_t sel2;     // OP_SPDIF: right code (sel = left code)
  MatrixAddOn addOn;  // OP_ADDON
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
  void writeSpdifOut( uint32_t left, uint32_t right ) override { ops.push_back( { OP_SPDIF, MATRIX_OUT, 0, 0.0, left, {}, {}, right } ); }
  void writeAddOn( const MatrixAddOn& cfg ) override { ops.push_back( { OP_ADDON, MATRIX_OUT, 0, 0.0, 0, {}, {}, 0, cfg } ); }
  void writeBiquad( MatrixChannelKind kind, int idx, int stage, const float a[3], const float b[3] ) override
  {
    Op op = { OP_BIQUAD, kind, idx, 0.0, (uint32_t)stage, {}, {}, 0 };
    memcpy( op.a, a, sizeof( op.a ) );
    memcpy( op.b, b, sizeof( op.b ) );
    ops.push_back( op );
  }
};

RecordPort port;
MatrixModel* model = NULL;
SemaphoreHandle_t modelMutex = NULL;  // the model is used by the web task, the worker and loop()
bool keepStored = false;              // its file failed to load: never overwrite it with defaults
MasterVolumeSaver* volumeSaver = NULL;  // under modelMutex too: changed on the worker, stored from loop()

const char* const MASTER_VOLUME_FILE = "/mastervol.txt";

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
    else if( op.type == OP_SPDIF )
    {
      spdifOutput.selectionLeft = op.sel;
      spdifOutput.selectionRight = op.sel2;
      setSpdifOutputRouting();
    }
    else if( op.type == OP_ADDON )
      writeAddOnBConfig( op.addOn.addr, op.addOn.reg, op.addOn.data );
    else if( op.type == OP_BIQUAD )
    {
      const uint16_t* base = ( op.kind == MATRIX_BUS ? matrixMeta.busbiquad : matrixMeta.outbiquad );
      const uint16_t addr = base ? base[op.idx * matrixMeta.nbiquad + op.sel] : 0;
      static bool warned = false;
      if( addr == 0 )  // plugin.ini has no (or a short) biquad table: never write to address 0
      {
        if( !warned )
        {
          warned = true;
          Serial.println( F("[ERROR] matrix: plugin.ini has no biquad address, skipping biquad writes") );
        }
      }
      else
        writeBiquad( addr, op.a, op.b );
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
  return String( matrixPresetFileName( preset ).c_str() );
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
/*! Add-on B config of a Preset from /addoncfg.00N; not set without Add-on B or a (whole) file. */
MatrixAddOn readAddOnFile( uint8_t preset )
{
  MatrixAddOn cfg = MatrixAddOn();
  if( Settings.addonid != ADDON_B || preset >= MAX_NUM_PRESETS )
    return cfg;
  File f = SPIFFS.open( presetAddonCfgFile[preset], "r" );
  if( !f )
    return cfg;
  uint8_t v[3];
  if( f.read( v, 3 ) == 3 )
    cfg = { true, v[0], v[1], v[2] };
  f.close();
  return cfg;
}

void writeAddOnFile( uint8_t preset, const MatrixAddOn& cfg )
{
  if( !cfg.set )
    return;
  File f = SPIFFS.open( presetAddonCfgFile[preset], "w" );
  if( !f )
  {
    Serial.println( F("[ERROR] matrix: cannot write add-on config file") );
    return;
  }
  const uint8_t v[3] = { cfg.addr, cfg.reg, cfg.data };
  f.write( v, 3 );
  f.close();
}

/*! Autosave of a Preset: the matrix file and, with Add-on B, its config. */
void savePreset( uint8_t preset, const std::string& json, const MatrixAddOn& addOn )
{
  writePresetFile( preset, json );
  writeAddOnFile( preset, addOn );
}

float readMasterVolumeFile( void )
{
  File f = SPIFFS.open( MASTER_VOLUME_FILE, "r" );
  if( !f )
    return parseStoredVolume( NULL );
  String text = f.readString();
  f.close();
  return parseStoredVolume( text.c_str() );
}

void writeMasterVolumeFile( float db )
{
  File f = SPIFFS.open( MASTER_VOLUME_FILE, "w" );
  if( !f )
  {
    Serial.println( F("[ERROR] matrix: cannot write master volume file") );
    return;
  }
  f.print( String( db, 2 ) );
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
  model = new MatrixModel( port, matrixMeta.nbus, matrixMeta.nout, matrixMeta.nbiquad, matrixMeta.maxdly );
  masterVolume.val = readMasterVolumeFile();
  volumeSaver = new MasterVolumeSaver( masterVolume.val );
}

void matrixMasterVolumeChanged( void )
{
  if( !volumeSaver )
    return;
  ModelLock lock;
  volumeSaver->changed( masterVolume.val, millis() );
}

bool matrixBeginPresetSwitch( void )
{
  if( !model )
    return true;
  ModelLock lock;
  return model->beginPresetSwitch();
}

void matrixEndPresetSwitch( void )
{
  if( !model )
    return;
  ModelLock lock;
  model->endPresetSwitch();
}

std::string matrixCoeffs( const char* ch, int idx )
{
  if( !model )
    return "";
  ModelLock lock;
  return model->coeffsJson( ch, idx );
}

void matrixWriteBaseline( void )
{
  if( !model )
    return;
  std::vector<Op> ops;
  {
    ModelLock lock;
    port.ops.clear();
    model->writeBaseline();  // the DSP program was just loaded: write everything, not the difference
    ops.swap( port.ops );
    if( Settings.addonid == ADDON_B )  // updateAddOn() wrote it before the DSP program upload
      model->setAddOn( { true, currentAddOnCfg[0], currentAddOnCfg[1], currentAddOnCfg[2] } );
  }
  execOps( ops );
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
  // parsed before the lock (it does not touch the model), in place: json is not needed after
  MatrixModel::ParsedPreset file;
  const std::string err = json.empty() ? "no preset file" : model->parsePreset( &json[0], file );

  const MatrixAddOn addOn = readAddOnFile( currentPreset );  // not set: keep the current config

  std::vector<Op> ops;
  std::string oldJson;  // changes of the previous Preset that Autosave has not stored yet
  MatrixAddOn oldAddOn;
  uint8_t oldPreset;
  {
    ModelLock lock;
    oldPreset = model->preset();
    if( model->dirty() && !keepStored )
    {
      oldJson = model->savePreset();
      oldAddOn = model->addOn();
    }
    port.ops.clear();
    // only the difference, inside the shared ramp; a failed load writes nothing
    keepStored = false;
    if( err.empty() )
      model->switchPreset( file, &addOn );
    else
    {
      Serial.print( F("matrix: using defaults: ") );
      Serial.println( err.c_str() );
      model->switchToDefaults( &addOn );
      keepStored = !json.empty();  // an existing file stays as it is; edits are not autosaved
    }
    model->setPreset( currentPreset );
    ops.swap( port.ops );
  }
  if( !oldJson.empty() )
    savePreset( oldPreset, oldJson, oldAddOn );
  execOps( ops );
}

MatrixEditResult matrixApply( JsonObject patch, std::string& state )
{
  if( !model )
    return { MATRIX_EDIT_BAD_REQUEST, "not a matrix plugin" };
  // i2cPost() does not block, so the post stays under the lock: a refused post is undone
  // before anyone else sees the model, and posts keep the order of the model changes.
  ModelLock lock;
  port.ops.clear();
  MatrixEditResult result = model->apply( patch, millis() );
  if( !result.ok() )
    return result;
  std::vector<Op> ops;
  ops.swap( port.ops );
  if( !ops.empty() && !i2cPost( [ops]() { execOps( ops ); } ) )
  {
    model->undo();
    return { MATRIX_EDIT_BUSY, "busy" };
  }
  state = model->stateJson();  // under the same lock: the reply shows exactly this edit
  return result;
}

MatrixEditResult matrixImport( char* json, int preset )
{
  if( !model )
    return { MATRIX_EDIT_BAD_REQUEST, "not a matrix plugin" };
  // The costly parse needs no lock: the worker and loop() go on meanwhile.
  MatrixModel::ParsedPreset file;
  const std::string reason = model->parsePreset( json, file );
  if( !reason.empty() )
    return { MATRIX_EDIT_BAD_REQUEST, reason };
  if( Settings.addonid != ADDON_B )
    file.addOn = MatrixAddOn();  // no SPDIF mux to write the file's config to
  ModelLock lock;  // as matrixApply(): the post stays under the lock
  port.ops.clear();
  MatrixEditResult result = model->importPreset( file, preset, millis() );
  if( !result.ok() )
    return result;
  std::vector<Op> ops;
  ops.swap( port.ops );
  if( !ops.empty() && !i2cPost( [ops]() { execOps( ops ); } ) )
  {
    model->undo();
    return { MATRIX_EDIT_BUSY, "busy" };
  }
  keepStored = false;  // the active Preset is valid now: Autosave may overwrite its file
  return result;
}

MatrixEditResult matrixImportFilterFile( const char* ch, int idx, int preset, const char* text, size_t len,
                                         std::vector<std::string>& errors, std::string& state )
{
  errors.clear();
  if( !model )
  {
    errors.assign( 1, "not a matrix plugin" );
    return { MATRIX_EDIT_BAD_REQUEST, errors[0] };
  }
  // The costly check needs no lock (it reads only the model's fixed limits): the worker and
  // loop() go on meanwhile.
  FilterFileRow row;
  if( parseFilterFileFor( *model, ch, text, len, row, errors ) != MATRIX_EDIT_OK )
    return { MATRIX_EDIT_BAD_REQUEST, errors.empty() ? "" : errors[0] };
  ModelLock lock;  // as matrixApply(): the post stays under the lock
  port.ops.clear();
  MatrixEditResult result = applyFilterFile( *model, ch, idx, preset, row, millis() );
  if( result.ok() )
  {
    std::vector<Op> ops;
    ops.swap( port.ops );
    if( !ops.empty() && !i2cPost( [ops]() { execOps( ops ); } ) )
    {
      model->undo();
      result = { MATRIX_EDIT_BUSY, "busy" };
    }
  }
  if( !result.ok() )
  {
    errors.assign( 1, result.reason );
    return result;
  }
  state = model->stateJson();
  return result;
}

std::string matrixPresetJson( void )
{
  if( !model )
    return "";
  ModelLock lock;
  return model->savePreset();
}

MatrixEditResult matrixSetAddOn( uint8_t addr, uint8_t reg, uint8_t data )
{
  if( !model )
    return { MATRIX_EDIT_BAD_REQUEST, "not a matrix plugin" };
  StaticJsonDocument<128> doc;
  JsonObject patch = doc.to<JsonObject>();
  patch["ch"] = "addon";
  JsonArray i2c = patch.createNestedArray( "i2c" );
  i2c.add( addr );
  i2c.add( reg );
  i2c.add( data );
  std::string state;
  return matrixApply( patch, state );
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
  if( volumeSaver->saveDue( millis() ) )
  {
    float db = volumeSaver->value();
    if( i2cPost( [db]() { writeMasterVolumeFile( db ); } ) )
      volumeSaver->markSaved( db );
  }
  if( keepStored || !model->saveDue( millis() ) )
    return;
  std::string json = model->savePreset();
  int preset = model->preset();
  MatrixAddOn addOn = model->addOn();
  // only a queued save counts as saved; otherwise it is retried on the next call
  if( i2cPost( [json, preset, addOn]() { savePreset( preset, json, addOn ); } ) )
    model->markSaved();
}
