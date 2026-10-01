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
- Verify on newer AXIS OS whether `com.axis.scene.frame.v1` becomes readable for apps.
- Show the app status (connected, frames per minute) on the settings page.
