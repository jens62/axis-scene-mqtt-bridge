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
* **Scene frames** arrive every ~100 ms and always differ in timestamp and bounding box.
  `src/dedupe.c` compares each frame with the last published one and publishes only if
  With `ObjectsStartStopOnly` (default) a message is sent only when a tracked object appears or its class
  type changes, and an empty scene when none was seen for `ClearTimeoutSec`. With it off, the following
  also applies:
  * a classified object appeared or disappeared, or its attributes changed (everything except
    `timestamp`, `bounding_box` and `score` values; colour lists count with their best entry), or
  * an object moved more than `MoveThreshold`.

  Faces never cause a message by themselves (they flicker); they are part of whatever frame goes out.
  At most one message per `MinIntervalMs` (default 3 s) is sent. A change inside the interval is held
  back and sent when the interval is over, so "the person left" is not lost.
* The camera sends nothing for an empty scene, so after `ClearTimeoutSec` without objects the app
  publishes `{"detections":[],"synthetic":true,...}`.

The published payload is the camera's original JSON, untouched.

Objects that have no `class` yet (fresh tracks before classification) do not count as a change,
unless *Report objects that are not classified yet* is enabled. Faces arrive as their own objects
(`class.type = "Face"`, "Head" in `frame.v1`), not linked to a person.

### Which scene topic and transport?

Since 0.4.0 the scene frames come through **Device Data Hub** (`SceneTransport=devicedatahub`,
stable from AXIS OS 12.11; the package then needs 12.11.72 or newer). The bridge only reads a
public topic, which works without the `deviceDataHub` resource in the manifest, so it has none. The Message Broker API and the `analytics_scene_description.v0.beta`
topic are deprecated, the Message Broker API is removed in AXIS OS 13. It stays selectable
(`SceneTransport=messagebroker`) for now. `SceneTopic` is a pull-down:

| Topic | Transport | Use |
|---|---|---|
| `com.axis.scene.frame.v1` (default) | Device Data Hub | objects per frame (`detections`) with `class`, clothing colours, bounding box; an idle frame with only `timestamp` every 2 s while the scene is empty |
| `com.axis.scene.object_track.v1` | Device Data Hub | one summary per finished track, arrives late (about 20 s for people); not run through the change filter |
| `com.axis.analytics_scene_description.v0.beta` | Message Broker | the previous default, deprecated |
| `com.axis.consolidated_track.v1.beta` | Message Broker | deprecated |
| `com.axis.radar.analytics_scene_description.v0.beta` | Message Broker | radar products only |

A `SceneTopic` stored by an older version (the deprecated one) is replaced by `scene.frame.v1`
when the transport is Device Data Hub; the log says so. Differences in `frame.v1`: heads are
reported as `class.type = "Head"` (the filter ignores them like faces), a frame can carry
`track_events` (e.g. `TrackEnded`, ignored), and `SceneSource` is the channel number.

Hold time: the camera's classifiers and motion detectors switch on and off in short bursts.
With `AudioHoldSec` / `MotionHoldSec` (default 5 each) the first "on" (`Detected`, `triggered`,
`active` or `State` true) is sent at once, further bursts are merged, and the "off" follows that
many seconds after the last burst (it carries the time of that last "off"). `0` forwards every
on/off. Applies per event topic. Because the camera's timer works in whole seconds, the hold can be
up to a second longer.

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

The settings page has a check list for the known motion events (several report the same motion,
pick one per source), **Reset to defaults** (including the broker; press Save afterwards),
and **Export** / **Import** of the settings as a JSON file (without the password) for moving
settings to another camera or after a reinstall.

Updating: upload the new `.eap` over the old one, the settings stay (parameters survive an
update). The camera may refuse a package with the same version number as the installed one, so every
build gets a new version in `app/manifest.json`. The log shows the installed version and the
settings in use at every start.

### Parameters

| Name | Default | Meaning |
|---|---|---|
| `MqttHost`, `MqttPort`, `MqttUser`, `MqttPassword` | – / 1883 | Broker. The app idles while `MqttHost` is empty. |
| `TopicPrefix` | `axis/<serial>/bridge` | Topic prefix |
| `PublishObjects`, `PublishAudio`, `PublishMotion` | yes | Switch sources on/off |
| `SceneTransport` | `devicedatahub` | `devicedatahub` or `messagebroker` (deprecated) |
| `SceneTopic`, `SceneSource` | `com.axis.scene.frame.v1`, `1` | Scene topic (pull-down) and channel |
| `AudioHoldSec`, `MotionHoldSec` | 5 | Merge on/off bursts into one episode, 0 = off |
| `PublishUnclassified` | no | Objects without a class count as a change |
| `ObjectsStartStopOnly` | yes | Objects behave like a motion detector: one message when an object appears, an empty scene when none was seen for `ClearTimeoutSec`. Movement and attribute changes (clothing colours flicker) are ignored. |
| `MoveThreshold` | 0.15 | (only if `ObjectsStartStopOnly` is off) Movement (normalized image units) that counts as a change |
| `MinIntervalMs` | 3000 | Minimum interval between object messages |
| `ClearTimeoutSec` | 3 | Seconds since the last frame containing an object (faces do not count) until the empty scene is published |
| `AudioEvents`, `MotionEvents` | audio: three topics, motion: `tns1:RuleEngine/MotionRegionDetector` | Comma separated event topics, e.g. `tns1:AudioSource/tnsaxis:TriggerLevel`. A level without namespace inherits the previous one. |
| `EventKeys` | see `manifest.json` | Event keys copied into `data` (the event API cannot list keys) |

## Status

Compiles and links for aarch64 (AXIS OS SDK 12.11) and the de-duplication is unit
tested. **Not yet verified on a camera.** In particular check in the log
(`Apps → Log`) that the event subscriptions in `AudioEvents` / `MotionEvents`
match; a prefix such as `tnsaxis:AudioClassification` is expected to match all
events below it. List the events your camera declares with
`axevent/get_eventlist.py` from the
[ACAP examples](https://github.com/AxisCommunications/acap-native-sdk-examples).
