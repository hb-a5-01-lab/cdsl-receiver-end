#!/usr/bin/env python3

import sys
import xml.etree.ElementTree as ET
from pathlib import Path

DEFAULTS = {
    "arduino": {
        "port": "/dev/ttyUSB0",
        "baud_rate": 115200,
    },
    "pca9685": {
        "address": 0x40,
        "frequency": 50,
        "servo_min": 130,
        "servo_max": 520,
    },
    "components": {
        "base": {
            "id": 0,
            "channel": 0,
            "mirror_channel": -1,
            "min": 0,
            "max": 180,
            "default": 90,
        },
        "shoulder": {
            "id": 1,
            "channel": 1,
            "mirror_channel": 2,
            "min": 0,
            "max": 180,
            "default": 90,
        },
        "elbow": {
            "id": 2,
            "channel": 3,
            "mirror_channel": -1,
            "min": 0,
            "max": 180,
            "default": 90,
        },
        "wrist": {
            "id": 3,
            "channel": 4,
            "mirror_channel": -1,
            "min": 0,
            "max": 180,
            "default": 90,
        },
        "palm": {
            "id": 4,
            "channel": 5,
            "mirror_channel": -1,
            "min": 0,
            "max": 180,
            "default": 90,
        },
        "claw": {
            "id": 5,
            "channel": 6,
            "mirror_channel": -1,
            "min": 0,
            "max": 90,
            "default": 0,
        },
    },
}


def to_pascal_case(name: str) -> str:
    return "".join(part.capitalize() for part in name.split("_"))


