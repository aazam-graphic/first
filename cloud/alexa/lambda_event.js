/* Azam Car - event forwarder (Phase I announcements).
 *
 * AWS IoT rule SQL: SELECT * FROM 'azamcar/event/<USER_ID>'
 * Rule action: invoke this Lambda. It converts car critical events into
 * Alexa Proactive Events (requires skill messaging setup, see README).
 *
 * For quick testing without proactive-events approval, the same IoT rule can
 * instead publish to an SNS topic that sends you an SMS/push notification.
 */
'use strict';

const TEXTS = {
  estop: 'Attention. Azam Car reports an emergency stop.',
  pad_lost: 'Attention. Azam Car lost its controller link.',
  sensor_fault: 'Attention. Azam Car reports a front sensor fault.',
  stuck: 'Attention. Azam Car is stuck and needs help.',
  rule_fired: 'Azam Car automation fired.',
};

exports.handler = async (event) => {
  console.log('car event:', JSON.stringify(event));
  const text = TEXTS[event.type] || `Azam Car event: ${event.type}`;
  // TODO (Phase I): call Alexa Proactive Events API here
  // (needs apiEndpoint + access token from skill messaging).
  return { ok: true, announce: text };
};
