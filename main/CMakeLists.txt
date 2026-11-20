set(COMPONENT_SRCS "main.c" "file_server.c" "wifi_sta.c" "tape_out.c" "tape_file.c" "tap_player.c" "mic_in.c" "weblog.c" "zx_menu.c" "zx_save.c" "zx_turbo.c" "tape_status.c" "web_i18n.c" "zx_text.c")
set(COMPONENT_ADD_INCLUDEDIRS ".")

set(COMPONENT_EMBED_FILES "favicon.ico" "upload_script.html" "web_head.html" "asm/spmenu.bin" "asm/spwifi.bin" "asm/spinfo.bin")

register_component()
