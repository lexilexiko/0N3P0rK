# Vendored Lua 5.4 (for on-device scripting)

`src/script/pigvm.*` embeds Lua **using the plain C API** (`lua.h`, `lauxlib.h`,
`lualib.h`), because the engine needs three things a C++ wrapper library does
not give us:

* its own **allocator** — a 48 KB budget so a user script cannot starve the heap
  the capture path needs;
* the raw **`lua_State*`** — to install the instruction hook that aborts a
  runaway script;
* a **sandboxed stdlib** — `io` / `os` / `package` / `debug` must never open.

That means the Lua sources have to be in the repo. They are **not** a `lib_deps`
entry on purpose (see the note in `platformio.ini`).

## How to vendor it (one command)

From the project root, run:

```bash
# Linux / macOS / Termux
bash scripts/fetch_lua.sh
```

```powershell
# Windows
powershell -ExecutionPolicy Bypass -File scripts/fetch_lua.ps1
```

The script downloads the official Lua 5.4 source, copies the interpreter files
into `lib/lua/src/`, skips `lua.c` / `luac.c` (they define `main()`), and
uncomments `-DPORK_LUA=1` in `platformio.ini`. Then:

```bash
pio run -t upload
```

If you prefer to do it by hand, the manual steps are below.

1. Download the official source of **Lua 5.4.7** (or any 5.4.x):
   <https://www.lua.org/ftp/lua-5.4.7.tar.gz>

2. Unpack it and copy **only the `src/` contents** here, so the layout is:

   ```text
   lib/lua/src/lua.h
   lib/lua/src/luaconf.h
   lib/lua/src/lauxlib.h
   lib/lua/src/lualib.h
   lib/lua/src/lapi.c
   lib/lua/src/lstate.c
   ... (all l*.c and l*.h)
   ```

3. Flip the switch in `platformio.ini` — uncomment this line in `build_flags`:

   ```ini
   -DPORK_LUA=1
   ```

That is the whole integration. `-Ilib/lua/src` is already in `build_flags`, so
the headers are on the include path regardless of how PlatformIO treats library
folders, and `library.json` here tells PlatformIO to compile `src/`.

## Why not `fischer-simon/Esp32Lua` / `arnoson/lua-on-arduino`

`Esp32Lua` looks tempting (it is the only Lua entry in the PlatformIO registry
that targets ESP32), but its `include/` folder exposes **only a C++ wrapper**:
`lua_State` stays private, the constructor takes no allocator, and it pulls in
`std::mutex`, `std::string` and `std::vector` — heavy for a board with no PSRAM,
and it would remove every safety limit above. Vendoring the four headers and the
C sources is smaller, faster and keeps the sandbox.

## Notes

* Lua is plain ANSI C, so it builds for the ESP32 Arduino core with no patches.
  Do **not** define `LUA_USE_POSIX` / `LUA_USE_LINUX` — those pull in POSIX
  functions the board does not have.
* `-w` in `library.json` silences the vendored C's warnings; they are not
  interesting and they bury the project's own diagnostics.
* If the build ever complains about a missing `lua.h`, the sources are not in
  `lib/lua/src/` yet — or `-DPORK_LUA=1` is on without them.
