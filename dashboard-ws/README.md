# URFD WebSocket Dashboard

This directory contains a standalone Vue-based dashboard that mirrors the legacy non-admin live reflector pages while consuming the URFD WebSocket notifier instead of the old PHP/XML dashboard stack.

It uses the Vue production build (`*.prod.js`) for deployment. Development builds are not intended for production use.

This app is intentionally separate from the legacy dashboard and does not serve or expose the old PHP dashboard assets.

## Overview

The app is a static HTML/Vue implementation served by Flask in the project runtime. The frontend assets live in the `static/` directory so Flask can use its built-in static-file support instead of a custom catch-all route.

- `static/index.html` boots the Vue app
- `static/app.js` mounts the root application
- `static/components/*.js` contain the page views and UI logic
- `static/lib/notifier.js` manages the WebSocket connection to the notifier endpoint
- `static/styles.css` contains the dashboard styling

The dashboard reads live reflector state from `initial_connect`, `state_snapshot`, and related event messages emitted by the URFD WebSocket notifier.

## Local development

This dashboard can be served either as a plain static site or as a Python/Flask app.

When running under Flask, the app uses `static/` as its static folder so Vue files are served via Flask's built-in static support.

### Static mode

From the static frontend directory, you can serve the app with a simple local web server:

```bash
cd dashboard-ws/static
python3 -m http.server 8000
```

Then open:

```text
http://localhost:8000/
```

The browser will attempt to load a `config.js` file if it exists. If no file is present, the app falls back to the default production Vue build and same-host websocket URL (`ws://<host>:8080` or `wss://<host>:8080` over HTTPS).

When the Python app is serving the frontend, it will also provide a `/config.js` route derived from its Dynaconf settings. The Python-side config is not a static-mode config; it is the active app configuration while the Python server is running.

To provide a static deployment override, create a `config.js` file next to `index.html` with a minimal config like:

```javascript
window.__URFD_DASHBOARD_CONFIG__ = {
  __configLoaded: true,
  wsUrl: 'ws://reflector.example:8080',
  reflectorListUrl: null,
  quadnetLiveUrl: null,
};
```

For the Flask app configuration, this project uses Dynaconf and reads INI-style settings from `settings.ini` or `settings.local.ini` in this directory. A sample config file is included as `settings.ini.example`.

```ini
[default]
enable_reflector_list = false
enable_quadnet_live = false
vue_url = https://cdn.jsdelivr.net/npm/vue@3/dist/vue.esm-browser.prod.js
ws_url =
```

To use it in a local deployment, copy `settings.ini.example` to `settings.ini` and adjust the values as needed.

### Flask mode

The Python app can also serve the same Vue files and provide a dynamic `/config.js` response based on Python-level settings.

```bash
cd dashboard-ws
python3 -m pip install -r requirements.txt
URFD_ENABLE_REFLECTOR_LIST=true URFD_ENABLE_QUADNET_LIVE=true python3 server.py
```

Then open:

```text
http://localhost:8000/
```

When running under the Flask app, the page still checks for `config.js` in the browser, but the server supplies its own `/config.js` response so the app can use the Python runtime settings. The reflector list and QuadNet items remain off unless explicitly enabled through the Flask configuration.

The app exposes the following proxy endpoints when enabled:

```text
/api/reflector-list
/api/quadnet
```

## Production / static deployment

This dashboard is designed as a static frontend; no build step is required for the browser-side Vue code because it is loaded from ES modules directly in the browser.

For production deployment, keep the Vue production build URL (`*.prod.js`) in place. Do not switch back to the development Vue bundles.

To deploy it in production:

1. Copy the contents of `dashboard-ws/` to your web server or static hosting target.
2. Ensure the notifier endpoint is reachable from the browser.
3. Add a static `config.js` file if you need a custom websocket URL or feature defaults.
4. Ensure the web server is configured to serve `.js` modules correctly (most modern static hosts do this automatically).

### Default notifier URL

The app defaults to the same host and port 8080, using the notifier endpoint described by the project’s WebSocket protocol, e.g.:

```text
ws://<host>:8080
```

## Notes

- The legacy dashboard pages under `dashboard/` are intentionally not served or modified by this app.
- The reflector list and QuadNet pages remain placeholders unless a dedicated data source is added, because the notifier does not expose those exact feeds.
- This app is intended to be a websocket-native replacement for the live non-admin dashboard screens while preserving the original legacy dashboard code.
