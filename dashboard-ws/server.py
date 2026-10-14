import html
import json
import time
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

from dynaconf import Dynaconf
from flask import Flask, Response

PROD_VUE_URL = 'https://cdn.jsdelivr.net/npm/vue@3/dist/vue.esm-browser.prod.js'

settings = Dynaconf(
    envvar_prefix='URFD_DASHBOARD',
    settings_files=['settings.ini'],
    environments=True,
    load_dotenv=True,
    default_settings={
        'enable_reflector_list': False,
        'enable_quadnet_live': False,
        'vue_url': PROD_VUE_URL,
        'ws_url': None,
        'port': 8000,
        'debug': False,
    },
)

app = Flask(__name__, static_url_path='', static_folder='static')

REFLECTOR_LIST_CACHE = {
    'payload': None,
    'fetched_at': 0,
}
REFLECTOR_LIST_TTL_SECONDS = 3600


@app.get('/')
def index():
    return app.send_static_file('index.html')


def runtime_config():
    reflector_enabled = bool(settings.get('enable_reflector_list', False))
    quadnet_enabled = bool(settings.get('enable_quadnet_live', False))
    config = {
        '__configLoaded': True,
        'vueUrl': settings.get('vue_url', PROD_VUE_URL),
        'wsUrl': settings.get('ws_url'),
        'reflectorListUrl': '/api/reflector-list' if reflector_enabled else None,
        'quadnetLiveUrl': '/api/quadnet' if quadnet_enabled else None,
    }
    return config


@app.get('/config.js')
def config_js():
    payload = json.dumps(runtime_config())
    return Response(f'window.__URFD_DASHBOARD_CONFIG__ = {payload};', mimetype='application/javascript')


def _xml_text(xml_text, tag_name):
    start = f'<{tag_name}>'
    end = f'</{tag_name}>'
    start_index = xml_text.find(start)
    if start_index == -1:
        return ''
    end_index = xml_text.find(end, start_index + len(start))
    if end_index == -1:
        return ''
    return html.unescape(xml_text[start_index + len(start):end_index]).strip()


def _parse_reflector_list_xml(xml_bytes):
    xml_text = xml_bytes.decode('utf-8', 'replace')
    entries = []
    reflector_start = '<reflector>'
    reflector_end = '</reflector>'

    cursor = 0
    while True:
        start_index = xml_text.find(reflector_start, cursor)
        if start_index == -1:
            break
        end_index = xml_text.find(reflector_end, start_index + len(reflector_start))
        if end_index == -1:
            break

        block = xml_text[start_index + len(reflector_start):end_index]
        entry = {
            'name': _xml_text(block, 'name'),
            'lastIp': _xml_text(block, 'lastip'),
            'dashboardUrl': _xml_text(block, 'dashboardurl'),
            'uptime': int(_xml_text(block, 'uptime') or 0),
            'lastContact': int(_xml_text(block, 'lastcontact') or 0),
            'country': _xml_text(block, 'country'),
            'comment': _xml_text(block, 'comment'),
        }
        if entry['name']:
            entries.append(entry)
        cursor = end_index + len(reflector_end)

    payload = {
        'query': _xml_text(xml_text, 'query') or 'GetReflectorList',
        'date': _xml_text(xml_text, 'date'),
        'timestamp': int(_xml_text(xml_text, 'timestamp') or 0),
        'count': len(entries),
        'reflectors': entries,
    }
    return payload


def get_reflector_list_payload():
    global REFLECTOR_LIST_CACHE

    now = int(time.time())
    if REFLECTOR_LIST_CACHE['payload'] is not None and (now - REFLECTOR_LIST_CACHE['fetched_at']) < REFLECTOR_LIST_TTL_SECONDS:
        return REFLECTOR_LIST_CACHE['payload']

    url = 'http://xlxapi.rlx.lu/api.php?do=GetReflectorList'
    try:
        req = Request(url, headers={'User-Agent': 'urfd-dashboard/1.0'})
        with urlopen(req, timeout=15) as resp:
            body = resp.read()
    except (HTTPError, URLError, TimeoutError) as exc:
        if REFLECTOR_LIST_CACHE['payload'] is not None:
            return REFLECTOR_LIST_CACHE['payload']
        raise RuntimeError(f'Failed to fetch reflector list: {exc}') from exc

    payload = _parse_reflector_list_xml(body)
    REFLECTOR_LIST_CACHE['payload'] = payload
    REFLECTOR_LIST_CACHE['fetched_at'] = now
    return payload


@app.get('/api/reflector-list')
def reflector_list_proxy():
    enabled = bool(settings.get('enable_reflector_list', False))
    if not enabled:
        return Response('Reflector list proxy is disabled', status=403, mimetype='text/plain')

    try:
        payload = get_reflector_list_payload()
    except RuntimeError as exc:
        return Response(str(exc), status=502, mimetype='text/plain')

    return Response(json.dumps(payload), mimetype='application/json')


@app.get('/api/quadnet')
def quadnet_proxy():
    enabled = bool(settings.get('enable_quadnet_live', False))
    if not enabled:
        return Response('QuadNet proxy is disabled', status=403, mimetype='text/plain')

    url = 'https://irc-1.openquad.net/ics/ics'
    try:
        req = Request(url, headers={'User-Agent': 'urfd-dashboard/1.0', 'Range': 'bytes=-10240'})
        with urlopen(req, timeout=5) as resp:
            body = resp.read()
    except (HTTPError, URLError, TimeoutError) as exc:
        return Response(f'Failed to fetch QuadNet feed: {exc}', status=502, mimetype='text/plain')

    text = body.decode('utf-8', 'replace')
    if '\n' in text:
        text = text.split('\n', 1)[1]
    return Response(text, mimetype='text/plain; charset=utf-8')


if __name__ == '__main__':
    app.run(host='0.0.0.0', port=int(settings.get('port', 8000)), debug=bool(settings.get('debug', False)))
