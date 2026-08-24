# Chrome Developer Launcher

A Windows 10+ system tray utility that launches Chrome with remote debugging enabled for AI agent automation.

## Features

- **Remote Debugging** - Launches Chrome with `--remote-debugging-port` for Chrome DevTools Protocol access
- **Port Forwarding** - Automatically sets up netsh port forwards for all non-loopback network interfaces
- **System Tray** - Runs quietly in the system tray with status monitoring
- **Status Display** - Shows Chrome version, API status, and active port forwards
- **Configuration** - Registry-backed settings with modern WebView2 configuration dialog
- **Self Update** - Compares embedded local and repository versions, installs newer builds, or force-reinstalls the same version
- **Auto-Elevation** - Automatically requests administrator privileges (required for port forwarding)
- **Single Instance** - Prevents multiple instances from running simultaneously
- **Clean Shutdown** - Removes port forwards and terminates Chrome processes on exit

## Requirements

- Windows 10 or later
- Administrator privileges (for netsh port forwarding)
- Google Chrome installed
- Microsoft Edge WebView2 Runtime (included with Windows 11; [download for Windows 10](https://developer.microsoft.com/en-us/microsoft-edge/webview2/))

## Usage

1. Run `ChromeDevLauncher.exe`
2. Accept the UAC elevation prompt
3. Configure Chrome path via right-click menu if not auto-detected
4. Chrome launches with remote debugging on port 9222 (default)

## Configuration Options

- **Chrome Executable Path** - Path to chrome.exe
- **Debug Port** - Remote debugging port (default: 9222)
- **Chrome IP Address** - Address Chrome binds to (default: 127.0.0.1)
- **Status Check Interval** - How often to poll Chrome DevTools API (default: 60 seconds)
- **Automatically Check for Updates** - Checks at startup, whenever Configure opens, and every 60 minutes (enabled by default)

Settings are stored in the Windows Registry at:
```
HKEY_CURRENT_USER\SOFTWARE\JPIT\ChromeDevLauncher
```

The configuration footer displays the application and WebView2 runtime
versions as `v<application version> / <WebView2 version>`.

## Updates

Automatic checks are silent unless a newer version is available. A newer
build opens Configuration and displays the update prompt. **Ignore this
version** suppresses that version during later automatic checks, including
after restart; the manual **Update** button still displays every result.

Checks download the repository's
[`release/ChromeDevLauncher.exe`](release/ChromeDevLauncher.exe) to the user's
temporary directory and compare its embedded Windows file version with the
running executable. The Update button shows download speed while checking and
acts as a cancellable stop button. A matching remote version offers **Force
update**; an older repository build is never installed.

Accepted updates use an elevated helper to replace the running executable,
roll back if the updated application cannot start, clean up temporary files,
and reopen Configuration with the installed version confirmation. Cancelling
the download, result prompt, or UAC approval leaves the current version
running.

## System Tray Menu

Right-click the tray icon to see:
- Chrome version and connection status
- API response status
- Active port forwards
- Configure option
- Exit option

## Building from Source

Requires MinGW-w64 cross-compiler and Node.js on Linux:

```bash
make clean && make
```

This builds the React/shadcn frontend (`assets/`), embeds it into the executable as a resource, and cross-compiles with MinGW.

Output: `release/ChromeDevLauncher.exe`

## License

[MIT](LICENSE)
