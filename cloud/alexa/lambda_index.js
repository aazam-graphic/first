/* Azam Car x Alexa - Lambda backend (Custom Skill + Smart Home Skill).
 *
 * Flow: Alexa intent/directive -> build car command -> MQTT publish to
 *   azamcar/cmd/<USER_ID> -> car executes via safety gateway -> car updates
 *   Device Shadow -> Lambda reads shadow -> spoken response.
 * Works with firmware: main/alexa_bridge.c (single-S3, no ESP-NOW needed).
 *
 * Env vars: IOT_ENDPOINT (e.g. abc123-ats.iot.ap-south-1.amazonaws.com)
 *           USER_ID      (must match firmware NVS "alexa"/"user")
 *           THING_NAME   (default "azam-car")
 *           WAIT_MS      (default 1800, shadow poll delay)
 *
 * Deploy: npm install && zip, upload to Lambda (Node.js 20.x),
 * attach a role with iot:Publish + iot:GetThingShadow on the thing.
 */
'use strict';
const Alexa = require('ask-sdk-core');
const { IoTDataPlaneClient, PublishCommand, GetThingShadowCommand } =
  require('@aws-sdk/client-iot-data-plane');

const IOT_ENDPOINT = process.env.IOT_ENDPOINT;
const USER_ID = process.env.USER_ID || 'default';
const THING = process.env.THING_NAME || 'azam-car';
const WAIT_MS = parseInt(process.env.WAIT_MS || '1800', 10);
const iot = new IoTDataPlaneClient({ endpoint: `https://${IOT_ENDPOINT}` });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const uid = () => `${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;

async function sendCmd(cmd, args, heard) {
  const requestId = uid();
  const t0 = Date.now();
  const payload = JSON.stringify({
    cmd, args: args || {}, source: 'ALEXA',
    userId: USER_ID, requestId, heard: heard || cmd,
    timestamp: Math.floor(Date.now() / 1000), ttl: 5000,
  });
  await iot.send(new PublishCommand({
    topic: `azamcar/cmd/${USER_ID}`, payload: Buffer.from(payload), qos: 1,
  }));
  await sleep(WAIT_MS);
  try {
    const sh = await iot.send(new GetThingShadowCommand({ thingName: THING }));
    const doc = JSON.parse(Buffer.from(sh.payload).toString());
    const ts = doc.timestamp ? doc.timestamp * 1000 : 0;
    return { requestId, reported: (doc.state && doc.state.reported) || null, fresh: ts >= t0 - 5000 };
  } catch (e) {
    return { requestId, reported: null, fresh: false }; // car offline / no shadow yet
  }
}

/* Confirm-gated firmware cmds: no immediate state change, car shows A/B dialog. */
const CONFIRM_CMDS = ['SET_AUTO', 'SET_GEAR', 'CALIBRATE', 'START_GAME', 'SAVE_SETTINGS', 'RULE_DELETE'];

/* Per-command state verify: (args, reported) -> speech or null (not verifiable). */
const EXPECT = {
  SET_HAZARD: (a, r) => (typeof r.hazard === 'boolean' && r.hazard === !!a.on ? (a.on ? 'Hazard lights on.' : 'Hazard lights off.') : null),
  SET_HEADLIGHT: (a, r) => (typeof r.headlight === 'boolean' && r.headlight === !!a.on ? (a.on ? 'Headlights on.' : 'Headlights off.') : null),
  SET_TFT_BRIGHT: (a, r) => (r.tft_bright === a.pct ? `Brightness ${a.pct} percent.` : null),
  SET_VOLUME: (a, r) => (r.volume === a.pct ? `Volume ${a.pct} percent.` : null),
  SET_MUTE: (a, r) => (typeof r.muted === 'boolean' && r.muted === !!a.on ? (a.on ? 'Car muted.' : 'Car unmuted.') : null),
  SET_PROFILE: (a, r) => (String(r.profile || '').toLowerCase() === String(a.profile || '').toLowerCase() ? `${a.profile} profile on.` : null),
  SET_ROOF_BRIGHT: (a, r) => (r.roof_bright === a.pct ? `Roof brightness ${a.pct} percent.` : null),
  ALL_LIGHTS_OFF: (a, r) => (!r.headlight && !r.hazard && !r.roof_on ? 'All lights off.' : null),
};

/* Shared verify for custom + smart-home paths. */
function verifyAction(rep, fresh, cmd, args) {
  if (!rep) return { ok: false, speech: 'The car is not reachable. Please check it is powered on and connected to Wi-Fi.' };
  const ex = EXPECT[cmd];
  if (ex) { const s = ex(args || {}, rep); if (s) return { ok: true, speech: s }; }
  if (rep.estop) return { ok: false, speech: 'The car is in emergency stop. Clear it on the controller first.' };
  if (CONFIRM_CMDS.includes(cmd)) {
    return fresh ? { ok: false, speech: 'Please confirm on the car controller. Press A to confirm, B to cancel.' }
                 : { ok: false, speech: 'The car is not reachable. Please check it is powered on and connected to Wi-Fi.' };
  }
  if (fresh) return { ok: true, speech: 'Done.' };
  return { ok: false, speech: 'The car did not respond. Please check it is online and try again.' };
}

/* Action ack via fresh shadow state (firmware shadow carries no cmdId). */
function actionAck(rep, fresh, cmd, args) {
  return verifyAction(rep, fresh, cmd, args).speech;
}

function cmdAck(rep, requestId, okText) {
  if (!rep) return 'The car is not reachable. Please check it is powered on and connected to Wi-Fi.';
  if (rep.cmdId !== requestId) return 'Request sent. Please check the car screen to confirm.';
  if (rep.cmdResult === 'DONE') return okText;
  if (rep.cmdResult === 'NEEDS_CONFIRM') return 'Please confirm on the car controller. Press A to confirm, B to cancel.';
  const why = {
    ESTOP: 'The car is in emergency stop. Clear it on the controller first.',
    CAR_MOVING: 'That needs the car parked and stationary.',
    LOCKED: 'That action is locked for safety.',
    EXPIRED: 'The command expired. Please try again.',
    BUSY: 'The car is busy. Please try again.',
    UNKNOWN: 'The car did not understand that command.',
  }[rep.cmdReason] || 'The car rejected the command.';
  return why;
}

const cm = (s) => (s === -1 ? 'clear' : `${s} centimetres`);
function phraseStatus(r) {
  if (!r) return 'The car is not reachable.';
  const move = r.moving ? 'moving' : 'stationary';
  return `Parked status: ${r.mode} mode, ${r.profile} profile, gear ${r.gear}, ${move}. ` +
    `Front ${cm(r.front_cm)}, left ${cm(r.left_cm)}, right ${cm(r.right_cm)}. ` +
    (r.estop ? 'Emergency stop is active. ' : 'No emergency stop. ') +
    `Headlight ${r.headlight ? 'on' : 'off'}, hazard ${r.hazard ? 'on' : 'off'}.`;
}
function phraseZone(r, side) {
  if (!r) return 'The car is not reachable.';
  const v = side === 'left' ? r.left_cm : side === 'right' ? r.right_cm : r.front_cm;
  if (v === -1) return `The ${side} side is clear.`;
  if (v < r.obstacle_cm) return `The ${side} side is blocked, object at ${v} centimetres.`;
  return `The ${side} side is clear, nearest object at ${v} centimetres.`;
}

// ---- intent -> car command map (firmware vocabulary, alexa_bridge.c) ----
const INTENT_CMDS = {
  CarStatusIntent: () => ['STATUS', {}],
  ZoneQueryIntent: (s) => ['ZONE', {}], // answered from shadow, side in speech
  ModeQueryIntent: () => ['MODE_Q', {}],
  MotionQueryIntent: () => ['MOTION_Q', {}],
  CalibrationQueryIntent: () => ['STATUS', {}],
  SensorHealthIntent: () => ['SENSOR_HEALTH', {}],
  LinkQueryIntent: () => ['LINK_Q', {}],
  LastAlertIntent: () => ['LAST_ALERT', {}],
  DayReportIntent: () => ['DAY_REPORT', {}],
  MaintenanceIntent: () => ['MAINTENANCE', {}],
  MemStatusIntent: () => ['MEM_STATUS', {}],
  SessionQueryIntent: () => ['SESSION_Q', {}],
  OpenScreenIntent: (s) => ['OPEN_SCREEN', { screen: screenSlot(s) }],
  SetBrightnessIntent: (s) => ['SET_TFT_BRIGHT', { pct: numSlot(s, 'pct', 80) }],
  DisplayThemeIntent: (s) => ['SET_THEME', { theme: themeSlot(s) }],
  SetVolumeIntent: (s) => ['SET_VOLUME', { pct: numSlot(s, 'pct', 50) }],
  MuteIntent: () => ['SET_MUTE', { on: true }],
  UnmuteIntent: () => ['SET_MUTE', { on: false }],
  SetProfileIntent: (s) => ['SET_PROFILE', { profile: profileSlot(s) }],
  AutoModeIntent: (s) => ['SET_AUTO', { on: true }],
  SetGearIntent: (s) => ['SET_GEAR', { gear: numSlot(s, 'gear', 1) }],
  CalibrateIntent: () => ['CALIBRATE', {}],
  StartGameIntent: (s) => ['START_GAME', { game: numSlot(s, 'game', 0) }],
  SaveSettingsIntent: () => ['SAVE_SETTINGS', {}],
  MakeRuleIntent: (s) => ['MAKE_RULE', ruleSlots(s)],
  ListRulesIntent: () => ['LIST_RULES', {}],
  EnableRuleIntent: (s) => ['RULE_ENABLE', { id: numSlot(s, 'id', 0), on: true }],
  DisableRuleIntent: (s) => ['RULE_ENABLE', { id: numSlot(s, 'id', 0), on: false }],
  DeleteRulesIntent: () => ['RULE_DELETE', { all: true }],
  HeadlightIntent: (s) => ['SET_HEADLIGHT', { on: onSlot(s) }],
  HazardIntent: (s) => ['SET_HAZARD', { on: onSlot(s) }],
  RoofColorIntent: (s) => ['SET_ROOF_COLOR', { color: strSlot(s, 'color', 'blue') }],
  RoofModeIntent: (s) => ['SET_ROOF_MODE', { mode: strSlot(s, 'mode', 'steady') }],
  RoofBrightnessIntent: (s) => ['SET_ROOF_BRIGHT', { pct: numSlot(s, 'pct', 50) }],
  AllLightsOffIntent: () => ['ALL_LIGHTS_OFF', {}],
  WarnLightsIntent: () => ['WARN_BURST', {}],
  FreeCommandIntent: (s) => freeCmd(s),
};
// ---- slot helpers ----
const slot = (s, n) => s && s[n] && s[n].value;
const strSlot = (s, n, d) => { const v = slot(s, n); return v ? String(v).toLowerCase() : d; };
const numSlot = (s, n, d) => { const v = slot(s, n); const p = parseInt(v, 10); return Number.isNaN(p) ? d : p; };
const onSlot = (s) => /on|enable|start|yes/i.test(String(slot(s, 'state') || 'on'));

/* Free-form query -> safest matching car command (never movement). */
function freeCmd(s) {
  const q = String((s && s.query && s.query.value) || '').toLowerCase();
  const off = /off|band|bujhao|stop|disable/.test(q);
  const num = (() => { const m = q.match(/\d+/); return m ? parseInt(m[0], 10) : null; })();
  if (/hazard/.test(q)) return ['SET_HAZARD', { on: !off }];
  if (/headlight|head light|light/.test(q)) return ['SET_HEADLIGHT', { on: !off }];
  if (/mute|awaaz|silent|quiet/.test(q)) return ['SET_MUTE', { on: !/unmute/.test(q) }];
  if (/volume|awaaz tez|loud/.test(q)) return ['SET_VOLUME', { pct: num !== null ? num : 50 }];
  if (/status|report|haal|condition/.test(q)) return ['STATUS', {}];
  return ['STATUS', {}]; // safe readonly fallback
}
function screenSlot(s) {
  const v = strSlot(s, 'screen', 'drive');
  if (/radar/.test(v)) return 'radar';
  if (/diag/.test(v)) return 'diagnostics';
  if (/game/.test(v)) return 'games';
  if (/voice|history|log/.test(v)) return 'voice_history';
  if (/setting/.test(v)) return 'settings';
  if (/home/.test(v)) return 'home';
  return 'drive';
}
function themeSlot(s) { return /night|dark/.test(strSlot(s, 'theme', 'day')) ? 'night' : 'day'; }
function profileSlot(s) {
  const v = strSlot(s, 'profile', 'safe');
  for (const p of ['park', 'safe', 'night', 'demo', 'performance', 'silent', 'game'])
    if (v.includes(p)) return p;
  return 'safe';
}
function ruleSlots(s) {
  const t = strSlot(s, 'ruleText', '');
  return {
    trigger: /idle/.test(t) ? 'idle' : 'obstacle',
    action: /dim/.test(t) ? 'dim' : 'flash',
  };
}

async function handleCustom(handlerInput, buildCmd, speak) {
  const slots = handlerInput.requestEnvelope.request.intent.slots || {};
  const [cmd, args] = buildCmd(slots);
  const heard = handlerInput.requestEnvelope.request.intent.name;
  const { requestId, reported, fresh } = await sendCmd(cmd, args, heard);
  return speak(handlerInput, requestId, reported, slots, fresh, cmd, args);
}

const CustomHandler = {
  canHandle(h) {
    const r = h.requestEnvelope.request;
    return r.type === 'IntentRequest' && INTENT_CMDS[r.intent.name];
  },
  async handle(h) {
    const name = h.requestEnvelope.request.intent.name;
    const build = INTENT_CMDS[name];
    const done = (speech) => h.responseBuilder.speak(speech).getResponse();
    switch (name) {
      case 'CarStatusIntent': return handleCustom(h, build, (hh, id, r) => done(phraseStatus(r)));
      case 'ZoneQueryIntent': return handleCustom(h, build, (hh, id, r, s) => done(phraseZone(r, strSlot(s, 'side', 'front'))));
      case 'ModeQueryIntent': return handleCustom(h, build, (hh, id, r) => done(r ? `${r.mode} mode, ${r.profile} profile, gear ${r.gear}.` : 'The car is not reachable.'));
      case 'MotionQueryIntent': return handleCustom(h, build, (hh, id, r) => done(r ? (r.moving ? 'The car is moving.' : 'The car is stationary.') : 'The car is not reachable.'));
      case 'CalibrationQueryIntent': return handleCustom(h, build, (hh, id, r) => done(!r ? 'The car is not reachable.' : (r.imu_cal === 2 ? 'Motion sensor is calibrated.' : 'Motion sensor calibration is due.')));
      case 'SensorHealthIntent': return handleCustom(h, build, (hh, id, r) => done(!r ? 'The car is not reachable.' : (r.front_sensor_ok && r.imu_valid ? `All sensors OK. Left, front, right clear readings, motion ${r.imu_valid ? 'valid' : 'invalid'}.` : 'A sensor needs attention. Check the car diagnostics screen.')));
      case 'LinkQueryIntent': return handleCustom(h, build, (hh, id, r) => done(!r ? 'The car is not reachable.' : (r.pad_connected ? 'Xbox controller connected.' : 'Controller is disconnected.')));
      case 'LastAlertIntent': return handleCustom(h, build, (hh, id, r) => done(!r ? 'The car is not reachable.' : `Last alert: ${r.last_alert}.`));
      case 'DayReportIntent': return handleCustom(h, build, (hh, id, r) => done(!r ? 'The car is not reachable.' : `Today: ${r.cnt_front_stop} front stops, ${r.cnt_pad_drop} controller drops, ${r.cnt_estop} emergency stops, ${r.cnt_stuck} stuck events.`));
      case 'MaintenanceIntent': return handleCustom(h, build, (hh, id, r) => done(!r ? 'The car is not reachable.' : ((r.cal_due ? 'Motion sensor calibration is due. ' : 'Calibration OK. ') + `${r.cnt_stuck} stuck events logged.`)));
      case 'MemStatusIntent': return handleCustom(h, build, (hh, id, r) => done(!r ? 'The car is not reachable.' : `Memory ${(r.heap_free / 1024).toFixed(0)} kilobytes free, ${(r.psram_free / 1048576).toFixed(1)} megabytes PSRAM free. All systems normal.`));
      case 'SessionQueryIntent': return handleCustom(h, build, (hh, id, r) => done(!r ? 'The car is not reachable.' : `Last drive was ${r.last_drive_min} minutes. Uptime ${r.uptime_min} minutes.`));
      case 'ListRulesIntent': return handleCustom(h, build, (hh, id, r) => {
        if (!r || !r.rules || !r.rules.length) return done('No active rules.');
        return done(`${r.rules.length} active rules. ` + r.rules.map((x) => `Rule ${x.id}, trigger ${x.trigger === 1 ? 'idle' : 'obstacle'}, action ${x.action === 1 ? 'dim' : 'flash'}, ${x.enabled ? 'enabled' : 'disabled'}.`).join(' '));
      });
      default: return handleCustom(h, build, (hh, id, r, s, fresh, cmd, args) => done(actionAck(r, fresh, cmd, args)));
    }
  },
};

const LaunchHandler = {
  canHandle(h) { return h.requestEnvelope.request.type === 'LaunchRequest'; },
  handle(h) { return h.responseBuilder.speak('Azam Car ready. Ask for status, lights, profiles, or diagnostics.').reprompt('What should the car do?').getResponse(); },
};

// ---- Smart Home Skill ----
const ROOF_COLORS = { red: { r: 255, g: 0, b: 0 }, green: { r: 0, g: 255, b: 0 }, blue: { r: 0, g: 0, b: 255 }, yellow: { r: 255, g: 255, b: 0 }, magenta: { r: 255, g: 0, b: 255 }, white: { r: 255, g: 255, b: 255 }, cyan: { r: 0, g: 255, b: 255 } };
const SmartHomeHandler = {
  canHandle(h) { const r = h.requestEnvelope.request || {}; return (r.type || '').startsWith('Alexa.') || !!h.requestEnvelope.directive; },
  async handle(h) {
    const env = h.requestEnvelope || {};
    const dir = env.directive || (env.request && env.request.directive) || env.request || {};
    const hdr = dir.header || {};
    const pay = dir.payload || {};
    const ns = hdr.namespace;
    const name = hdr.name;
    const ep = (dir.endpoint || {}).endpointId;
    const corr = hdr.correlationToken;
    const msgId = hdr.messageId;
    const contextProps = (rep) => [
      { namespace: 'Alexa.EndpointHealth', name: 'connectivity', value: { value: rep ? 'OK' : 'UNREACHABLE' }, timeOfSample: new Date().toISOString(), uncertaintyInMilliseconds: 1000 },
    ];
    if (ns === 'Alexa.Discovery' && name === 'Discover') {
      return { event: require('./smartHomeDiscovery.json').event };
    }
    // endpoint -> firmware command
    let cmd = null, args = {}, prop = null;
    const power = pay.powerState === 'ON';
    if (ep === 'azam-car-headlights' && name === 'TurnOn') { cmd = 'SET_HEADLIGHT'; args = { on: true }; prop = { namespace: 'Alexa.PowerController', name: 'powerState', value: 'ON' }; }
    else if (ep === 'azam-car-headlights' && name === 'TurnOff') { cmd = 'SET_HEADLIGHT'; args = { on: false }; prop = { namespace: 'Alexa.PowerController', name: 'powerState', value: 'OFF' }; }
    else if (ep === 'azam-car-rearlight' && name === 'TurnOn') { cmd = 'SET_REAR'; args = { on: true }; prop = { namespace: 'Alexa.PowerController', name: 'powerState', value: 'ON' }; }
    else if (ep === 'azam-car-rearlight' && name === 'TurnOff') { cmd = 'SET_REAR'; args = { on: false }; prop = { namespace: 'Alexa.PowerController', name: 'powerState', value: 'OFF' }; }
    else if (ep === 'azam-car-hazards' && (name === 'TurnOn' || name === 'TurnOff')) { cmd = 'SET_HAZARD'; args = { on: name === 'TurnOn' }; prop = { namespace: 'Alexa.PowerController', name: 'powerState', value: name === 'TurnOn' ? 'ON' : 'OFF' }; }
    else if (ep === 'azam-car-rooflight' && name === 'TurnOn') { cmd = 'SET_ROOF_MODE'; args = { mode: 'steady' }; prop = { namespace: 'Alexa.PowerController', name: 'powerState', value: 'ON' }; }
    else if (ep === 'azam-car-rooflight' && name === 'TurnOff') { cmd = 'SET_ROOF_MODE'; args = { mode: 'off' }; prop = { namespace: 'Alexa.PowerController', name: 'powerState', value: 'OFF' }; }
    else if (ep === 'azam-car-rooflight' && name === 'SetBrightness') { cmd = 'SET_ROOF_BRIGHT'; args = { pct: pay.brightness }; prop = { namespace: 'Alexa.BrightnessController', name: 'brightness', value: args.pct }; }
    else if (ep === 'azam-car-rooflight' && name === 'SetColor') {
      const c = pay.color; let best = 'blue', bd = 1e9;
      for (const [k, v] of Object.entries(ROOF_COLORS)) { const d = Math.abs(v.r - c.red) + Math.abs(v.g - c.green) + Math.abs(v.b - c.blue); if (d < bd) { bd = d; best = k; } }
      cmd = 'SET_ROOF_COLOR'; args = { color: best };
      prop = { namespace: 'Alexa.ColorController', name: 'color', value: { hue: 0, saturation: 0, brightness: 1 } };
    }
    else if (ep === 'azam-car-rooflight' && name === 'SetMode') { const m = pay.mode.toLowerCase(); cmd = 'SET_ROOF_MODE'; args = { mode: m }; prop = { namespace: 'Alexa.ModeController', name: 'mode', value: m }; }
    else if (ep === 'azam-car-profile' && name === 'SetMode') { const m = pay.mode.toLowerCase(); cmd = 'SET_PROFILE'; args = { profile: m }; prop = { namespace: 'Alexa.ModeController', name: 'mode', value: m }; }
    else if (ep === 'azam-car-alllights' && name === 'TurnOff') { cmd = 'ALL_LIGHTS_OFF'; args = {}; prop = { namespace: 'Alexa.PowerController', name: 'powerState', value: 'OFF' }; }
    else if ((ep === 'azam-car-warning' || ep === 'azam-car-hazards') && name === 'TurnOn' && ep === 'azam-car-warning') { cmd = 'WARN_BURST'; args = {}; prop = { namespace: 'Alexa.PowerController', name: 'powerState', value: 'ON' }; }
    if (!cmd) {
      return { event: { header: { namespace: 'Alexa', name: 'ErrorResponse', messageId: msgId, payloadVersion: '3', correlationToken: corr }, payload: { type: 'INVALID_DIRECTIVE', message: 'Unsupported' } } };
    }
    const { requestId, reported, fresh } = await sendCmd(cmd, args, `${name} ${ep}`);
    const verdict = verifyAction(reported, fresh, cmd, args);
    if (!verdict.ok) {
      const bridgeErr = !reported ? 'BRIDGE_UNREACHABLE' : 'ENDPOINT_UNREACHABLE';
      return { event: { header: { namespace: 'Alexa', name: 'ErrorResponse', messageId: msgId, payloadVersion: '3', correlationToken: corr }, payload: { type: bridgeErr, message: verdict.speech } } };
    }
    return {
      context: { properties: [Object.assign({}, prop, { timeOfSample: new Date().toISOString(), uncertaintyInMilliseconds: 500 }), ...contextProps(reported)] },
      event: { header: { namespace: 'Alexa', name: 'Response', messageId: msgId, payloadVersion: '3', correlationToken: corr }, endpoint: { endpointId: ep }, payload: {} },
    };
  },
};

const ErrorHandler = {
  canHandle() { return true; },
  handle(h, err) {
    console.error(err);
    return h.responseBuilder.speak('Sorry, the car skill had a problem.').getResponse();
  },
};

const skill = Alexa.SkillBuilders.custom()
  .addRequestHandlers(LaunchHandler, CustomHandler)
  .addErrorHandlers(ErrorHandler);
const skillHandler = skill.lambda();

/* Dual-mode entry: Alexa.Discovery/PowerController directives are answered
 * directly (raw {event,context} envelope); custom intents go to ASK SDK. */
exports.handler = async (event, context, callback) => {
  if (event && event.directive) {
    try {
      const out = await SmartHomeHandler.handle({ requestEnvelope: event });
      callback(null, out);
    } catch (e) {
      console.error(e);
      const h = (event.directive && event.directive.header) || {};
      callback(null, { event: { header: { namespace: 'Alexa', name: 'ErrorResponse', messageId: h.messageId, payloadVersion: '3', correlationToken: h.correlationToken }, payload: { type: 'INTERNAL_ERROR', message: 'Skill error' } } });
    }
    return;
  }
  return skillHandler(event, context, callback);
};
