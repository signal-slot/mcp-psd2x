# mcp-psd2x

An MCP (Model Context Protocol) server for inspecting and exporting Adobe Photoshop (PSD) files. Built with [QtMcp](https://github.com/signal-slot/qtmcp) and [QtPsd](https://github.com/signal-slot/qtpsd).

## Tools

| Tool | Parameters | Description |
|------|-----------|-------------|
| `load_psd` | `path` | Load a PSD file for inspection and export |
| `get_layer_tree` | | Get the full layer hierarchy |
| `get_layer_details` | `layerId` | Get detailed info for a layer (text runs, shape path, linked files, opacity, export hint) |
| `set_export_hint` | `layerId`, `type`, `options` | Configure how a layer is exported |
| `do_export` | `format`, `outputDir`, `options` | Export the PSD to a target format |
| `list_exporters` | | List available exporter plugins |
| `save_hints` | | Persist export hints to the `.psd_` sidecar file |

### set_export_hint

- **layerId** (int) — Layer ID to configure
- **type** (string) — `embed`, `merge`, `custom`, `native`, `skip`, or `none`
- **options** (string) — JSON object with optional keys:
  - `visible` (bool) — whether the layer is visible in export
  - `componentName` (string) — component name for `custom` type
  - `baseElement` (string) — `Container`, `TouchArea`, `Button`, or `Button_Highlighted` for `native` type

### do_export

- **format** (string) — exporter plugin key (e.g. `QtQuick`, `Flutter`, `SwiftUI`)
- **outputDir** (string) — absolute path to the output directory
- **options** (string) — JSON object with optional keys:
  - `width` (int) — output width in pixels (0 or omitted = original)
  - `height` (int) — output height in pixels (0 or omitted = original)
  - `fontScaleFactor` (double) — font scale factor (default: 1.0)
  - `imageScaling` (bool) — enable image scaling (default: false)
  - `makeCompact` (bool) — enable compact output (default: false)

## Build

Requires Qt 6.

### Using submodules (recommended)

```bash
git clone --recursive https://github.com/signal-slot/mcp-psd2x.git
cd mcp-psd2x
./build.sh
```

Or if already cloned:

```bash
git submodule update --init --recursive
./build.sh
```

### Using external builds

If you have [QtMcp](https://github.com/signal-slot/qtmcp) and [QtPsd](https://github.com/signal-slot/qtpsd) built separately:

```bash
cmake -B build -DQT_ADDITIONAL_PACKAGES_PREFIX_PATH="<qtmcp-build>;<qtpsd-build>"
cmake --build build
```

## Usage

### stdio (default)

With submodule build:

```bash
export QT_PLUGIN_PATH="$PWD/build-qtmcp/lib64/qt6/plugins:$PWD/build-qtpsd/lib64/qt6/plugins"
./build/mcp-psd2x
```

With external builds:

```bash
export QT_PLUGIN_PATH="<qtmcp-build>/lib64/qt6/plugins:<qtpsd-build>/lib64/qt6/plugins"
./build/mcp-psd2x
```

### SSE

```bash
./build/mcp-psd2x --backend sse --address 127.0.0.1:8000
```

### Claude Desktop configuration

With submodule build:

```json
{
  "mcpServers": {
    "psd2x": {
      "command": "/path/to/mcp-psd2x/build/mcp-psd2x",
      "env": {
        "QT_PLUGIN_PATH": "/path/to/mcp-psd2x/build-qtmcp/lib64/qt6/plugins:/path/to/mcp-psd2x/build-qtpsd/lib64/qt6/plugins",
        "LD_LIBRARY_PATH": "/path/to/mcp-psd2x/build-qtmcp/lib64:/path/to/mcp-psd2x/build-qtpsd/lib64"
      }
    }
  }
}
```

## License

BSD-3-Clause
