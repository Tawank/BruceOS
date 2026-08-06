#pragma once

/* The "bparted" CLI: list/status/create/delete/format/apply/cancel/reboot
 * subcommands over core_sdk/partition_manager.h, dispatched to by
 * bparted_app.c when GUI=1 is not set. */
int bparted_cli__main(int argc, char **argv);
