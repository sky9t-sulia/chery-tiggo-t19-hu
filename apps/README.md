# Lua programs for the HU

Programs live **only on the USB stick**: `<stick>/apps/<folder>/`. Without a stick there are none. The **Lua Apps** screen shows everything in `apps/`. This folder is excluded from the music, video and photo search.

```
apps/calculator/
  manifest.lua   description (optional)
  main.lua       program (required)
  icon.png       icon (optional): the whole tile, 256×256, transparent corners
  *.lua          modules: require("name")
```

## manifest.lua

```lua
return {
  name = "Calculator",          -- name (without it - the folder name)
  desc = "Basic calculator",    -- line under the name
  version = "1.0",              -- "apps/<folder> · v1.0"
  author = "Suleiman",
  color = "#FF9F0A",            -- tile color if there is no icon.png
}
```

## main.lua

The program draws in the "Running app" window: everything below the header is its canvas. Coordinates are design px (as in Car Media), the canvas is 1140×560. The HU calls the program's global functions if they exist:

| function | when |
|---|---|
| `draw(w, h)` | window redraw; drawing is allowed only here |
| `tap(id)` | finger released on a `ui.button(id, …)` button |
| `touch(phase, x, y)` | any touch of the canvas: `"down"` / `"move"` / `"up"` |
| `tick(now)` | before every frame (if present, the window is redrawn constantly - for animations only) |

After `tap` / `touch` the window redraws itself. If the state changed for another reason, call `ui.redraw()`.

### ui - system elements

```lua
ui.rect(x, y, w, h, color [, radius [, border_color]])
ui.text(str, x, y, { size = 20, weight = 600, color = "#F5F5F7",
                     align = "left" | "center" | "right",   -- from x
                     maxw = 0,       -- clip to width (overflow: hidden)
                     spacing = 0,    -- letter-spacing, px
                     tnum = false }) -- digits of equal width
                     -- y - middle of the line
ui.button(id, x, y, w, h, { label, size = 28, weight = 600, bg, fg, radius = 22,
                            border, icon, icon_size = 24 })
          -- system button: press (scale .93), MCU click, color change .2s; id - string or number
ui.icon(name, cx, cy, size, color)   -- system icons ("close", "play", "chev_r", ...)
ui.text_width(str, size, weight [, tnum]) -> px
ui.line_height(size, weight) -> px
ui.size() -> w, h
ui.time() -> seconds
ui.redraw()
ui.close()                            -- close the program window
```

Colors are given as `"#RRGGBB"`, `"#RRGGBBAA"`, `"rgba(255, 255, 255, 0.10)"`, `0xRRGGBB`. System colors are `"accent"`, `"on_accent"`, `"accent_soft"`: they follow the accent chosen in Display.

Fonts are baked in advance (weight/size - `hu/assets_fonts.txt`). If a combination is missing, the nearest one is used and `font … not baked` is written to the log.

### Sandbox

Available: the libraries `string`, `table`, `math`, `utf8`, `os.time()`, `os.clock()`, as well as `print` (writes to the HU log) and `require("module")` (only `.lua` from the program folder).

Not available: `io`, the rest of `os`, `load`, `dofile`, `loadfile`, `package` and bytecode.

Limits: 16 MB of memory and 20 million instructions per call. On an error or when a limit is exceeded the window closes, a HUD "Can't open: <program>" appears, and the error text goes to the log (`hu: lua <folder>: …`).

### sys - only for programs from NAND (`/mnt/data1/hu-apps`)

```lua
sys.console()        -- the screen becomes a Linux console (root sh, USB keyboard); exit: exit or the Home button. true - opened
sys.log("hu" | "mcu", n) -> { lines }   -- last n lines of the HU log / MCU exchange (read-only)
```

For programs from a USB stick `sys.console()` does not give root and returns `false`.
