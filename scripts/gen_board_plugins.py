#!/usr/bin/env python3
"""
Generate espd_board_* plugin directories from boards/*.yaml.

Run automatically from root CMakeLists.txt before Kconfig / component discovery.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

try:
    import yaml
except ImportError as exc:  # pragma: no cover
    raise SystemExit(
        "PyYAML is required (install in IDF Python env: pip install pyyaml)"
    ) from exc

ID_RE = re.compile(r"^[a-z][a-z0-9_]*$")
GENERATED_HEADER = "# Auto-generated from {src} — do not edit.\n"

_USB_OTG_TARGETS = {"esp32s3", "esp32c3", "esp32c6", "esp32h2", "esp32c5", "esp32p4"}
_WIFI_USB_TARGETS = {"esp32s3", "esp32c3", "esp32c6", "esp32h2", "esp32c5"}

@dataclass
class PeripheralSchema:
    required: list[str]

_SUPPORTED_PERIPHERALS = {
    "sensor": PeripheralSchema(required=["bsp_getter", "rate_ms", "outputs"]),
    "actuator": PeripheralSchema(required=["bsp_setter", "inputs"]),
}

def _gen_cmake_glue(data: dict, src: str) -> str:
    # CMake REQUIRES uses the bare component name (no namespace slashes)
    bsp_component = data["bsp"]["component"]
    peripherals = data.get("peripherals") or []
    no_ws2812 = data.get("bsp", {}).get("no_ws2812_leds", False)
    extra_srcs = ""
    if peripherals:
        extra_srcs = '\n            "espd_board_peripherals.c"'
    no_led_block = ""
    return f"""# Auto-generated from {src} — do not edit.

if(CONFIG_ESPD_BOARD_ESP_BSP_GLUE)
    idf_component_register(
        SRCS
            "espd_bsp_audio_glue.c"
            "espd_bsp_io_glue.c"{extra_srcs}
        INCLUDE_DIRS "." "${{CMAKE_CURRENT_LIST_DIR}}"
        REQUIRES espd_integration {bsp_component}
    ){no_led_block}
else()
    idf_component_register()
endif()
"""

AUDIO_GLUE_C = """\
/*
 * Auto-generated from {src} — do not edit.
 *
 * Board-owned glue TU to keep component source ownership clean while
 * reusing shared espd_integration implementation.
 */
#include "../espd_integration/espd_bsp_esp_bsp_audio.c"
"""

# Variant for boards that need the IO expander initialised before the codec
# regardless of BSP_CAPS_BUTTONS (e.g. Korvo-2 with TCA9554).
AUDIO_GLUE_C_IO_EXPANDER = """\
/*
 * Auto-generated from {src} — do not edit.
 *
 * Like espd_bsp_esp_bsp_audio.c but always calls bsp_io_expander_init()
 * before bsp_audio_init(), regardless of BSP_CAPS_BUTTONS.  Required for
 * boards (e.g. Korvo-2) whose TCA9554 IO expander must be initialised so
 * that the codec's I2C init does not time-out.
 */
#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "espd_bsp_audio.h"
#include "esp_check.h"
#include "esp_log.h"

#ifndef BSP_AUDIO_MCLK_MULTIPLE
#define BSP_AUDIO_MCLK_MULTIPLE I2S_MCLK_MULTIPLE_384
#endif

static const char *TAG = "espd_bsp";

