// /config/zigbee2mqtt/external_converters/shs01.js
'use strict';

import * as fz from 'zigbee-herdsman-converters/converters/fromZigbee';
import * as tz from 'zigbee-herdsman-converters/converters/toZigbee';
import * as exposes from 'zigbee-herdsman-converters/lib/exposes';
import * as reporting from 'zigbee-herdsman-converters/lib/reporting';

const e = exposes.presets;
const ea = exposes.access;

const EP1 = 1;                   // light + config
const EP2 = 2;                   // occupancy (overall + moving/static)

const CL_CFG = 0xFDCD;           // custom config (EP1)
const CL_OCC = 0x0406;           // msOccupancySensing (EP2)

const ATTR_MOVEMENT_COOLDOWN = 0x0001;
const ATTR_OCC_CLEAR_COOLDOWN = 0x0002;
const ATTR_MOVING_SENS_0_10   = 0x0003;
const ATTR_STATIC_SENS_0_10   = 0x0004;
const ATTR_MOVING_MAX_GATE    = 0x0005;
const ATTR_STATIC_MAX_GATE    = 0x0006;

const ATTR_MOVING_TARGET = 0xF001; // mfg bool in CL_OCC (EP2)
const ATTR_STATIC_TARGET = 0xF002; // mfg bool in CL_OCC (EP2)

const U16 = 0x21, BOOL_DT = 0x10;
const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, Number(v)));
const M_PER_GATE = 0.75;
const gateToM = (g) => Number((Math.max(0, Math.min(8, Number(g))) * M_PER_GATE).toFixed(2));
const mToGateMv = (m) => Math.max(0, Math.min(8, Math.round(Number(m) / M_PER_GATE)));
const mToGateSt = (m) => Math.max(2, Math.min(8, Math.round(Number(m) / M_PER_GATE)));

const fzLocal = {
  occ_ep2: {
    cluster: 'msOccupancySensing',
    type: ['attributeReport', 'readResponse'],
    convert: (_model, msg) => {
      if (msg.endpoint?.ID !== EP2) return {};
      const d = msg.data || {}, out = {};
      const occ = d.occupancy ?? d['0'];
      if (occ !== undefined) out['occupancy'] = (typeof occ === 'number') ? ((occ & 1) === 1) : !!occ;
      const mv = d[ATTR_MOVING_TARGET] ?? d[String(ATTR_MOVING_TARGET)];
      const st = d[ATTR_STATIC_TARGET] ?? d[String(ATTR_STATIC_TARGET)];
      if (mv !== undefined) out['moving_target'] = (mv === true || mv === 1);
      if (st !== undefined) out['static_target'] = (st === true || st === 1);
      return out;
    },
  },
  cfg_ep1: {
    cluster: CL_CFG,
    type: ['readResponse', 'attributeReport'],
    convert: (_model, msg) => {
      if (msg.endpoint?.ID !== EP1) return {};
      const d = msg.data || {}, out = {};
      if (d[ATTR_MOVEMENT_COOLDOWN]   !== undefined) out['movement_clear_cooldown']       = d[ATTR_MOVEMENT_COOLDOWN];
      if (d[ATTR_OCC_CLEAR_COOLDOWN]  !== undefined) out['occupancy_clear_cooldown']       = d[ATTR_OCC_CLEAR_COOLDOWN];
      if (d[ATTR_MOVING_SENS_0_10]    !== undefined) out['movement_detection_sensitivity']      = d[ATTR_MOVING_SENS_0_10];
      if (d[ATTR_STATIC_SENS_0_10]    !== undefined) out['occupancy_detection_sensitivity']        = d[ATTR_STATIC_SENS_0_10];
      if (d[ATTR_MOVING_MAX_GATE]     !== undefined) out['movement_detection_range']            = gateToM(d[ATTR_MOVING_MAX_GATE]);
      if (d[ATTR_STATIC_MAX_GATE]     !== undefined) out['occupancy_detection_range']              = gateToM(d[ATTR_STATIC_MAX_GATE]);
      return out;
    },
  },
};

