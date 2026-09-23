/* SPDX-License-Identifier: Apache-2.0 */
/* alexa_config.h - build-time defaults for the Alexa cloud link.
 *
 * Single-S3 note: this board IS the car (Xbox dongle via USB host, TFT,
 * sensors all local). The blueprint's controller-S3/car-S3 split collapses:
 * MQTT (cloud) -> alexa_bridge (safety gateway) -> Car OS directly.
 * No ESP-NOW, no second S3 needed. No Echo hardware is ever touched.
 *
 * Runtime override: NVS namespace "alexa" keys broker/user/thing/profile.
 * TLS client certs (AWS IoT): NVS blobs ca/cert/key (written by the
 * provisioning screen / tooling). Empty broker = bridge disabled (zero
 * network traffic, zero RAM impact beyond a sleeping task).
 */
#pragma once

/* MQTT broker URI. "" = disabled. Local test: "mqtt://192.168.1.50:1883"
 * AWS IoT Core: "mqtts://<endpoint>.iot.<region>.amazonaws.com:8883" */
#define ALEXA_DEFAULT_BROKER  "mqtts://a2n0b2oir0907t-ats.iot.ap-south-1.amazonaws.com:8883"

/* Topic scope: azamcar/cmd/<user> etc. Must match the Lambda skill config. */
#define ALEXA_DEFAULT_USER    "default"

/* AWS IoT Thing name for Device Shadow updates ($aws/things/<thing>/...). */
#define ALEXA_DEFAULT_THING   "azam-car"

/* Amazon Root CA 1 (needed for TLS verification of AWS IoT endpoint). */
#define ALEXA_DEFAULT_CA \
    "-----BEGIN CERTIFICATE-----\n" \
    "MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF\n" \
    "ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6\n" \
    "b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL\n" \
    "MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv\n" \
    "b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj\n" \
    "ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM\n" \
    "9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw\n" \
    "IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6\n" \
    "VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L\n" \
    "93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm\n" \
    "jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC\n" \
    "AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA\n" \
    "A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI\n" \
    "U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs\n" \
    "N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv\n" \
    "o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU\n" \
    "5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy\n" \
    "rqXRfboQnoZsG4q5WTP468SQvvG5\n" \
    "-----END CERTIFICATE-----\n"

/* Device certificate + private key (AWS IoT mutual TLS).
 * SECURITY: NEVER commit real device certs/keys to git.
 * Provision via NVS blobs ca/cert/key (namespace "alexa") using
 * cloud/alexa/write_pems_nvs.py. Empty = bridge runs only when
 * NVS is provisioned. */
#define ALEXA_DEFAULT_CERT ""
#define ALEXA_DEFAULT_KEY ""
