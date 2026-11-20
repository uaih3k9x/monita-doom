#!/usr/bin/env python3
"""monita-doom 的 2PPA 静态校验: 调 Monita 的 fw/sdk/validate.py (loader 规则 + 导入守卫 + 不许有 VFP 指令),
再给 doom / doom_snd 两个目标加上逐个点名的导入白名单 (名单外的导入一律 FAIL)。不连设备。

  MONITA_FW=<Monita 仓库>/fw python3 tools/r05_validate.py build/r05/doom.bin build/r05/doom.elf --target doom
"""
import os, sys

FW = os.environ.get("MONITA_FW")
if not FW or not os.path.isfile(os.path.join(FW, "sdk", "validate.py")):
    sys.exit("要设 MONITA_FW=<Monita 仓库>/fw (需要 sdk/validate.py 和 sdk/r05_exports.csv)")
sys.path.insert(0, os.path.join(FW, "sdk"))
import validate as V  # noqa: E402

DOOM = {
    "ql_pin_set_func", "ql_gpio_init", "ql_gpio_deinit", "ql_gpio_set_level", "ql_gpio_get_level",
    "ql_rtos_task_create", "ql_rtos_task_sleep_ms", "ql_dev_cfg_wdt", "ql_dev_feed_wdt", "osiUpTimeUS",
    "ql_uart_open", "ql_uart_read", "ql_uart_write", "ql_uart_register_cb",
    "ql_mipi_lcd_init", "ql_mipi_lcd_write_screen", 0xac3ae871,          # r05_lcd_pm_lock (名字未解出)
    "malloc", "free", "ql_dev_memory_size_query",                          # 导入成 r05_kmalloc / r05_kfree
    "ql_pwrkey_shutdown_time_set", "ql_pwrkey_callback_register", "ql_pwrkey_press_cb_register",
    "ql_pwrkey_release_cb_register",
    # 外挂 flash: 只有挂载和读; 写类 (ql_fwrite / ql_fsync / ql_mkdir / ql_rename / ql_remove) 在 validate.py 永久禁止名单里
    "ql_spi6_ext_nor_flash_init", "ql_spi6_ext_nor_flash_sffs_mount",
    "ql_fopen", "ql_fread", "ql_fseek", "ql_fclose", "ql_fsize",
}
SOUND = {"ql_set_audio_path_speaker", "ql_pcm_open", "ql_pcm_write", "ql_pcm_close", "ql_aud_data_done"}
V.TARGET_IMPORT_ALLOW["doom"] = DOOM
V.TARGET_IMPORT_ALLOW["doom_snd"] = DOOM | SOUND

if __name__ == "__main__":
    V.main()