const tzLocal = {
  _ep1: (meta) => meta.device.getEndpoint(EP1),
  'movement_clear_cooldown': {
    key: ['movement_clear_cooldown'],
    convertSet: async (_e, _k, v, meta) => {
      const sec = clamp(v, 0, 300);
      await tzLocal._ep1(meta).write(CL_CFG, { [ATTR_MOVEMENT_COOLDOWN]: { value: sec, type: U16 } });
      return { state: { 'movement_clear_cooldown': sec } };
    },
    convertGet: async (_e, _k, meta) => tzLocal._ep1(meta).read(CL_CFG, [ATTR_MOVEMENT_COOLDOWN]),
  },
  'occupancy_clear_cooldown': {
    key: ['occupancy_clear_cooldown'],
    convertSet: async (_e, _k, v, meta) => {
      const sec = clamp(v, 0, 65535);
      await tzLocal._ep1(meta).write(CL_CFG, { [ATTR_OCC_CLEAR_COOLDOWN]: { value: sec, type: U16 } });
      return { state: { 'occupancy_clear_cooldown': sec } };
    },
    convertGet: async (_e, _k, meta) => tzLocal._ep1(meta).read(CL_CFG, [ATTR_OCC_CLEAR_COOLDOWN]),
  },
  'movement_detection_sensitivity': {
    key: ['movement_detection_sensitivity'],
    convertSet: async (_e, _k, v, meta) => {
      const val = clamp(v, 0, 10);
      await tzLocal._ep1(meta).write(CL_CFG, { [ATTR_MOVING_SENS_0_10]: { value: val, type: U16 } });
      return { state: { 'movement_detection_sensitivity': val } };
    },
    convertGet: async (_e, _k, meta) => tzLocal._ep1(meta).read(CL_CFG, [ATTR_MOVING_SENS_0_10]),
  },
  'occupancy_detection_sensitivity': {
    key: ['occupancy_detection_sensitivity'],
    convertSet: async (_e, _k, v, meta) => {
      const val = clamp(v, 0, 10);
      await tzLocal._ep1(meta).write(CL_CFG, { [ATTR_STATIC_SENS_0_10]: { value: val, type: U16 } });
      return { state: { 'occupancy_detection_sensitivity': val } };
    },
    convertGet: async (_e, _k, meta) => tzLocal._ep1(meta).read(CL_CFG, [ATTR_STATIC_SENS_0_10]),
  },
  'movement_detection_range': {
    key: ['movement_detection_range'],
    convertSet: async (_e, _k, v, meta) => {
      const gate = mToGateMv(clamp(v, 0.0, 6.0));
      await tzLocal._ep1(meta).write(CL_CFG, { [ATTR_MOVING_MAX_GATE]: { value: gate, type: U16 } });
      return { state: { 'movement_detection_range': gateToM(gate) } };
    },
    convertGet: async (_e, _k, meta) => tzLocal._ep1(meta).read(CL_CFG, [ATTR_MOVING_MAX_GATE]),
  },
  'occupancy_detection_range': {
    key: ['occupancy_detection_range'],
    convertSet: async (_e, _k, v, meta) => {
      const gate = mToGateSt(clamp(v, 0.75, 6.0));
      await tzLocal._ep1(meta).write(CL_CFG, { [ATTR_STATIC_MAX_GATE]: { value: gate, type: U16 } });
      return { state: { 'occupancy_detection_range': gateToM(gate) } };
    },
    convertGet: async (_e, _k, meta) => tzLocal._ep1(meta).read(CL_CFG, [ATTR_STATIC_MAX_GATE]),
  },
};

