# Remote control (JSON-RPC)

inspectrum exposes a [JSON-RPC 2.0](https://www.jsonrpc.org/specification) API
over a local Unix-domain socket so other programs can open files, seek, and
query state.

## Connecting

- **Socket:** `$TMPDIR/inspectrum.sock` (the exact path is printed to stderr on
  startup, e.g. `JSON-RPC control listening on /.../inspectrum.sock`).
- **Framing:** one compact JSON object per line (`\n`-terminated), request and
  response alike.
- A request with no `id` is a notification and gets no reply.

## Methods

| Method | Params | Returns |
|--------|--------|---------|
| `open` | `{ "path": string }` | state |
| `seek` | `{ "sample": int }` **or** `{ "seconds": number }` | state |
| `snapshot` | none, or `{ "path": string }` | image |
| `getState` | none | state |
| `rpc.discover` | none | [OpenRPC](https://spec.open-rpc.org/) document describing this API |

`seek` scrolls so the offset is at the left edge; it rounds down to the nearest
spectrogram column (`samplesPerColumn = fftSize·nfftSkip/zoomLevel`).

`snapshot` captures the current canvas (spectrogram + frequency/time axes +
annotation boxes) as a PNG. With `{ "path": ... }` it writes the file and
returns `{ path, width, height }`; otherwise it returns the PNG inline as
`{ png_base64, width, height }`. **The window must be visible and unoccluded**
(the canvas is captured from the on-screen OpenGL surface), and macOS requires
screen-recording permission for the process.

**state** object:

```json
{ "file": "...", "sampleOffset": 0, "sampleCount": 20000000,
  "sampleRate": 40000000, "fftSize": 512, "zoomLevel": 1,
  "powerMin": -110, "powerMax": -35 }
```

Errors use standard JSON-RPC codes: `-32700` parse, `-32600` invalid request,
`-32601` method not found, `-32602` invalid params.

## Examples

Shell:

```sh
printf '{"jsonrpc":"2.0","method":"seek","params":{"seconds":0.1},"id":1}\n' \
  | nc -U "$TMPDIR/inspectrum.sock"
```

Python:

```python
import socket, json

def call(method, params=None, rid=1):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(f"/tmp/inspectrum.sock")  # or $TMPDIR/inspectrum.sock
    req = {"jsonrpc": "2.0", "method": method, "id": rid}
    if params is not None:
        req["params"] = params
    s.sendall((json.dumps(req) + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        buf += s.recv(65536)
    return json.loads(buf.split(b"\n")[0])

call("open", {"path": "/data/capture.sigmf-meta"})
call("seek", {"sample": 9300984})
print(call("getState")["result"])
```

Discover the API at runtime with `rpc.discover`.