def cpp_string(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def cpp_float(value: float) -> str:
    text = format(float(value), ".8g")

    if "." not in text and "e" not in text.lower():
        text += ".0"

    return text + "f"


def get_text(element, field, fallback):
    if element is None:
        return str(fallback)

    child = element.find(field)

    if child is None or child.text is None or not child.text.strip():
        return str(fallback)

    return child.text.strip()


def get_int(element, field, fallback):
    text = get_text(element, field, fallback)

    try:
        return int(text, 0) if text.lower().startswith("0x") else int(text)
    except ValueError:
        raise ValueError(f"Invalid integer for '{field}': {text!r}") from None


def get_float(element, field, fallback):
    text = get_text(element, field, fallback)

    try:
        value = float(text)
    except ValueError:
        raise ValueError(f"Invalid number for '{field}': {text!r}") from None

    import math

    if not math.isfinite(value):
        raise ValueError(f"'{field}' must be finite")

    return value


def validate_config(baud, address, frequency, servo_min, servo_max, components):
    if not 1 <= baud <= 4_000_000:
        raise ValueError("baud_rate must be between 1 and 4000000")

    if not 0x03 <= address <= 0x77:
        raise ValueError("PCA9685 I2C address must be a valid 7-bit address")

    if not 1 <= frequency <= 1000:
        raise ValueError("PCA9685 frequency must be between 1 and 1000 Hz")

    if not 0 <= servo_min < servo_max <= 4095:
        raise ValueError("servo_min/servo_max must satisfy 0 <= min < max <= 4095")

    ids = set()

    for name, item in components.items():
        component_id = item["id"]
        channel = item["channel"]
        mirror = item["mirror_channel"]
        minimum = item["min"]
        maximum = item["max"]
        default = item["default"]

        if not 0 <= component_id <= 255:
            raise ValueError(f"{name}: id must be between 0 and 255")

        if component_id in ids:
            raise ValueError(f"Duplicate component ID: {component_id}")

        ids.add(component_id)

        if not 0 <= channel <= 15:
            raise ValueError(f"{name}: channel must be between 0 and 15")

        if mirror != -1 and not 0 <= mirror <= 15:
            raise ValueError(f"{name}: mirror_channel must be between 0 and 15")

        if mirror == channel:
            raise ValueError(f"{name}: channel and mirror_channel cannot be identical")

        if not minimum < maximum:
            raise ValueError(f"{name}: min must be less than max")

        if not minimum <= default <= maximum:
            raise ValueError(f"{name}: default must be between min and max")


def main():
    if len(sys.argv) != 3:
        print(
            f"Usage: {sys.argv[0]} <config.xml> <output.hpp>",
            file=sys.stderr,
        )
        sys.exit(1)

    config_path = Path(sys.argv[1])
    output_path = Path(sys.argv[2])

    if not config_path.is_file():
        raise FileNotFoundError(f"Configuration file not found: {config_path}")

    root = ET.parse(config_path).getroot()

    if root.tag != "config":
        raise ValueError("Root element must be <config>")

    # Arduino configuration.
    arduino = root.find("arduino")
    arduino_defaults = DEFAULTS["arduino"]

    port = get_text(arduino, "port", arduino_defaults["port"])
    baud = get_int(arduino, "baud_rate", arduino_defaults["baud_rate"])

    # PCA9685 configuration.
    pca = root.find("pca9685")
    pca_defaults = DEFAULTS["pca9685"]

    address = get_int(pca, "address", pca_defaults["address"])
    frequency = get_int(pca, "frequency", pca_defaults["frequency"])
    servo_min = get_int(pca, "servo_min", pca_defaults["servo_min"])
    servo_max = get_int(pca, "servo_max", pca_defaults["servo_max"])

    # Start with defaults, then override them with XML entries.
    components = {
        name: values.copy() for name, values in DEFAULTS["components"].items()
    }

    component_section = root.find("component")

    if component_section is not None:
        for component_element in component_section:
            name = component_element.tag

            if name not in components:
                # New components must provide their own ID and channel.
                components[name] = {
                    "id": 0,
                    "channel": 0,
                    "mirror_channel": -1,
                    "min": 0,
                    "max": 180,
                    "default": 90,
                }

            values = components[name]

            for field in ("id", "channel", "min", "max", "default"):
                values[field] = get_int(
                    component_element,
                    field,
                    values[field],
                )

            mirror_element = component_element.find("mirror_channel")

            if mirror_element is not None:
                if mirror_element.text is None or not mirror_element.text.strip():
                    values["mirror_channel"] = -1
                else:
                    try:
                        values["mirror_channel"] = int(mirror_element.text.strip())
                    except ValueError:
                        raise ValueError(
                            f"{name}: mirror_channel must be an integer"
                        ) from None

    validate_config(
        baud,
        address,
        frequency,
        servo_min,
        servo_max,
        components,
    )

    # Sort components by ID for predictable generated output.
    components = dict(sorted(components.items(), key=lambda item: item[1]["id"]))

    output = f"""\
#pragma once

#include <stdint.h>
#include <stddef.h>

// Generated from config.xml. Do not edit manually.

// Arduino configuration.
static const char ARDUINO_PORT[] = {cpp_string(port)};
constexpr uint32_t BAUD_RATE = {baud}UL;

// PCA9685 configuration.
constexpr uint8_t PCA9685_ADDRESS = 0x{address:02X};
constexpr uint16_t PCA9685_FREQUENCY = {frequency};
constexpr uint16_t SERVO_MIN_PULSE = {servo_min};
constexpr uint16_t SERVO_MAX_PULSE = {servo_max};

// Component identifiers.
enum class ComponentId : uint8_t {{
"""

    for name, values in components.items():
        output += f"    {to_pascal_case(name)} = {values['id']},\n"

    output += """\
};

struct ComponentConfig {
    ComponentId id;
    const char* name;
    uint8_t channel;
    int16_t mirror_channel; // -1 means no mirror
    float min_angle;
    float max_angle;
    float default_angle;
};

static const ComponentConfig component_configs[] = {
"""

    for name, values in components.items():
        mirror = values["mirror_channel"]

        output += (
            f"    {{ComponentId::{to_pascal_case(name)}, "
            f"{cpp_string(name)}, "
            f"{values['channel']}, "
            f"{mirror}, "
            f"{cpp_float(values['min'])}, "
            f"{cpp_float(values['max'])}, "
            f"{cpp_float(values['default'])}}},\n"
        )

    output += """\
};

constexpr size_t COMPONENT_COUNT =
    sizeof(component_configs) / sizeof(component_configs[0]);

inline const ComponentConfig* get_component_config(
    ComponentId component
) {
    for (size_t i = 0; i < COMPONENT_COUNT; ++i) {
        if (component_configs[i].id == component) {
            return &component_configs[i];
        }
    }

    return nullptr;
}

inline const ComponentConfig* get_component_config_by_id(uint8_t id) {
    return get_component_config(static_cast<ComponentId>(id));
}

inline bool is_valid_component_id(uint8_t id) {
    return get_component_config_by_id(id) != nullptr;
}

inline const char* component_name(ComponentId component) {
    const ComponentConfig* config = get_component_config(component);
    return config != nullptr ? config->name : "unknown";
}
"""

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(output, encoding="utf-8")

    print(f"Generated {output_path}")
    print(f"Components: {len(components)}")
    print(f"Baud rate: {baud}")
    print(f"PCA9685 address: 0x{address:02X}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, ET.ParseError) as error:
        print(f"Configuration error: {error}", file=sys.stderr)
        sys.exit(1)
