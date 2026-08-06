#include "bparted_app.h"

#include "core_sdk/runtime.h"

#include "bparted_cli.h"
#include "bparted_gui.h"

/* The single registered entry point ("bparted"): a GParted/parted-style
 * split where bparted_cli.c owns the "just like on Linux" subcommand
 * interface and bparted_gui.c owns the dialog__-driven menu, both built on
 * core_sdk/partition_manager.h. Which one runs is decided the same way
 * every other dual-mode Bruce app does it (see modules/webui/webui_app.c,
 * modules/nrf24/nrf24_app.c): "GUI=1 bparted" (what the launcher menu's
 * "Partitions" entry runs) opens the GUI, plain "bparted ..." from a
 * terminal/script runs the CLI. */
int bparted_app_main(int argc, char **argv) {
    if (runtime__gui_requested()) return bparted_gui__main(argc, argv);
    return bparted_cli__main(argc, argv);
}
