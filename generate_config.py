#!/usr/bin/env python3

"""Generate a C++ configuration header from an XML configuration file."""

import sys
import xml.etree.ElementTree as ET
from pathlib import Path

# ---------------------------------------------------------------------------
# Default configuration
# ---------------------------------------------------------------------------

# Values used when the corresponding XML element or field is absent or empty.
# Component defaults also provide the initial definitions for the components
# that are included unless replaced or extended by the XML configuration.
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


# ---------------------------------------------------------------------------
# C++ source formatting
# ---------------------------------------------------------------------------


def to_pascal_case(name: str) -> str:
    """Convert an underscore-separated name to PascalCase.

    Each component of the name is capitalized and the components are
    concatenated without separators.
    """
    return "".join(part.capitalize() for part in name.split("_"))


def cpp_string(value: str) -> str:
    """Return a Python string escaped and quoted for a C++ string literal."""
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def cpp_float(value: float) -> str:
    """Format a numeric value as a C++ float literal with an 'f' suffix.

    The value is formatted to eight significant digits. An explicit decimal
    point is added when the formatted value contains neither a decimal point
    nor an exponent.
    """
    text = format(float(value), ".8g")

    if "." not in text and "e" not in text.lower():
        text += ".0"

    return text + "f"


# ---------------------------------------------------------------------------
# XML field extraction
# ---------------------------------------------------------------------------


def get_text(element, field, fallback):
    """Return a non-empty XML child value, or its fallback as text.

    If the parent element is absent, the child is missing, or the child's
    text is absent or whitespace-only, the fallback is returned as a string.
    """
    if element is None:
        return str(fallback)

    child = element.find(field)

    if child is None or child.text is None or not child.text.strip():
        return str(fallback)

    return child.text.strip()


def get_int(element, field, fallback):
    """Read an integer field from XML, using the fallback when necessary.

    Decimal integers and integers beginning with the '0x' prefix are
    supported. Invalid integer values raise ValueError.
    """
    text = get_text(element, field, fallback)

    try:
        return int(text, 0) if text.lower().startswith("0x") else int(text)
    except ValueError:
        raise ValueError(f"Invalid integer for '{field}': {text!r}") from None


def get_float(element, field, fallback):
    """Read a finite floating-point value from XML or use its fallback."""
    text = get_text(element, field, fallback)

    try:
        value = float(text)
    except ValueError:
        raise ValueError(f"Invalid number for '{field}': {text!r}") from None

    import math

    # Reject NaN and positive or negative infinity.
    if not math.isfinite(value):
        raise ValueError(f"'{field}' must be finite")

    return value


# ---------------------------------------------------------------------------
# Configuration validation
# ---------------------------------------------------------------------------


def validate_config(baud, address, frequency, servo_min, servo_max, components):
    """Validate global settings and component definitions.

    Raises ValueError when a configured value violates one of the constraints
    checked below. The function does not modify the supplied configuration.
    """
    # Validate the serial baud rate.
    if not 1 <= baud <= 4_000_000:
        raise ValueError("baud_rate must be between 1 and 4000000")

    # Validate the seven-bit I2C address range accepted by this generator.
    if not 0x03 <= address <= 0x77:
        raise ValueError("PCA9685 I2C address must be a valid 7-bit address")

    # Validate the configured PCA9685 frequency.
    if not 1 <= frequency <= 1000:
        raise ValueError("PCA9685 frequency must be between 1 and 1000 Hz")

    # PCA9685 PWM counts range from 0 through 4095. The minimum pulse count
    # must also be strictly less than the maximum pulse count.
    if not 0 <= servo_min < servo_max <= 4095:
        raise ValueError("servo_min/servo_max must satisfy 0 <= min < max <= 4095")

    # Track identifiers already encountered to detect duplicate component IDs.
    ids = set()

    for name, item in components.items():
        component_id = item["id"]
        channel = item["channel"]
        mirror = item["mirror_channel"]
        minimum = item["min"]
        maximum = item["max"]
        default = item["default"]

        # Component IDs are transmitted as an unsigned eight-bit value.
        if not 0 <= component_id <= 255:
            raise ValueError(f"{name}: id must be between 0 and 255")

        if component_id in ids:
            raise ValueError(f"Duplicate component ID: {component_id}")

        ids.add(component_id)

        # Validate the primary PCA9685 channel.
        if not 0 <= channel <= 15:
            raise ValueError(f"{name}: channel must be between 0 and 15")

        # A mirror value of -1 represents an absent mirror channel.
        if mirror != -1 and not 0 <= mirror <= 15:
            raise ValueError(f"{name}: mirror_channel must be between 0 and 15")

        # Prevent the primary and mirror channels from being identical.
        if mirror == channel:
            raise ValueError(f"{name}: channel and mirror_channel cannot be identical")

        # The angular interval must have a positive width.
        if not minimum < maximum:
            raise ValueError(f"{name}: min must be less than max")

        # The configured startup angle must lie inside the angular interval.
        if not minimum <= default <= maximum:
            raise ValueError(f"{name}: default must be between min and max")


