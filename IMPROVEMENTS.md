# Improvements

Ideas that came up while building and testing the app. Not scheduled.

## Reuse the camera's MQTT client settings
Today the broker has to be entered again in the app's settings page. The camera's MQTT client
([MQTT client API](https://developer.axis.com/vapix/network-video/mqtt-client-api/)) returns host, port and
username but never the password, and has no documented method to publish arbitrary messages, so the app
needs its own connection anyway.

Idea: when `MqttHost` is empty, read host, port and username from the camera's MQTT client settings
(`/axis-cgi/mqtt/client.cgi`, method `getClientStatus`) using the temporary VAPIX service account that apps can
request since AXIS OS 11.6
([VAPIX access for ACAP applications](https://developer.axis.com/acap/4/develop/VAPIX-access-for-ACAP-applications/)).
A broker that needs a password would still need that one field.
Open: manifest permission for the VAPIX account, behaviour when the camera client is not configured or uses TLS.

## Other open points
- TLS for the broker connection (libmosquitto is currently built without TLS).
- Fewer duplicate motion messages by default: `…/VMD/Camera1ProfileANY` and `…/Camera1Profile1` and
  `RuleEngine/MotionRegionDetector` fire together (see README, `MotionEvents`).
- Show the app status (connected, frames per minute) on the settings page.

## Dedicated `animal/` category
The events of `axis-animal-detector` (`tnsaxis:AnimalDetector/<Species>`, data `Detected`, `Species`,
`Score`) are published under `motion/AnimalDetector/…` because the bridge only knows the categories
audio and motion, and they share `MotionHoldSec`. Idea: a third category with its own setting
(`AnimalEvents`), its own hold time and the path `animal/<Species>`.

## Animals in `bridge/objects`
Idea: subscribe to a topic of the animal detector with boxes and scores (see its IMPROVEMENTS.md) and
publish animals on `objects` like the camera's own detections.

## Event keys after an update
New keys in the default `EventKeys` (e.g. `Species`, `Score`) do not reach a camera that keeps its
stored value. Idea: merge new default keys into the stored list at start (and say so in the log).

## Retained state messages
State events (`active` true/false, e.g. `…/AnimalDetector/Any`) are published without the retain flag, so a
subscriber that starts later (openHAB after a restart) does not know the current state until the next change.
Idea: publish stateful event messages retained, the pulse-like ones (e.g. `AnimalDetector/Detection`) not.

