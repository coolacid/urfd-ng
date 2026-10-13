# WebSocket notifier protocol

This project exposes a lightweight WebSocket notifier for clients that want near-real-time reflector state without polling a JSON file.

## Connection lifecycle

- The server opens a libwebsockets context when the notifier backend is enabled.
- Each client connection is accepted on the configured bind address and port.
- When a client connects, the server immediately sends a `state_snapshot` event describing the current reflector state.
- The connection stays alive with periodic keepalive traffic. If a client is idle too long, the server closes it.

## Message format

Every message from the server is a JSON object with the following shape:

```json
{
  "event_type": "state_snapshot",
  "timestamp": 1730000000,
  "reflector_name": "URF123",
  "payload": {
    "server_started": 1730000000,
    "current_time": 1730000000,
    "uptime_seconds": 0,
    "links": [
      {
        "callsign": "DSTAR1",
        "on_module": "A",
        "protocol": "dcs",
        "connect_time": 1730000000
      }
    ],
    "last_heard": [
      {
        "callsign": "N7TAE",
        "on_module": "A",
        "via_peer": "URF123",
        "via_node": "",
        "protocol": "ysf",
        "last_heard_time": 1730000000,
        "locator": "FN42xx"
      }
    ],
    "open_channels": [
      {
        "module": "A",
        "stream_id": 1234,
        "user": "N7TAE",
        "callsign": "N7TAE",
        "protocol": "ysf",
        "linked_module": "A"
      }
    ]
  }
}
```

Notes:

- `event_type` identifies the event category.
- `timestamp` is the Unix time when the server emitted the event.
- `reflector_name` is the configured reflector callsign when available.
- `payload` is always a JSON object, even when the event is just a minimal status update.

## Server-to-client events

The notifier emits regular event objects for the reflector lifecycle and state changes.

### `state_snapshot`

Sent immediately after a client connects.

This includes the current state of the reflector:

- `server_started`: Unix timestamp when the reflector process started
- `current_time`: Unix timestamp at the moment the snapshot was built
- `uptime_seconds`: elapsed time since start
- `links`: all currently linked clients/modules
- `last_heard`: recent heard stations
- `open_channels`: active streams currently transmitting on a module

### `client_added`

Sent when a reflector client is added.

Example payload:

```json
{
  "callsign": "DSTAR1",
  "protocol": "dcs",
  "reflector_module": "A",
  "connect_time": 1730000000
}
```

The websocket transport intentionally omits IP addresses from this payload.

### `client_removed`

Sent when a reflector client is removed.

Example payload:

```json
{
  "callsign": "DSTAR1",
  "protocol": "dcs",
  "reflector_module": "A",
  "last_heard_time": 1730000000
}
```

### `peer_added`

Sent when a peer node is added.

Example payload:

```json
{
  "callsign": "URFPEER",
  "protocol": "mmdvm",
  "clients": [
    {
      "callsign": "M1TEST",
      "protocol": "mmdvm",
      "reflector_module": "B"
    }
  ]
}
```

### `peer_removed`

Sent when a peer node is removed.

Example payload:

```json
{
  "callsign": "URFPEER",
  "protocol": "mmdvm"
}
```

### `station_started`

Sent when a station begins transmitting or becomes active.

Example payload:

```json
{
  "callsign": "N7TAE",
  "via_node": "URF123",
  "via_peer": "URF123",
  "on_module": "A",
  "protocol": "ysf",
  "last_heard_time": 1730000000,
  "locator": "FN42xx"
}
```

### `station_stopped`

Sent when a station becomes inactive.

The payload matches the same shape as `station_started`, but describes the active station being cleared.

### `uptime_update`

Sent from the main process at startup and whenever the process emits an uptime event.

Example payload:

```json
{
  "server_started": 1730000000,
  "current_time": 1730000000,
  "uptime_seconds": 0
}
```

## Ping / pong behavior

The server uses libwebsockets keepalive timers to monitor client responsiveness.

- Each client connection gets a timer configured to the configured ping interval.
- When the timer expires, the server checks whether the connection has been idle long enough.
- If the client has been quiet for too long, the server closes the socket as idle.
- Otherwise, it sends a WebSocket ping frame with payload `urfd`.
- The peer is expected to respond with a corresponding pong frame.
- If the ping fails or the client does not respond in a timely way, the server closes the connection.

This is not a user-space application-level ping/pong protocol; it is the standard WebSocket control frame mechanism implemented by libwebsockets.

## Notes

- The server does not accept messages from clients for this notifier path; it only pushes state updates to connected websocket clients.
- The `state_snapshot` is intentionally sent on connect so that a freshly connected browser or UI can render the current reflector status immediately rather than showing a blank page.
- The server intentionally omits client IP addresses from websocket payloads for privacy and UI simplicity.