# ---------------------------------------------------------------------------
# Configuration loading and header generation
# ---------------------------------------------------------------------------


def main():
    """Load XML settings, validate them, and generate a C++ header."""
    # Require the input XML path and output header path as command-line
    # arguments, in that order.
    if len(sys.argv) != 3:
        print(
            f"Usage: {sys.argv[0]} <config.xml> <output.hpp>",
            file=sys.stderr,
        )
        sys.exit(1)

    config_path = Path(sys.argv[1])
    output_path = Path(sys.argv[2])

    # Verify that the requested input path identifies an existing file.
    if not config_path.is_file():
        raise FileNotFoundError(f"Configuration file not found: {config_path}")

    # Parse the XML document and obtain its root element.
    root = ET.parse(config_path).getroot()

    if root.tag != "config":
        raise ValueError("Root element must be <config>")

    # -----------------------------------------------------------------------
    # Arduino settings
    # -----------------------------------------------------------------------

    # Read serial settings from the <arduino> element, falling back to
    # DEFAULTS when the element or an individual field is absent or empty.
    arduino = root.find("arduino")
    arduino_defaults = DEFAULTS["arduino"]

    port = get_text(arduino, "port", arduino_defaults["port"])
    baud = get_int(arduino, "baud_rate", arduino_defaults["baud_rate"])

    # -----------------------------------------------------------------------
    # PCA9685 settings
    # -----------------------------------------------------------------------

    # Read the I2C address, PWM frequency, and pulse-count limits.
    pca = root.find("pca9685")
    pca_defaults = DEFAULTS["pca9685"]

    address = get_int(pca, "address", pca_defaults["address"])
    frequency = get_int(pca, "frequency", pca_defaults["frequency"])
    servo_min = get_int(pca, "servo_min", pca_defaults["servo_min"])
    servo_max = get_int(pca, "servo_max", pca_defaults["servo_max"])

    # -----------------------------------------------------------------------
    # Component settings
    # -----------------------------------------------------------------------

    # Copy the default component definitions so XML overrides do not modify
    # the DEFAULTS dictionary used by subsequent calls to main().
    components = {
        name: values.copy() for name, values in DEFAULTS["components"].items()
    }

    # Process each direct child of <component> as a component definition.
    component_section = root.find("component")

    if component_section is not None:
        for component_element in component_section:
            name = component_element.tag

            if name not in components:
                # Create a baseline definition for an unrecognized component
                # name. Its ID and channel must be explicitly provided in XML
                # to replace these initial values.
                components[name] = {
                    "id": 0,
                    "channel": 0,
                    "mirror_channel": -1,
                    "min": 0,
                    "max": 180,
                    "default": 90,
                }

            values = components[name]

            # Read the integer-valued component fields, retaining their
            # current values when the corresponding XML child is absent.
            for field in ("id", "channel", "min", "max", "default"):
                values[field] = get_int(
                    component_element,
                    field,
                    values[field],
                )

            # Read mirror_channel separately because an explicitly empty
            # element is interpreted as disabling the mirror channel.
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

    # Reject invalid settings before producing the output header.
    validate_config(
        baud,
        address,
        frequency,
        servo_min,
        servo_max,
        components,
    )

    # Sort by component ID so the generated definitions have a predictable
    # order independent of the order in which components appeared in the XML.
    components = dict(sorted(components.items(), key=lambda item: item[1]["id"]))

    # -----------------------------------------------------------------------
    # Generate the C++ header preamble and global configuration
    # -----------------------------------------------------------------------

    # The generated header contains the global settings, a component ID enum,
    # the ComponentConfig structure, the component definition array, and
    # inline lookup and validation helpers.
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

    # Generate one enum member for each configured component. The component
    # name is converted to PascalCase and its configured ID is preserved.
    for name, values in components.items():
        output += f"    {to_pascal_case(name)} = {values['id']},\n"

    # Define the data structure shared by the generated component array
    # and the Arduino code that consumes the configuration.
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

    # Generate one aggregate initializer for each component. Numeric angle
    # values are formatted as C++ float literals by cpp_float().
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

    # Add the component count and inline lookup functions used by the
    # Arduino receiver to resolve component identifiers.
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

    # Create missing parent directories and write the generated header as
    # UTF-8 text. Existing output at the destination path is overwritten.
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(output, encoding="utf-8")

    # Report the output path and selected settings after successful generation.
    print(f"Generated {output_path}")
    print(f"Components: {len(components)}")
    print(f"Baud rate: {baud}")
    print(f"PCA9685 address: 0x{address:02X}")


# ---------------------------------------------------------------------------
# Script entry point and error reporting
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, ET.ParseError) as error:
        # Report configuration, filesystem, and XML parsing errors to stderr.
        # A nonzero exit status indicates that generation did not complete.
        print(f"Configuration error: {error}", file=sys.stderr)
        sys.exit(1)