esp_err_t espd_bsp_audio_hw_init(const espd_bsp_audio_hw_params_t *params,
    espd_bsp_audio_hw_t *hw)
{{
    i2s_std_config_t std_cfg;
    i2s_slot_mode_t slot_mode;
    uint32_t mclk_multiple;

    ESP_RETURN_ON_FALSE(params && hw, ESP_ERR_INVALID_ARG, TAG, "null arg");

    hw->spk = NULL;
    hw->mic = NULL;

    slot_mode = (params->channels >= 2) ? I2S_SLOT_MODE_STEREO
                                        : I2S_SLOT_MODE_MONO;

    std_cfg = (i2s_std_config_t){{
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(params->sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, slot_mode),
        .gpio_cfg = {{
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws   = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din  = BSP_I2S_DSIN,
            .invert_flags = {{
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            }},
        }},
    }};

    mclk_multiple = params->mclk_multiple ? params->mclk_multiple
                                         : BSP_AUDIO_MCLK_MULTIPLE;
    std_cfg.clk_cfg.mclk_multiple = mclk_multiple;

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "bsp_i2c_init");
    /* Always init IO expander — codec I2C hangs without it on this board. */
    ESP_RETURN_ON_FALSE(bsp_io_expander_init() != NULL, ESP_FAIL, TAG,
        "io expander");
    ESP_RETURN_ON_ERROR(bsp_audio_init(&std_cfg), TAG, "bsp_audio_init");

    hw->spk = bsp_audio_codec_speaker_init();
    ESP_RETURN_ON_FALSE(hw->spk, ESP_FAIL, TAG, "speaker codec init");

    hw->mic = bsp_audio_codec_microphone_init();
    if (!hw->mic)
        ESP_LOGW(TAG, "microphone codec init failed");

    return ESP_OK;
}}
"""

IO_GLUE_C = """\
/*
 * Auto-generated from {src} — do not edit.
 *
 * Board-owned glue TU to keep component source ownership clean while
 * reusing shared espd_integration implementation.
 */
#include "../espd_integration/espd_bsp_esp_bsp_io.c"
"""

# Variant for boards that own bsp_led_set() themselves (e.g. Korvo-2 with
# led_indicator): suppress the WS2812-style bsp_led_* declarations in bsp_io.h
# so they don't conflict with the BSP's own declaration.
IO_GLUE_C_NO_LED = """\
/*
 * Auto-generated from {src} — do not edit.
 *
 * Like espd_bsp_esp_bsp_io.c but suppresses the WS2812 bsp_led_* declarations
 * in bsp/bsp_io.h to avoid conflicts with BSPs that define their own
 * bsp_led_set() (e.g. Korvo-2 uses led_indicator, not a LED strip).
 */
