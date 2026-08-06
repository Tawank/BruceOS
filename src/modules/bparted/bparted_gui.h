#pragma once

/* The "bparted" GUI: a dialog__-driven menu over the same
 * core_sdk/partition_manager.h state, dispatched to by bparted_app.c when
 * GUI=1 is set. */
int bparted_gui__main(int argc, char **argv);
