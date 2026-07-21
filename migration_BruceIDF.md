# Bruce OS Architecture

## Goal

Bruce OS is a small runtime for ESP32 devices.

The core should stay as small as possible and only provide the runtime and hardware APIs.

Everything the user interacts with should be an application in the `modules/` directory.

Bruce OS should not force any user interface. A user may boot into a launcher, terminal, clock, or their own application, by setting the `launcherApp` in the configuration. That application may be a menu, a terminal, or anything else. And that application may launch other applications, or not using the AppRunner/Task API.

---

# Project Structure

```
core/
    config/
    runtime/
    task/
    app_runner/

    wifi/
    display/
    storage/
    input/
    gpio/
    js_interpreter/
    bt/
    ir/
    rf/
    rfid/
    gps/
    ethernet/
    fm/
    audio/

modules/
    launcher/
    terminal/
    wifi/
    config/
    weather/
    clock/
```

The **core** contains the operating system.

The **modules** directory contains everything the user runs.

---

# Core

The core should only contain:

- Bruce HAL
- Bruce Runtime
- BruceConfig
- Task Manager
- AppRunner

Nothing else. Core should be callable from AppRunner tasks elf/javascript and should be thread safe.

The core should **not** contain:

- menus
- launcher
- themes
- settings screens
- application logic

---

# BruceConfig

BruceConfig is responsible for loading and saving:

```
bruce.json
```

It stores system settings such as:

- colors
- brightness
- timezone
- sound
- WiFi
- startup application
- developer mode
- QR codes
- user preferences

Applications should never edit JSON directly.

---

# AppRunner

AppRunner starts applications.

Applications register exactly one entry point.

```c
apprunner__register(
    "wifi",
    wifi_app
);

apprunner__register(
    "clock",
    clock_app
);
```

Applications are started by name.

```c
apprunner__run("wifi");
```

Arguments work exactly like `main()`.

```c
apprunner__run(
    "wifi",
    "connect",
    "MyWifi",
    "password123"
);
```

Applications receive:

```c
int wifi_app(int argc, char **argv);
```

The application decides what to do.

Example:

```c
int wifi_app(int argc, char **argv)
{
    if (argc == 0)
        wifi_menu();

    else if (!strcmp(argv[0], "scan"))
        wifi_scan();

    else if (!strcmp(argv[0], "connect"))
        wifi_connect(argv[1], argv[2]);

    return 0;
}
```

AppRunner does not know application commands.

It simply starts applications.

---

# Task Manager

Task Manager manages every running application.

Responsibilities:

- start
- stop
- restart
- pause
- resume
- move to background
- bring to foreground

Tracks:

- task
- heap usage
- stack usage
- CPU usage
- running state

Applications never access FreeRTOS directly.

---

# Runtime

The runtime is responsible for:

- creating tasks
- destroying tasks
- loading applications
- unloading applications
- resource ownership
- memory tracking

Applications use the Bruce API only.

---

# HAL

The core provides all hardware APIs.

Naming convention:

```
module__action()
```

Examples:

```c
wifi__connect();

wifi__disconnect();

wifi__scan();
```

```c
display__clear();

display__update();

display__draw_text();
```

```c
gpio__read();

gpio__write();
```

```c
fs__open();

fs__read();

fs__write();
```

```c
audio__play();
```

The HAL owns all hardware resources.

Applications never use ESP-IDF directly.

---

# Applications

Applications live inside:

```
modules/
```

Examples:

```
launcher/

terminal/

wifi/

config/

weather/

clock/
```

Applications only use Bruce APIs.

Applications never access ESP-IDF directly.

---

# Launcher

Launcher is optional.

It is just another application.

It may read:

```
apps.json
```

Example:

```json
{
    "wifi": {
        "Connect": "wifi connect"
    },

    "clock": "clock",

    "config": {
        "Display": "config display"
    }
}
```

Selecting an item simply executes:

```
wifi connect
```

↓

```c
apprunner__run(...)
```

Users are free to replace the launcher.

---

# Terminal

Terminal is also just an application.

It parses commands.

Example:

```
wifi

wifi scan

wifi connect MyWifi password

clock

weather
```

Each command becomes:

```c
apprunner__run(...)
```

A user may set Terminal as the startup application instead of Launcher.

---

# Startup

BruceConfig contains:

```json
{
    "startupApp": "terminal"
}
```

or

```json
{
    "startupApp": "launcher"
}
```

or

```json
{
    "startupApp": "my_app"
}
```

The runtime simply executes:

```c
apprunner__run(startupApp);
```

No application is special.

---

# Resource Ownership

The runtime owns every resource.

Examples:

- memory
- files
- GPIO
- WiFi
- sockets
- timers
- display

Resources are released automatically when an application exits.

---

# Memory

Applications allocate memory using the Bruce API.

```c
bruce__malloc();

bruce__free();
```

The runtime tracks allocations for each application.

When an application exits, remaining allocations are released automatically.

---

# Background Applications

Applications may run in:

- foreground
- background

Examples:

- Weather
- MQTT
- Clock
- Music player

Task Manager manages them all.

---

# Design Rules

- Keep the core small.
- The core only provides runtime and hardware APIs.
- Every application has one entry point.
- Applications decide how to handle their own arguments.
- AppRunner only starts applications.
- Task Manager manages application lifecycles.
- Applications communicate through AppRunner.
- The HAL owns all hardware.
- Applications never access ESP-IDF directly.
- Every public API follows the `module__action()` naming convention.
- Launcher is optional.
- Terminal is optional.
- Any application can become the startup application.
- Nothing in the core should depend on a specific user interface.