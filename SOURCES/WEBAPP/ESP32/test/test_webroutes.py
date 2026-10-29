"""Registration order of the web server routes in web.cpp.

ESPAsyncWebServer gives a request to the first registered handler whose URI
matches, and a handler for "/x" also matches "/x/<anything>"
(AsyncCallbackWebHandler::canHandle). A route "/x/y" registered after "/x" with
the same method is therefore never reached. This test reads the server.on()
calls in web.cpp and fails on such a shadowed route.

Run: SOURCES/WEBAPP/ESP32/test/run.sh
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
WEB_CPP = os.path.join(HERE, '..', 'aurora', 'web.cpp')

ROUTE = re.compile(r'server\.on\(\s*"([^"]+)"\s*,\s*(HTTP_[A-Z]+)')


def routes(text):
    """(uri, method) of every server.on() call, in source order."""
    return ROUTE.findall(text)


def shadowed(text):
    """The routes that an earlier route with the same method takes over."""
    found = []
    seen = []
    for uri, method in routes(text):
        for earlier_uri, earlier_method in seen:
            same_method = method == earlier_method or 'HTTP_ANY' in (method, earlier_method)
            if same_method and uri.startswith(earlier_uri.rstrip('/') + '/') and earlier_uri != '/':
                found.append('%s %s after %s' % (method, uri, earlier_uri))
        seen.append((uri, method))
    return found


def test_finds_a_shadowed_route():
    text = 'server.on( "/m", HTTP_GET, f ); server.on( "/m/x", HTTP_GET, g );'
    assert shadowed(text) == ['HTTP_GET /m/x after /m']


def test_other_method_or_longer_name_is_not_shadowed():
    text = ('server.on( "/m", HTTP_POST, f ); server.on( "/m/x", HTTP_GET, g );'
            'server.on( "/preset", HTTP_GET, f ); server.on( "/preset.param", HTTP_GET, g );')
    assert shadowed(text) == []


def test_web_cpp_has_no_shadowed_route():
    with open(WEB_CPP, encoding='utf-8') as f:
        text = f.read()
    assert len(routes(text)) > 50
    assert shadowed(text) == []