/* Suppress WS2812 bsp_led_* declarations — the BSP defines its own. */
#define ESPD_BSP_IO_NO_LED_DECL
#include "../espd_integration/espd_bsp_esp_bsp_io.c"
"""


def _config_key(key: str) -> str:
    key = str(key).strip()
    if key.startswith("CONFIG_"):
        return key[len("CONFIG_") :]
    return key


def _kconfig_value(value):
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        s = value.strip()
        if s in ("y", "Y", "yes", "true"):
            return True
        if s in ("n", "N", "no", "false"):
            return False
        return s
    return value


def _config_line(key: str, value) -> str:
    k = _config_key(key)
    value = _kconfig_value(value)
    if value is None or value == "":
        return f"CONFIG_{k}=\n"
    if isinstance(value, bool):
        return f"CONFIG_{k}={'y' if value else 'n'}\n"
    if isinstance(value, str) and not (value[0] in '"\'' and value[-1] in "'\""):
        return f'CONFIG_{k}="{value}"\n'
    return f"CONFIG_{k}={value}\n"


def _feature_implied(data: dict, symbol: str) -> bool:
    profile = data.get("profile") or {}
    for options in profile.values():
        if isinstance(options, dict):
            val = options.get(symbol)
            if val is not None:
                return bool(_kconfig_value(val))
    return symbol in (data.get("features") or {}).get("imply") or []


def _profile_has_usb_otg(data: dict) -> bool:
    return _feature_implied(data, "ESPD_USE_USB_OTG")


def _profile_has_wifi(data: dict) -> bool:
    return _feature_implied(data, "ESPD_USE_WIFI")


def _otg_sdkconfig_extras(data: dict, profile_keys: set[str]) -> list[tuple[str, object]]:
    """Hand the shared USB PHY to OTG: console off, USB Serial/JTAG off.

    These are console *choice* members and a driver toggle that Kconfig
    ``select`` cannot set, so boards no longer list them — we derive them from
    ESPD_USE_USB_OTG. A board can still override the primary/secondary console
    by putting any ESP_CONSOLE_* member in its profile (e.g. keep a UART
    primary on a board that exposes one), and the matching default is skipped.
    """
    # Check if profile explicitly disables USB OTG (overrides features.imply)
    profile = data.get("profile") or {}
    usb_otg_disabled = False
    usb_otg_explicit = False
    for options in profile.values():
        if isinstance(options, dict):
            val = options.get("ESPD_USE_USB_OTG")
            if val is not None:
                usb_otg_explicit = True
                if not _kconfig_value(val):
                    usb_otg_disabled = True
                    break
    
    if not _profile_has_usb_otg(data) or usb_otg_disabled or str(data.get("target")) not in _USB_OTG_TARGETS:
        return []
    
    _CONSOLE_PRIMARY_MEMBERS = {
        "ESP_CONSOLE_UART_DEFAULT",
        "ESP_CONSOLE_USB_CDC",
        "ESP_CONSOLE_USB_SERIAL_JTAG",
        "ESP_CONSOLE_UART_CUSTOM",
        "ESP_CONSOLE_NONE",
    }
    _CONSOLE_SECONDARY_MEMBERS = {
        "ESP_CONSOLE_SECONDARY_NONE",
        "ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG",
    }
    
    extras: list[tuple[str, object]] = []
    # Skip console handoff if profile explicitly sets any console option or dev sync option
    # (allows boards to override features.imply for console configuration)
    if profile_keys & _CONSOLE_PRIMARY_MEMBERS:
        # Profile has explicit console setting, skip handoff
        return []
    if "ESPD_DEV_SYNC" in profile_keys or "ESPD_DEV_SERIAL_SYNC" in profile_keys or "ESPD_DEV_CDC_SYNC" in profile_keys:
        # Profile has explicit dev sync setting, skip console handoff (may come from chip defaults)
        return []
    # If USB OTG is only in features.imply (not explicit in profile), be conservative
    # and don't disable console to allow chip defaults (e.g., serial sync) to work
    if not usb_otg_explicit:
        return []
    if not (profile_keys & _CONSOLE_PRIMARY_MEMBERS):
        extras.append(("ESP_CONSOLE_NONE", True))
    if not (profile_keys & _CONSOLE_SECONDARY_MEMBERS):
        extras.append(("ESP_CONSOLE_SECONDARY_NONE", True))
    usj_in_profile = "USJ_ENABLE_USB_SERIAL_JTAG" in profile_keys
    if not usj_in_profile and (
        "ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG" not in profile_keys
        and "ESP_CONSOLE_USB_SERIAL_JTAG" not in profile_keys
    ):
        extras.append(("USJ_ENABLE_USB_SERIAL_JTAG", False))
    return extras


def _wifi_usb_coexist_extras(data: dict, profile_keys: set[str]) -> list[tuple[str, object]]:
    """S3/C3: Wi-Fi PHY init disables USB unless ESP_PHY_ENABLE_USB (IDF default n
    when console is not USB Serial/JTAG). Required for OTG CDC after esp_wifi_init()."""
    if not _profile_has_usb_otg(data) or not _profile_has_wifi(data):
        return []
    if str(data.get("target")) not in _WIFI_USB_TARGETS:
        return []
    if "ESP_PHY_ENABLE_USB" in profile_keys:
        return []
    return [("ESP_PHY_ENABLE_USB", True)]


def _load_board(path: Path) -> dict:
    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"{path}: expected mapping at top level")
    return data


def _validate_peripherals(data: dict, path: Path) -> None:
    peripherals = data.get("peripherals") or []
    if not isinstance(peripherals, list):
        raise ValueError(f"{path}: peripherals must be a list of mappings")
    for i, p in enumerate(peripherals):
        if not isinstance(p, dict):
            raise ValueError(f"{path}: peripheral item {i} must be a mapping")
        ptype = p.get("type")
        if not ptype:
            raise ValueError(f"{path}: peripheral at index {i} missing 'type'")
        if ptype not in _SUPPORTED_PERIPHERALS:
            raise ValueError(
                f"{path}: unrecognized peripheral type {ptype!r} at index {i}. "
                f"Supported types: {list(_SUPPORTED_PERIPHERALS.keys())}"
            )
        
        schema = _SUPPORTED_PERIPHERALS[ptype]
        for req in schema.required:
            if req not in p:
                raise ValueError(f"{path}: peripheral {ptype!r} at index {i} missing required key {req!r}")
        
        # Validate list types for known keys
        for list_key in ("outputs", "inputs"):
            if list_key in p and not isinstance(p[list_key], list):
                raise ValueError(f"{path}: peripheral {list_key} at index {i} must be a list")


def _validate_board(data: dict, path: Path) -> None:
    board_id = data.get("id")
    if not board_id or not ID_RE.match(str(board_id)):
        raise ValueError(f"{path}: id must match {ID_RE.pattern!r}")
    if not data.get("name"):
        raise ValueError(f"{path}: missing name")
    if not data.get("target"):
        raise ValueError(f"{path}: missing target")
    bsp = data.get("bsp")
    if not isinstance(bsp, dict) or not bsp.get("component"):
        raise ValueError(f"{path}: bsp.component is required")
    _validate_peripherals(data, path)


def _kconfig_symbol(board_id: str) -> str:
    return f"ESPD_BOARD_{board_id.upper()}"


def _gen_kconfig(data: dict, src: str) -> str:
    sym = _kconfig_symbol(data["id"])
    target = data["target"]
    name = data["name"]
    help_text = (data.get("help") or name).strip()
    bsp = data.get("bsp", {})
    
    # Only select ESPD_BOARD_ESP_BSP_GLUE for non-local BSPs
    # Local BSPs (path: local) don't provide bsp/esp-bsp.h
    select_bsp_glue = bsp.get("path") != "local"
    
    lines = [
        GENERATED_HEADER.format(src=src),
        f"config {sym}\n",
        f'    bool "{name}"\n',
        f"    depends on IDF_TARGET_{target.upper()}\n",
    ]
    if select_bsp_glue:
        lines.append("    select ESPD_BOARD_ESP_BSP_GLUE\n")
    for feat in data.get("features", {}).get("imply", []) or []:
        lines.append(f"    imply {feat}\n")
    lines.append("    help\n")
    for hl in help_text.splitlines():
        lines.append(f"        {hl}\n")
    return "".join(lines)


def _gen_idf_component_yml(data: dict, src: str) -> str:
    bsp = data["bsp"]
    comp = bsp["component"]
    # registry_component allows a namespaced key like "espressif/foo" for the
    # idf_component.yml dependency entry, while cmake REQUIRES uses comp (no /)
    registry_comp = bsp.get("registry_component", comp)
    lines = [
        GENERATED_HEADER.format(src=src),
        'version: "0.1.0"\n',
        f'description: {data["name"]} board plugin for espd\n',
        "dependencies:\n",
        '  idf: ">=6.0.1,<6.1"\n',
        "  espd_integration:\n",
        "    path: ../espd_integration\n",
        f"  {registry_comp}:\n",
    ]
    if "git" in bsp:
        lines.append(f'    git: {bsp["git"]}\n')
        if "path" in bsp:
            lines.append(f'    path: {bsp["path"]}\n')
        if "version" in bsp:
            lines.append(f'    version: {bsp["version"]}\n')
    elif "registry" in bsp or "version" in bsp:
        ver = bsp.get("registry") or bsp.get("version")
        lines.append(f"    version: \"{ver}\"\n")
    elif bsp.get("path") == "local":
        # Local BSP from local_components/
        lines.append(f'    path: ../../local_components/{comp}\n')
    else:
        raise ValueError(f"{src}: bsp needs git, registry version, or path: local")
    return "".join(lines)


def _gen_sdkconfig_defaults(data: dict, src: str) -> str:
    sym = _kconfig_symbol(data["id"])
    lines = [
        GENERATED_HEADER.format(src=src),
        f"# Merged when CONFIG_{sym}=y (see root CMakeLists.txt).\n",
        "\n",
    ]
    profile = data.get("profile") or {}
    if not isinstance(profile, dict):
        raise ValueError(f"{src}: profile must be a mapping")
    profile_keys = {
        _config_key(k)
        for options in profile.values()
        if isinstance(options, dict)
        for k in options
    }
    imply_feats = (data.get("features") or {}).get("imply") or []
    feat_defaults = [f for f in imply_feats if _config_key(f) not in profile_keys]
    if feat_defaults:
        lines.append("# --- ESPD features (from features.imply) ---\n\n")
        for feat in feat_defaults:
            lines.append(_config_line(feat, True))
        lines.append("\n")
    
    # Cache feature checks
    has_usb_otg = _profile_has_usb_otg(data)
    has_wifi = _profile_has_wifi(data)
    
    extras = _otg_sdkconfig_extras(data, profile_keys) if has_usb_otg else []
    if extras:
        lines.append("# --- USB OTG console handoff (auto) ---\n\n")
        for key, value in extras:
            lines.append(_config_line(key, value))
        lines.append("\n")
    coexist = _wifi_usb_coexist_extras(data, profile_keys) if has_usb_otg and has_wifi else []
    if coexist:
        lines.append("# --- Wi-Fi + OTG (auto) ---\n\n")
        for key, value in coexist:
            lines.append(_config_line(key, value))
        lines.append("\n")
    for section, options in profile.items():
        lines.append(f"# --- {section} ---\n\n")
        if not isinstance(options, dict):
            raise ValueError(f"{src}: profile.{section} must be a mapping")
        for key, value in options.items():
            lines.append(_config_line(key, value))
        lines.append("\n")
    return "".join(lines)


def _gen_io_config(data: dict, src: str) -> str | None:
    io = data.get("io") or {}
    buttons = io.get("buttons")
    if not buttons:
        return None
    if not isinstance(buttons, list) or not buttons:
        raise ValueError(f"{src}: io.buttons must be a non-empty list")
    board_id = data["id"]
    entries = ", \\\n    ".join(f"BSP_BUTTON_{b}" for b in buttons)
    return (
        "/*\n"
        f" * Auto-generated from {src} — do not edit.\n"
        " *\n"
        f" * Button map for espd_board_{board_id}.\n"
        " */\n"
        "#pragma once\n\n"
        '#include "bsp/esp-bsp.h"\n\n'
        f"#define ESPD_BSP_BUTTON_COUNT {len(buttons)}\n"
        f"#define ESPD_BSP_BUTTON_MAP {{ \\\n    {entries}, \\\n}}\n"
    )


def _write_if_changed(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return
    path.write_text(content, encoding="utf-8")


def _gen_sensor_code(p: dict, idx: int) -> tuple[str, str]:
    name = p.get("name", f"device_{idx}")
    getter = p["bsp_getter"]
    rate = p["rate_ms"]
    outputs = p["outputs"]
    
    var_decls = ", ".join(f"val_{i} = 0.0f" for i in range(len(outputs)))
    var_ptrs = ", ".join(f"&val_{i}" for i in range(len(outputs)))
    send_calls = "\n".join(f'        _espd_send_pd_float("{sym}", val_{i});' 
                          for i, sym in enumerate(outputs))
    
    task = f"""        // Poll sensor {name} using BSP getter {getter}
        {{
            float {var_decls};
            {getter}({var_ptrs});
{send_calls}
            vTaskDelay(pdMS_TO_TICKS({rate}));
        }}"""
    return "", task


def _gen_actuator_code(p: dict, idx: int) -> tuple[str, str]:
    name = p.get("name", f"device_{idx}")
    setter = p["bsp_setter"]
    inputs = p["inputs"]
    
    global_code = []
    init_code = []
    
    for i, pd_sym in enumerate(inputs):
        global_code.append(f"""
