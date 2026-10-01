# axis-scene-mqtt-bridge

An ACAP application for Axis cameras that publishes scene metadata and events
as JSON to an MQTT broker — **only when something changes**.

Developed against an AXIS M4228-LVE (aarch64, AXIS OS 12.x).

| Source | MQTT topic | Published when |
|---|---|---|
| Object detection (class, clothing colors, ...) from the camera's scene description | `<prefix>/objects` | a track appears/disappears, its class/attributes change, or it moved noticeably (rate limited) |
| Audio events (speech, shout, glass break, level reached, ...) | `<prefix>/audio/<event topic>` | the camera reports the event |
| Motion events (VMD, object analytics, ...) | `<prefix>/motion/<event topic>` | the camera reports the event |
| Status | `<prefix>/status` | `online` / `offline` (retained, last will) |

`<prefix>` defaults to `axis/<serial number>/bridge`.

## How "only on change" works

* **Events (audio, motion)**: the camera's event system already sends stateful
  events only on state changes (`PropertyOperation=Changed`, e.g. `Detected=1`
  then `Detected=0`). They are forwarded as they come.
* **Scene frames** arrive every ~100 ms and always differ in timestamp and
  bounding box. `src/dedupe.c` publishes a frame only if
  * a track appeared or disappeared, or
  * anything except `timestamp`, `bounding_box` and `score` values changed
    (colour lists count with their best entry only), or
  * a track moved more than `MoveThreshold` and `MinIntervalMs` passed.
* The camera sends nothing for an empty scene, so after `ClearTimeoutSec`
  without objects the app publishes `{"detections":[],"synthetic":true,...}`.

The published payload is the camera's original JSON, untouched.

Objects that have no `class` yet (fresh tracks before classification) do not count as a change,
unless *Report objects that are not classified yet* is enabled. Faces arrive as their own objects
(`class.type = "Face"`), not linked to a person.

### Which scene topic?

The camera only lets apps read three message broker topics (see the install log lines
"Adding … to list of topics that are allowed to consume"). `SceneTopic` is a pull-down:

| Topic | Use |
|---|---|
| `com.axis.analytics_scene_description.v0.beta` (default) | objects per frame with `class`, clothing/vehicle colours, bounding box |
| `com.axis.consolidated_track.v1.beta` | one summary per object (not run through the change filter) |
| `com.axis.radar.analytics_scene_description.v0.beta` | radar products only |
| `com.axis.scene.frame.v1` | not on the allow-list for apps on the tested firmware; the camera's own MQTT publisher can send it |

Replayed states: when the app subscribes, the camera sends the current state of stateful events
once, with their old timestamp. Those messages carry `"initial":true`.

Several event topics fire for one motion (for example `…/VMD/Camera1ProfileANY` and
`…/VMD/Camera1Profile1`, plus `RuleEngine/MotionRegionDetector`). Trim `MotionEvents` if you
only want one of them.

Event payload:

```json
{"time":"2026-09-29T10:06:01.010935Z","topic":"AudioClassification/Speech",
 "data":{"AudioSource":"AudioDevice0Input0","Detected":1}}
```

## Build

On the Mac (Apple silicon is fine, the SDK image is amd64 and cross-compiles):

```sh
docker build --platform=linux/amd64 --tag axis-scene-mqtt-bridge:dev .
docker cp $(docker create --platform=linux/amd64 axis-scene-mqtt-bridge:dev):/opt/app ./build
```

`build/*.eap` is the package. Use `--build-arg ARCH=armv7hf` for ARTPEC-7 and older.
`libmosquitto` is not part of the SDK; the Dockerfile cross-compiles it
statically (without TLS).

Unit tests for the de-duplication run on the host: `tests/run.sh`.

## Install and configure

1. Upload the `.eap` in the camera's web UI (*Apps*).
2. Open the app's settings in the camera UI (*Apps* → *Scene MQTT Bridge* → **Open**), enter
   the broker and save. Saving restarts the app. Start the app afterwards if it is stopped.

   The page reads and writes the app parameters through `param.cgi`; you can also set them
   directly:

   ```sh
   curl --digest -u root:PASSWORD "http://CAMERA/axis-cgi/param.cgi?action=update\
   &root.axis_scene_mqtt_bridge.MqttHost=BROKER_IP\
   &root.axis_scene_mqtt_bridge.MqttPort=1883"
   ```
3. Watch: `mosquitto_sub -h BROKER -t 'axis/#' -v`

The camera must also produce the scene metadata: enable the analytics
metadata producer *Analytics scene description* / *Object analytics* in the
camera, and install AXIS Audio Analytics for the audio classification events.

### Parameters

| Name | Default | Meaning |
|---|---|---|
| `MqttHost`, `MqttPort`, `MqttUser`, `MqttPassword` | – / 1883 | Broker. The app idles while `MqttHost` is empty. |
| `TopicPrefix` | `axis/<serial>/bridge` | Topic prefix |
| `PublishObjects`, `PublishAudio`, `PublishMotion` | yes | Switch sources on/off |
| `SceneTopic`, `SceneSource` | `com.axis.analytics_scene_description.v0.beta`, `1` | Message broker topic (pull-down) and channel |
| `PublishUnclassified` | no | Objects without a class count as a change |
| `MoveThreshold` | 0.05 | Movement (normalized image units) that counts as a change |
| `MinIntervalMs` | 1000 | Minimum interval for move-only updates |
| `ClearTimeoutSec` | 3 | Seconds without objects until the empty scene is published |
| `AudioEvents`, `MotionEvents` | see `manifest.json` | Comma separated event topics, e.g. `tns1:AudioSource/tnsaxis:TriggerLevel`. A level without namespace inherits the previous one. |
| `EventKeys` | see `manifest.json` | Event keys copied into `data` (the event API cannot list keys) |

## Where does it run? (QNAP / Container Station)

The app runs **on the camera**, not in a container. Container Station on the
QNAP is only useful for the MQTT broker (e.g. an `eclipse-mosquitto` container)
that the camera publishes to. The build happens in Docker on the Mac.

## Status

Compiles and links for aarch64 (AXIS OS SDK 12.11) and the de-duplication is unit
tested. **Not yet verified on a camera.** In particular check in the log
(`Apps → Log`) that the event subscriptions in `AudioEvents` / `MotionEvents`
match; a prefix such as `tnsaxis:AudioClassification` is expected to match all
events below it. List the events your camera declares with
`axevent/get_eventlist.py` from the
[ACAP examples](https://github.com/AxisCommunications/acap-native-sdk-examples).
