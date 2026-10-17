
#include <Adafruit_PWMServoDriver.h>
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "component_config.hpp"

// ====================== CONFIGURATION ======================

#define ENABLE_SERVO_OUTPUT 1

constexpr size_t MAX_COMMAND_LENGTH = 64;

// BAUD_RATE, PCA9685_ADDRESS, PCA9685_FREQUENCY,
// SERVO_MIN_PULSE, SERVO_MAX_PULSE, ComponentConfig,
// component_configs, COMPONENT_COUNT, and component lookup
// functions are imported from component_config.hpp.

// ====================== PCA9685 ======================

Adafruit_PWMServoDriver pca(PCA9685_ADDRESS);

bool pca_ready = false;

char command_buffer[MAX_COMMAND_LENGTH];
size_t command_length = 0;
bool command_overflow = false;

// ====================== SERVO CONTROL ======================

bool set_servo_angle(uint8_t channel, float angle, float min_angle, float max_angle) {
    if (!isfinite(angle) || angle < min_angle || angle > max_angle || min_angle >= max_angle) {
        return false;
    }

#if ENABLE_SERVO_OUTPUT
    if (!pca_ready) {
        return false;
    }

    float fraction = (angle - min_angle) / (max_angle - min_angle);

    uint16_t pulse =
        static_cast<uint16_t>(SERVO_MIN_PULSE + fraction * (SERVO_MAX_PULSE - SERVO_MIN_PULSE));

    pca.setPWM(channel, 0, pulse);
#else
    (void)channel;
#endif

    return true;
}

// ====================== COMMAND EXECUTION ======================

bool execute_command(const ComponentConfig& config, float angle) {
    if (!isfinite(angle) || angle < config.min_angle || angle > config.max_angle) {
        return false;
    }

    // Move the primary servo.
    if (!set_servo_angle(config.channel, angle, config.min_angle, config.max_angle)) {
        return false;
    }

    // Move the mirrored servo, if configured.
    if (config.mirror_channel >= 0) {
        float mirrored_angle = config.min_angle + config.max_angle - angle;

        if (!set_servo_angle(static_cast<uint8_t>(config.mirror_channel), mirrored_angle,
                             config.min_angle, config.max_angle)) {
            return false;
        }
    }

    return true;
}

// ====================== INPUT PARSING ======================

// Accepted formats:
// <component_id>
// <component_id> <angle>
//
// Examples:
// 0
// 0 120
// 1 45.5

bool parse_command(char* input, int& component_id, float& angle, bool& has_angle) {
    char* cursor = input;
    char* end = nullptr;

    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r') {
        ++cursor;
    }

    if (*cursor == '\0') {
        return false;
    }

    long parsed_id = strtol(cursor, &end, 10);

    if (end == cursor || parsed_id < 0 || parsed_id > 255) {
        return false;
    }

    if (*end != '\0' && *end != ' ' && *end != '\t') {
        return false;
    }

    component_id = static_cast<int>(parsed_id);
    cursor = end;

    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }

    // Use the configured default angle when omitted.
    if (*cursor == '\0') {
        has_angle = false;
        angle = 0.0f;
        return true;
    }

    double parsed_angle = strtod(cursor, &end);

    if (end == cursor || !isfinite(parsed_angle)) {
        return false;
    }

    while (*end == ' ' || *end == '\t' || *end == '\r') {
        ++end;
    }

    if (*end != '\0') {
        return false;
    }

    angle = static_cast<float>(parsed_angle);

    if (!isfinite(angle)) {
        return false;
    }

    has_angle = true;
    return true;
}

// ====================== COMMAND HANDLER ======================

void process_command(char* input) {
    int component_id = -1;
    float angle = 0.0f;
    bool has_angle = false;

    if (!parse_command(input, component_id, angle, has_angle)) {
        Serial.println(F("REJECTED"));
        return;
    }

    // Lookup is provided by component_config.hpp.
    const ComponentConfig* config = get_component_config_by_id(static_cast<uint8_t>(component_id));

    if (config == nullptr) {
        Serial.println(F("REJECTED"));
        return;
    }

    if (!has_angle) {
        angle = config->default_angle;
    }

    if (!execute_command(*config, angle)) {
        Serial.println(F("REJECTED"));
        return;
    }

    Serial.print(F("EXECUTED: "));
    Serial.print(component_id);
    Serial.print(' ');
    Serial.println(angle, 2);
}

// ====================== SETUP ======================

void setup() {
    Serial.begin(BAUD_RATE);

#if ENABLE_SERVO_OUTPUT
    Wire.begin();

    pca_ready = pca.begin();

    if (pca_ready) {
        pca.setPWMFreq(PCA9685_FREQUENCY);
        delay(10);

        // Initialize every component to its configured default.
        for (size_t i = 0; i < COMPONENT_COUNT; ++i) {
            execute_command(component_configs[i], component_configs[i].default_angle);
        }
    }
#endif

    command_length = 0;
    command_overflow = false;
}

// ====================== MAIN LOOP ======================

void loop() {
    while (Serial.available() > 0) {
        char incoming = static_cast<char>(Serial.read());

        if (incoming == '\n') {
            if (command_overflow) {
                Serial.println(F("REJECTED"));
            } else {
                command_buffer[command_length] = '\0';

                if (command_length > 0) {
                    process_command(command_buffer);
                }
            }

            command_length = 0;
            command_overflow = false;
            continue;
        }

        // Support both LF and CRLF line endings.
        if (incoming == '\r') {
            continue;
        }

        if (command_overflow) {
            continue;
        }

        if (command_length < MAX_COMMAND_LENGTH - 1) {
            command_buffer[command_length++] = incoming;
        } else {
            command_overflow = true;
        }
    }
}
