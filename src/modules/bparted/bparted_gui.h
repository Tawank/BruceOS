#pragma once

/* The "bparted" GUI: one screen showing the layout running now next to the
 * one the next boot will use, over Create/Delete/Format/Apply/Cancel
 * actions. Built on core_sdk/partition_manager.h, dispatched to by
 * bparted_app.c when GUI=1 is set. */
int bparted_gui__main(int argc, char **argv);