// Actuator {name} callback for index {i}
static t_class *actuator_class_{name}_{i};
typedef struct _actuator_recv_{name}_{i} {{
    t_pd x_pd;
}} t_actuator_recv_{name}_{i};
static t_actuator_recv_{name}_{i} actuator_instance_{name}_{i};

extern void {setter}(float val);

static void actuator_recv_val_{name}_{i}(t_actuator_recv_{name}_{i} *x, t_float f) {{
    {setter}((float)f);
}}
""")
        init_code.append(f"""
    // Bind Pd receiver for {pd_sym} to {setter}
    actuator_class_{name}_{i} = class_new(gensym("_actuator_{name}_{i}"), 0, 0, sizeof(t_actuator_recv_{name}_{i}), CLASS_PD, 0);
    class_addfloat(actuator_class_{name}_{i}, (t_method)actuator_recv_val_{name}_{i});
    actuator_instance_{name}_{i}.x_pd = actuator_class_{name}_{i};
    pd_bind((t_pd *)&actuator_instance_{name}_{i}, gensym("{pd_sym}"));
""")
    
    return "\n".join(global_code), "\n".join(init_code)


def _gen_peripherals_c(data: dict, src: str) -> str:
    peripherals = data.get("peripherals") or []
    if not peripherals:
        return ""
    
    init_code = []
    task_code = []
    global_code = []
    
    bsp_header = data.get("bsp", {}).get("header", "bsp/esp-bsp.h")
    
    for i, p in enumerate(peripherals):
        ptype = p["type"]
        if ptype == "sensor":
            g, t = _gen_sensor_code(p, i)
            task_code.append(t)
        elif ptype == "actuator":
            g, t = _gen_actuator_code(p, i)
            global_code.append(g)
            init_code.append(t)
    
    globals_str = "\n".join(global_code)
    inits_str = "\n".join(init_code)
    tasks_str = "\n".join(task_code)
    
    return f"""/*
 * Auto-generated from {src} — do not edit.
 *
 * Board custom peripherals & actuators driver task (isolated to Core 0).
 */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "{bsp_header}"
