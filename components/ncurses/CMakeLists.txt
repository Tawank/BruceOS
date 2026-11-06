idf_component_register(
    SRCS
    "ncurses/base/define_key.c"
    "ncurses/base/key_defined.c"
    "ncurses/base/keybound.c"
    "ncurses/base/keyok.c"
    "ncurses/base/legacy_coding.c"
    "ncurses/base/lib_addch.c"
    "ncurses/base/lib_addstr.c"
    "ncurses/base/lib_beep.c"
    "ncurses/base/lib_bkgd.c"
    "ncurses/base/lib_box.c"
    "ncurses/base/lib_chgat.c"
    "ncurses/base/lib_clear.c"
    "ncurses/base/lib_clearok.c"
    "ncurses/base/lib_clrbot.c"
    "ncurses/base/lib_clreol.c"
    "ncurses/base/lib_color.c"
    "ncurses/base/lib_colorset.c"
    "ncurses/base/lib_delch.c"
    "ncurses/base/lib_delwin.c"
    "ncurses/base/lib_dft_fgbg.c"
    "ncurses/base/lib_echo.c"
    "ncurses/base/lib_endwin.c"
    "ncurses/base/lib_erase.c"
    "ncurses/base/lib_flash.c"
    "ncurses/base/lib_freeall.c"
    "ncurses/base/lib_getch.c"
    "ncurses/base/lib_getstr.c"
    "ncurses/base/lib_hline.c"
    "ncurses/base/lib_immedok.c"
    "ncurses/base/lib_inchstr.c"
    "ncurses/base/lib_initscr.c"
    "ncurses/base/lib_insch.c"
    "ncurses/base/lib_insdel.c"
    "ncurses/base/lib_insnstr.c"
    "ncurses/base/lib_instr.c"
    "ncurses/base/lib_isendwin.c"
    "ncurses/base/lib_leaveok.c"
    "ncurses/base/lib_move.c"
    "ncurses/base/lib_mvwin.c"
    "ncurses/base/lib_newterm.c"
    "ncurses/base/lib_newwin.c"
    "ncurses/base/lib_nl.c"
    "ncurses/base/lib_overlay.c"
    "ncurses/base/lib_pad.c"
    "ncurses/base/lib_printw.c"
    "ncurses/base/lib_redrawln.c"
    "ncurses/base/lib_refresh.c"
    "ncurses/base/lib_restart.c"
    "ncurses/base/lib_scanw.c"
    "ncurses/base/lib_screen.c"
    "ncurses/base/lib_scroll.c"
    "ncurses/base/lib_scrollok.c"
    "ncurses/base/lib_scrreg.c"
    "ncurses/base/lib_set_term.c"
    "ncurses/base/lib_touch.c"
    "ncurses/base/lib_ungetch.c"
    "ncurses/base/lib_vline.c"
    "ncurses/base/lib_wattroff.c"
    "ncurses/base/lib_wattron.c"
    "ncurses/base/lib_winch.c"
    "ncurses/base/lib_window.c"
    "ncurses/base/new_pair.c"
    "ncurses/base/resizeterm.c"
    "ncurses/base/safe_sprintf.c"
    "ncurses/base/tries.c"
    "ncurses/base/version.c"
    "ncurses/base/vsscanf.c"
    "ncurses/base/wresize.c"
    "ncurses/generated/codes.c"
    "ncurses/generated/comp_captab.c"
    "ncurses/generated/comp_userdefs.c"
    "ncurses/generated/expanded.c"
    "ncurses/generated/lib_gen.c"
    "ncurses/generated/lib_keyname.c"
    "ncurses/generated/names.c"
    "ncurses/generated/unctrl.c"
    "ncurses/tinfo/add_tries.c"
    "ncurses/tinfo/alloc_ttype.c"
    "ncurses/tinfo/comp_error.c"
    "ncurses/tinfo/comp_hash.c"
    "ncurses/tinfo/doalloc.c"
    "ncurses/tinfo/free_ttype.c"
    "ncurses/tinfo/getenv_num.c"
    "ncurses/tinfo/init_keytry.c"
    "ncurses/tinfo/lib_acs.c"
    "ncurses/tinfo/lib_baudrate.c"
    "ncurses/tinfo/lib_cur_term.c"
    "ncurses/tinfo/lib_data.c"
    "ncurses/tinfo/lib_has_cap.c"
    "ncurses/tinfo/lib_kernel.c"
    "ncurses/tinfo/lib_longname.c"
    "ncurses/tinfo/lib_options.c"
    "ncurses/tinfo/lib_print.c"
    "ncurses/tinfo/lib_raw.c"
    "ncurses/tinfo/lib_setup.c"
    "ncurses/tinfo/lib_termname.c"
    "ncurses/tinfo/lib_tgoto.c"
    "ncurses/tinfo/lib_ti.c"
    "ncurses/tinfo/lib_tparm.c"
    "ncurses/tinfo/lib_tputs.c"
    "ncurses/tinfo/lib_ttyflags.c"
    "ncurses/tinfo/name_match.c"
    "ncurses/tinfo/obsolete.c"
    "ncurses/tinfo/strings.c"
    "ncurses/tinfo/trim_sgr0.c"
    "ncurses/tinfo/use_screen.c"
    "ncurses/trace/varargs.c"
    "ncurses/tty/hardscroll.c"
    "ncurses/tty/hashmap.c"
    "ncurses/tty/lib_mvcur.c"
    "ncurses/tty/lib_twait.c"
    "ncurses/tty/lib_vidattr.c"
    "ncurses/tty/tty_update.c"
    "bruce/bruce_curses_owner.c"
    "bruce/bruce_fallback.c"
    "bruce/bruce_stubs.c"
    "bruce/bruce_tsearch.c"
    "bruce/bruce_tty.c"
    INCLUDE_DIRS "include" "ncurses" ".")

# bruce/*.c calls into core_sdk/tty.h, runtime.h, stdio.h, and process.h,
# the same way components/args reaches into core_sdk -- see that
# component's own CMakeLists.txt comment. Private: consumers of this
# component see only its own public headers (ncurses/curses.h, via
# core_sdk/curses.h), not the rest of firmware's src/ tree.
target_include_directories(${COMPONENT_LIB} PRIVATE "${PROJECT_DIR}/src")

# This is a vendored-and-heavily-adapted copy of upstream ncurses 6.4 (see
# README.md for what's dropped, stubbed, or BruceOS-rewritten and why), not
# original BruceOS code -- these flags mirror vi's and nes's own rationale
# for the same kind of warning in vendored third-party sources: real,
# long-standing patterns in that code against a stricter compiler than it
# was written against, not something introduced by vendoring it here.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=sign-compare
    -Wno-error=type-limits
    -Wno-error=implicit-fallthrough
    -Wno-error=unused-but-set-variable
    -Wno-error=unused-variable
    -Wno-error=unused-function
    -Wno-error=unused-value
    -Wno-error=maybe-uninitialized
    -Wno-error=format
    -Wno-error=format-truncation
    -Wno-error=stringop-truncation
    -Wno-error=stringop-overflow
    -Wno-error=array-bounds
    -Wno-error=missing-field-initializers
    -Wno-error=discarded-qualifiers
    -Wno-error=pointer-sign
    -DNCURSES_STATIC
    -DHAVE_CONFIG_H)