export default [{
  serverModuleFormat: 'cjs',
  fingerprint: [{modelID: 'SHS01', manufacturerName: 'SmartHomeScene'}],
  model: 'SHS01',
  icon: 'http://zigbee2mqtt.ourhome.co.za:8180/device_icons/ld2410.jpg',
  vendor: 'SmartHomeScene',
  description: 'ESP32-C6 LD2410C: light + Moving/Static/Occupancy + config (EP1/EP2)',
  meta: {configureKey: 31, multiEndpoint: true},

  // Only numeric endpoints come from the device itself (1, 2, 242)

  fromZigbee: [
    fz.on_off,        // EP1
    fzLocal.occ_ep2,  // EP2
    fzLocal.cfg_ep1,  // EP1 config readback
  ],
  toZigbee: [
    tz.on_off,                              // EP1
    tzLocal['movement_clear_cooldown'],
    tzLocal['occupancy_clear_cooldown'],
    tzLocal['movement_detection_sensitivity'],
    tzLocal['occupancy_detection_sensitivity'],
    tzLocal['movement_detection_range'],
    tzLocal['occupancy_detection_range'],
  ],

  exposes: [
    e.light(),
    e.binary('moving_target', ea.STATE, true, false).withDescription("Indicates whether the device detected movement"),
    e.binary('static_target', ea.STATE, true, false).withDescription("Indicates whether the device detected peace"),
    e.occupancy(),
    exposes.numeric('movement_clear_cooldown', ea.ALL).withUnit('s').withCategory("config").withValueMin(0).withValueMax(300).withDescription("Movement clear time"),
    exposes.numeric('occupancy_clear_cooldown', ea.ALL).withUnit('s').withCategory("config").withValueMin(0).withValueMax(65535).withDescription("Occupancy clear time"),
    exposes.numeric('movement_detection_sensitivity', ea.ALL).withCategory("config").withValueMin(0).withValueMax(10).withDescription("Movement detection sensitivity"),
    exposes.numeric('occupancy_detection_sensitivity', ea.ALL).withCategory("config").withValueMin(0).withValueMax(10).withDescription("Occupancy detection sensitivity"),
    exposes.numeric('movement_detection_range', ea.ALL).withUnit('m').withCategory("config").withValueMin(0.0).withValueMax(6.0).withValueStep(0.75).withDescription("Movement detection range distance"),
    exposes.numeric('occupancy_detection_range', ea.ALL).withUnit('m').withCategory("config").withValueMin(0.75).withValueMax(6.0).withValueStep(0.75).withDescription("Occupancy detection range distance"),

  ],

  configure: async (device, coordinatorEndpoint) => {
    const ep1 = device.getEndpoint(EP1);
    const ep2 = device.getEndpoint(EP2);

    await reporting.bind(ep1, coordinatorEndpoint, ['genOnOff']);
    await reporting.bind(ep2, coordinatorEndpoint, ['msOccupancySensing']);

    try { await ep1.configureReporting('genOnOff', [{attribute: 'onOff', minimumReportInterval: 0, maximumReportInterval: 3600}]); }
    catch { try { await reporting.onOff(ep1); } catch {} }
    try { await ep1.read('genOnOff', ['onOff']); } catch {}

    try { await ep2.configureReporting('msOccupancySensing', [{attribute: 'occupancy', minimumReportInterval: 0, maximumReportInterval: 3600, reportableChange: 0}]); }
    catch { try { await reporting.occupancy(ep2, {min: 0, max: 3600, change: 0}); } catch {} }

    const repCustom = [
      {attribute: ATTR_MOVING_TARGET, minimumReportInterval: 0, maximumReportInterval: 3600, reportableChange: 1, dataType: BOOL_DT},
      {attribute: ATTR_STATIC_TARGET, minimumReportInterval: 0, maximumReportInterval: 3600, reportableChange: 1, dataType: BOOL_DT},
    ];
    try { await ep2.configureReporting(CL_OCC, repCustom, {manufacturerCode: 0x115F}); }
    catch { try { await ep2.configureReporting(CL_OCC, repCustom); } catch {} }

    try { await ep2.read('msOccupancySensing', ['occupancy']); } catch {}
    try { await ep2.read(CL_OCC, [ATTR_MOVING_TARGET, ATTR_STATIC_TARGET]); } catch {}

    try {
      await ep1.read(CL_CFG, [
        ATTR_MOVEMENT_COOLDOWN, ATTR_OCC_CLEAR_COOLDOWN,
        ATTR_MOVING_SENS_0_10, ATTR_STATIC_SENS_0_10,
        ATTR_MOVING_MAX_GATE, ATTR_STATIC_MAX_GATE,
      ]);
    } catch {}
  },
}];