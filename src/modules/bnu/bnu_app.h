#pragma once

int bnu_pwd_app_main(int argc, char **argv);
int bnu_ls_app_main(int argc, char **argv);
int bnu_lsblk_app_main(int argc, char **argv);
int bnu_mount_app_main(int argc, char **argv);
int bnu_unmount_app_main(int argc, char **argv);
int bnu_free_app_main(int argc, char **argv);
int bnu_top_app_main(int argc, char **argv);
int bnu_shutdown_app_main(int argc, char **argv);
int bnu_reboot_app_main(int argc, char **argv);
int bnu_mkdir_app_main(int argc, char **argv);
int bnu_touch_app_main(int argc, char **argv);
int bnu_rm_app_main(int argc, char **argv);
int bnu_cat_app_main(int argc, char **argv);
int bnu_stty_app_main(int argc, char **argv);
const char *bnu__get_working_directory(void);
