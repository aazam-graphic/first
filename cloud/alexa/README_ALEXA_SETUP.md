# AZAM CAR × ALEXA — Setup & Provisioning Guide

Firmware side is DONE (`main/alexa_bridge.c`, `main/net_wifi.c`, Car OS
hooks). This file covers the cloud half + car provisioning. Blueprint
phases: A (skill skeleton) → B (cloud bridge) → C–K live one by one.

Single-S3 note: this board IS the car. No second ESP32, no ESP-NOW.
Topics stay exactly as the blueprint defines (`azamcar/cmd/<user>` …),
so the cloud below matches the blueprint §6/§7 word for word.

---

## 1. Quick local test (NO AWS, Phase B in 10 minutes)

Flash with a local broker to prove the full loop before touching AWS.

1. PC par Mosquitto chalao: `mosquitto -p 1883`
2. `main/alexa_config.h` me set karo:
   `#define ALEXA_DEFAULT_BROKER "mqtt://<PC-IP>:1883"`
3. Build + flash. TFT par har command ka banner dikhega.
4. Test command bhejo (PC se):
```json
mosquitto_pub -h localhost -t "azamcar/cmd/default" -m "{\"cmd\":\"SET_HEADLIGHT\",\"args\":{\"on\":true},\"source\":\"ALEXA\",\"userId\":\"default\",\"requestId\":\"t1\",\"heard\":\"turn on headlights\",\"timestamp\":1999999999,\"ttl\":5000}"
```
   (`timestamp` bada rakho taaki TTL check pass ho — ya SNTP sync ke baad
   real epoch bhejo.)
5. Jawab dekho: `mosquitto_sub -h localhost -t "azamcar/resp/default" -v`
   → `{"requestId":"t1","status":"DONE",...}` + TFT banner `✓ DONE`.
6. Blocked test: `{"cmd":"MOVE_FORWARD",...}` → `REJECTED/LOCKED`, kuch nahi hota.

## 2. Firmware provisioning (broker / user / certs)

Bridge default DISABLED hai (`ALEXA_DEFAULT_BROKER ""`) — zero traffic.

- Fast path: `alexa_config.h` edit + reflash (broker/user/thing).
- NVS override (namespace `alexa`): keys `broker`, `user`, `thing`,
  `profile` (u8). Blobs `ca`, `cert`, `key` = AWS IoT PEMs.
  (Setup screen se NVS write — provisioning UI roadmap me hai;
  tab tak config.h path use karo.)

## 3. AWS IoT Core (Phase B, TLS)

1. Thing banao: `azam-car` → certs download (device cert + private key +
   Amazon Root CA).
2. Policy (thing par attach):
```json
{
  "Version": "2012-10-17",
  "Statement": [
    { "Effect": "Allow", "Action": ["iot:Connect"], "Resource": "arn:aws:iot:<region>:<acct>:client/azam-car" },
    { "Effect": "Allow", "Action": ["iot:Publish"], "Resource": "arn:aws:iot:<region>:<acct>:topic/azamcar/resp/*" },
    { "Effect": "Allow", "Action": ["iot:Publish"], "Resource": "arn:aws:iot:<region>:<acct>:topic/azamcar/event/*" },
    { "Effect": "Allow", "Action": ["iot:Publish"], "Resource": "arn:aws:iot:<region>:<acct>:topic/azamcar/state/*" },
    { "Effect": "Allow", "Action": ["iot:Publish"], "Resource": "arn:aws:iot:<region>:<acct>:topic/$aws/things/azam-car/shadow/update" },
    { "Effect": "Allow", "Action": ["iot:Subscribe", "iot:Receive"], "Resource": "arn:aws:iot:<region>:<acct>:topicfilter/azamcar/cmd/*" }
  ]
}
```
3. PEMs car me dalo (NVS blobs `ca`/`cert`/`key`), broker =
   `mqtts://<endpoint>.iot.<region>.amazonaws.com:8883`.
4. SNTP time auto-sync hota hai (TLS cert validation ke liye zaroori).

## 4. Lambda (Phase A/C)

1. `cd cloud/alexa && npm install && npm run zip`
2. Lambda (Node 20) banao, `function.zip` upload.
3. Env vars: `IOT_ENDPOINT`, `USER_ID=default`,
   `THING_NAME=azam-car`, `WAIT_MS=1800`.
4. Role: `iot:Publish` on `azamcar/cmd/*`, `iot:GetThingShadow` on thing.
5. Alexa Developer Console → Custom skill (invocation `azam car`,
   locale en-IN) → model = `interactionModel_en-IN.json` →
   endpoint = Lambda ARN. Phir Smart Home skill → same Lambda,
   discovery = `smartHomeDiscovery.json` ke endpoints.

## 5. Routines + announcements (Phase H/I)

- Time/location routines: Alexa app me banao (targets = car lights/profile).
- Device-state routines: car `azamcar/state/<user>` (retained) + shadow
  par proactive report karta hai — Lambda ChangeReport wiring Phase H me.
- Echo announcements: IoT rule `SELECT * FROM 'azamcar/event/default'` →
  `lambda_event.js` → (Phase I) Alexa Proactive Events API.
  Anti-spam (60 s / 5-min repeat) firmware me already hai.

## 6. Rules (Phase J)

`MAKE_RULE` sirf 2 triggers samajhta hai: obstacle→flash headlights,
idle→dim TFT. `LIST_RULES` / `RULE_ENABLE` live hain. `RULE_DELETE`
( sab ) confirm-gated hai (car par A dabao).

## 7. Security model (blueprint §14)

Lambda: ASK SDK signature verify + applicationId check (console me
endpoint config me). MQTT: AWS IoT certs, per-user topics. Firmware:
userId match, 5 s TTL, requestId dedupe-16, allow-list only,
E-stop sab actions block, motors ko koi command nahi pahunchta
(dispatch me motor path hai hi nahi — compile-time guarantee).

## 8. Acceptance mapping (blueprint §16)

| Check | Status |
|---|---|
| status query → live answer | ✅ firmware + Lambda ready |
| headlights < 3 s | ✅ relay via Car OS path |
| TFT banner + history-20 | ✅ `alexa_banner_draw` + `OS_ALEXA_LOG` |
| auto/gear/cal/game/save → A/B dialog | ✅ `alexa_confirm_*` |
| blocked → polite refusal | ✅ LOCKED double layer |
| offline → clean error | ✅ "not reachable", no crash |
| XiaoZhi conflict | N/A (chatbot removed; priority SAFETY > ALEXA) |
| 11 PM park routine | ✅ via Smart Home profile endpoint |
| hazard → Echo announce | ✅ event topic + lambda_event |
| E-stop wins | ✅ gateway rejects, alert logged |
| replay rejected | ✅ dedupe + TTL |
| 1-hour stability | ⏳ flash karke monitor par verify karo |
