# gview
<img width="1869" height="912" alt="image" src="https://github.com/user-attachments/assets/3d284354-fd7c-4f4b-a56f-9d2483a7a9e5" />

Trace viewer for [microprof](https://github.com/carlinogiovanni04/microprof) Chrome Trace JSON, built on
[gcui](https://github.com/carlinogiovanni04/gcui).

It loads a trace, draws a zoomable timeline with one row per thread, and an
aggregated flame graph. Clicking a zone inspects it in the details panel.

## Controls

| Input | Action |
|---|---|
| `Ctrl+O` | Open a trace |
| `R` | Reload the current file |
| `F` / `Home` | Fit the whole trace |
| Mouse wheel | Zoom the timeline around the cursor |
| `Shift` + wheel | Scroll vertically |
| Left drag | Pan the timeline |
| Click | Select a zone |
| Drag a splitter | Resize the details panel or the thread column |
| `Tab` / `1` / `2` | Switch timeline and flame graph |
| `PageUp` / `PageDown` | Zoom in / out |
| Arrow keys | Pan |
| `Esc` | Clear the selection |

The trace file is polled every half second and reloaded automatically when its
timestamp or size changes.

## Trace format

gview reads the Chrome Trace subset microprof emits:

- `B` / `E` pairs for nested zones,
- `X` complete events,
- `M` metadata (thread names are used when present),
- microsecond `ts` / `dur`, decimal values accepted.

Unmatched `E` events, events without timestamps, and zones still open at the end
are counted and shown as warnings rather than failing the load.

## Layout

```text
gview/
  include/gview.h        trace types, JSON parser, model, flame aggregation
  src/main.c             app loop, timeline, flame graph, details, loading
  tests/test_parse.c     parser and model tests
  tests/fixtures/        small sample trace
```