#include "../pd/src/m_pd.h"

static const char *TAG = "espd_peripherals";

static void _espd_send_pd_float(const char *name, float val)
{{
    t_symbol *sym = gensym(name);
    if (sym && sym->s_thing) {{
        pd_float(sym->s_thing, (t_float)val);
    }}
}}

{globals_str}

static void espd_peripherals_task(void *arg)
{{
    (void)arg;
    ESP_LOGI(TAG, "peripherals polling task started on Core 0");
    for (;;) {{
{tasks_str}
    }}
}}

void espd_board_peripherals_init(void)
{{
{inits_str}

    xTaskCreatePinnedToCore(espd_peripherals_task, "espd_periph", 4096, NULL, 3, NULL, 0);
}}
"""


def _generate_board(repo: Path, yaml_path: Path) -> Path:
    try:
        rel_src = yaml_path.relative_to(repo).as_posix()
    except ValueError:
        rel_src = f"boards/{yaml_path.name}"
    data = _load_board(yaml_path)
    _validate_board(data, yaml_path)

    out_dir = repo / "components" / f"espd_board_{data['id']}"
    _write_if_changed(out_dir / "Kconfig.board", _gen_kconfig(data, rel_src))
    _write_if_changed(out_dir / "idf_component.yml", _gen_idf_component_yml(data, rel_src))
    _write_if_changed(
        out_dir / "CMakeLists.txt",
        _gen_cmake_glue(data, rel_src),
    )
    audio_tmpl = (
        AUDIO_GLUE_C_IO_EXPANDER
        if data.get("bsp", {}).get("io_expander_before_audio")
        else AUDIO_GLUE_C
    )
    _write_if_changed(out_dir / "espd_bsp_audio_glue.c", audio_tmpl.format(src=rel_src))
    io_tmpl = (
        IO_GLUE_C_NO_LED
        if data.get("bsp", {}).get("no_ws2812_leds")
        else IO_GLUE_C
    )
    _write_if_changed(out_dir / "espd_bsp_io_glue.c", io_tmpl.format(src=rel_src))
    _write_if_changed(out_dir / "sdkconfig.defaults", _gen_sdkconfig_defaults(data, rel_src))

    io_cfg = _gen_io_config(data, rel_src)
    io_path = out_dir / "espd_board_io_config.h"
    if io_cfg:
        _write_if_changed(io_path, io_cfg)
    elif io_path.exists():
        io_path.unlink()

    periphs_c = _gen_peripherals_c(data, rel_src)
    periphs_path = out_dir / "espd_board_peripherals.c"
    if periphs_c:
        _write_if_changed(periphs_path, periphs_c)
    elif periphs_path.exists():
        periphs_path.unlink()

    return out_dir


def _parse_args(argv: list[str]) -> tuple[Path, Path]:
    parser = argparse.ArgumentParser()
    parser.add_argument("repo", nargs="?", default=".", type=Path)
    parser.add_argument("--boards-dir", type=Path)
    args = parser.parse_args(argv[1:])
    repo = args.repo.resolve()
    boards_dir = args.boards_dir.resolve() if args.boards_dir else repo / "boards"
    return repo, boards_dir


def main(argv: list[str]) -> int:
    repo, boards_dir = _parse_args(argv)
    if not boards_dir.is_dir():
        return 0

    yaml_files = sorted(
        p for p in boards_dir.glob("*.yaml") if p.name != "index.yaml"
    )
    if not yaml_files:
        return 0

    for yaml_path in yaml_files:
        out = _generate_board(repo, yaml_path)
        print(f"gen_board_plugins: {yaml_path} -> {out.relative_to(repo)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
