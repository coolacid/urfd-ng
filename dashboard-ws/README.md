# URFD WebSocket Dashboard

This directory contains a standalone Vue-based dashboard that mirrors the legacy non-admin live reflector pages while consuming the URFD WebSocket notifier instead of the old PHP/XML dashboard stack.

It uses the Vue production build (`*.prod.js`) for deployment. Development builds are not intended for production use.

It is intentionally kept separate from the legacy dashboard under `dashboard/` so the original dashboard remains untouched.

## Overview

The app is a static HTML/Vue implementation:

- `index.html` boots the Vue app
- `app.js` mounts the root application
- `components/*.js` contain the page views and UI logic
- `lib/notifier.js` manages the WebSocket connection to the notifier endpoint
- `styles.css` contains the dashboard styling

The dashboard reads live reflector state from `initial_connect`, `state_snapshot`, and related event messages emitted by the URFD WebSocket notifier.

## Local development

From this directory, you can serve the app with a simple local web server:

```bash
cd dashboard-ws
python3 -m http.server 8000
```

Then open:

```text
http://localhost:8000/
```

The app will connect to the notifier at the same host by default. If the notifier is running on a different host or port, you can override it with the `?ws=` query string, for example:

```text
http://localhost:8000/?ws=ws://reflector.example:8080
```

## Optional config file

This directory includes an optional production config file named `config.js.prod`. When a file named `config.js` is present next to the app, the dashboard will:

- use the Vue production bundle (`*.prod.js`)
- use the WebSocket URL specified in the config file instead of the default runtime discovery

If the file is missing, the dashboard keeps the current fallback behavior: dev Vue bundle and auto-detected same-host WebSocket endpoint.

For production use, copy `config.js.prod` to `config.js` and adjust the values as needed.

Example `config.js.prod`:

```javascript
window.__URFD_DASHBOARD_CONFIG__ = {
  vueUrl: 'https://cdn.jsdelivr.net/npm/vue@3/dist/vue.esm-browser.prod.js',
  wsUrl: 'ws://localhost:8080',
};
```

## Production / static deployment

This dashboard is designed as a static frontend; no build step is required for the browser-side Vue code because it is loaded from ES modules directly in the browser.

For production deployment, keep the Vue production build URLs (`*.prod.js`) in place. Do not switch back to the development Vue bundles.

To deploy it in production:

1. Copy the contents of `dashboard-ws/` to your web server or static hosting target.
2. Ensure the notifier endpoint is reachable from the browser.
3. Keep or update `config.js` with the correct `wsUrl` value for the deployment.
4. Ensure the web server is configured to serve `.js` modules correctly (most modern static hosts do this automatically).

### Default notifier URL

The app defaults to the same host and port 8080, using the notifier endpoint described by the project’s WebSocket protocol, e.g.:

```text
ws://<host>:8080
```

If your deployment uses a reverse proxy or a different port, update the websocket URL in `lib/notifier.js` or pass it via the query string.

## Notes

- The legacy dashboard pages under `dashboard/` are intentionally not modified.
- The reflector list and QuadNet pages remain placeholders unless a dedicated data source is added, because the notifier does not expose those exact feeds.
- This app is intended to be a websocket-native replacement for the live non-admin dashboard screens while preserving the original legacy dashboard code.
