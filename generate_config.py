#!/usr/bin/env python3

import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def to_pascal_case(name: str) -> str:
    return "".join(part.capitalize() for part in name.split("_"))


def get_required_element(
    component: ET.Element,
    name: str,
    component_name: str,
) -> str:
    element = component.find(name)

    if element is None or element.text is None:
        raise ValueError(f"Component '{component_name}' is missing '{name}'")

    return element.text.strip()


def main() -> None:
    if len(sys.argv) != 3:
        print(
            f"usage: {sys.argv[0]} <config.xml> <output.hpp>",
            file=sys.stderr,
        )
        sys.exit(1)

    config_path = Path(sys.argv[1])
    output_path = Path(sys.argv[2])

    if not config_path.is_file():
        raise FileNotFoundError(f"Configuration file not found: {config_path}")

    tree = ET.parse(config_path)
    root = tree.getroot()

    # Extract and validate the Arduino baud rate.
    arduino_section = root.find("arduino")

    if arduino_section is None:
        raise ValueError("No <arduino> section found in config.xml")

    baud_rate_text = get_required_element(
        arduino_section,
        "baud_rate",
        "arduino",
    )

    try:
        baud_rate = int(baud_rate_text)
    except ValueError:
        raise ValueError("Arduino baud_rate must be an integer") from None

    if baud_rate <= 0:
        raise ValueError("Arduino baud_rate must be greater than zero")

    # Extract component IDs.
    component_section = root.find("component")

    if component_section is None:
        raise ValueError("No <component> section found in config.xml")

    component_data = []

    for component in component_section:
        name = component.tag

        try:
            component_id = int(get_required_element(component, "id", name))
        except ValueError:
            raise ValueError(f"Component '{name}' has a non-integer id") from None

        if not 0 <= component_id <= 255:
            raise ValueError(f"Component '{name}' id must be between 0 and 255")

        component_data.append((name, component_id))

    if not component_data:
        raise ValueError("No components found in config.xml")

    ids = [component_id for _, component_id in component_data]

    if len(ids) != len(set(ids)):
        raise ValueError("Duplicate component IDs found")

    component_data.sort(key=lambda item: item[1])

    # Generate the C++ header.
    output = f"""\
#pragma once

#include <stdint.h>

// Arduino serial communication configuration.
inline constexpr uint32_t BAUD_RATE = {baud_rate};

enum class ComponentId : uint8_t {{
"""

    for name, component_id in component_data:
        enum_name = to_pascal_case(name)
        output += f"    {enum_name} = {component_id},\n"

    output += """\
};

struct ComponentConfig {
    ComponentId id;
    const char* name;
};

inline const ComponentConfig component_configs[] = {
"""

    for name, _ in component_data:
        enum_name = to_pascal_case(name)
        output += f'    {{ComponentId::{enum_name}, "{name}"}},\n'

    output += """\
};

inline const ComponentConfig* get_component_config(
    ComponentId component
) {
    for (const auto& config : component_configs) {
        if (config.id == component) {
            return &config;
        }
    }

    return nullptr;
}

inline const char* component_name(ComponentId component) {
    const auto* config = get_component_config(component);

    return config != nullptr ? config->name : "unknown";
}

inline bool is_valid_component_id(uint8_t id) {
    return get_component_config(
        static_cast<ComponentId>(id)
    ) != nullptr;
}
"""

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(output, encoding="utf-8")

    print(f"Generated {output_path}")


if __name__ == "__main__":
    main()
