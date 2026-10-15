#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "component_config.hpp"

// Set to 0 while testing serial communication.
// Set to 1 when the PCA9685 and servos are connected.
#define ENABLE_SERVO_OUTPUT 0

constexpr size_t MAX_COMMAND_LENGTH = 64;

Adafruit_PWMServoDriver pca(PCA9685_ADDRESS);

bool pca_ready = false;

char command_buffer[MAX_COMMAND_LENGTH];
size_t command_length = 0;
bool command_overflow = false;


// -------------------- SERVO CONTROL --------------------

bool set_servo_angle(
    uint8_t channel,
    float angle,
    float min_angle,
    float max_angle
) {
    if (!isfinite(angle) ||
        angle < min_angle ||
        angle > max_angle ||
        min_angle >= max_angle) {
        return false;
    }

#if ENABLE_SERVO_OUTPUT
    if (!pca_ready) {
        return false;
    }

    float fraction =
        (angle - min_angle) / (max_angle - min_angle);

    uint16_t pulse = static_cast<uint16_t>(
        SERVO_MIN_PULSE +
        fraction * (SERVO_MAX_PULSE - SERVO_MIN_PULSE)
    );

    pca.setPWM(channel, 0, pulse);
#else
    // Serial-only testing: validate the command without moving hardware.
    (void)channel;
#endif

    return true;
}


// -------------------- COMMAND EXECUTION --------------------

bool execute_command(
    const ComponentConfig& config,
    float angle
) {
    if (!isfinite(angle) ||
        angle < config.min_angle ||
        angle > config.max_angle) {
        return false;
    }

    if (!set_servo_angle(
            config.channel,
            angle,
            config.min_angle,
            config.max_angle)) {
        return false;
    }

    // Drive the optional second servo for mirrored joints.
    if (config.mirror_channel >= 0) {
        if (!set_servo_angle(
                static_cast<uint8_t>(config.mirror_channel),
                angle,
                config.min_angle,
                config.max_angle)) {
            return false;
        }
    }

    return true;
}


// -------------------- INPUT PARSING --------------------

// Accepted formats:
// <component_id>
// <component_id> <angle>
//
// Examples:
// 0
// 0 120
// 1 45.5

bool parse_command(
    char* input,
    int& component_id,
    float& angle,
    bool& has_angle
) {
    char* cursor = input;
    char* end = nullptr;

    while (*cursor == ' ' ||
           *cursor == '\t' ||
           *cursor == '\r') {
        ++cursor;
    }

    if (*cursor == '\0') {
        return false;
    }

    // Parse component ID.
    long parsed_id = strtol(cursor, &end, 10);

    if (end == cursor ||
        parsed_id < 0 ||
        parsed_id > 255) {
        return false;
    }

    if (*end != '\0' &&
        *end != ' ' &&
        *end != '\t') {
        return false;
    }

    component_id = static_cast<int>(parsed_id);
    cursor = end;

    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }

    // An ID-only command uses the configured default angle.
    if (*cursor == '\0') {
        has_angle = false;
        angle = 0.0f;
        return true;
    }

    // Parse angle.
    double parsed_angle = strtod(cursor, &end);

    if (end == cursor || !isfinite(parsed_angle)) {
        return false;
    }

    while (*end == ' ' ||
           *end == '\t' ||
           *end == '\r') {
        ++end;
    }

    // Reject extra tokens or malformed input.
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


// -------------------- COMMAND HANDLER --------------------

void process_command(char* input)
{
    int component_id = -1;
    float angle = 0.0f;
    bool has_angle = false;

    if (!parse_command(
            input,
            component_id,
            angle,
            has_angle)) {
        Serial.println(F("REJECTED"));
        return;
    }

    const ComponentConfig* config =
        get_component_config_by_id(
            static_cast<uint8_t>(component_id)
        );

    if (config == nullptr) {
        Serial.println(F("REJECTED"));
        return;
    }

    // Use the default angle from component_config.hpp if omitted.
    if (!has_angle) {
        angle = config->default_angle;
    }

    if (angle < config->min_angle ||
        angle > config->max_angle) {
        Serial.println(F("REJECTED"));
        return;
    }

    if (!execute_command(*config, angle)) {
        Serial.println(F("REJECTED"));
        return;
    }

    // Response format expected by the Go client:
    // EXECUTED: <component_id> <angle>

    Serial.print(F("EXECUTED: "));
    Serial.print(component_id);
    Serial.print(' ');
    Serial.println(angle, 2);
}


// -------------------- SETUP --------------------

void setup()
{
    Serial.begin(BAUD_RATE);

#if ENABLE_SERVO_OUTPUT
    Wire.begin();

    pca_ready = pca.begin();

    if (pca_ready) {
        pca.setPWMFreq(PCA9685_FREQUENCY);
        delay(10);
    } else {
        // Commands will be rejected until the driver is available.
        pca_ready = false;
    }
#endif

    command_length = 0;
    command_overflow = false;
}


// -------------------- MAIN LOOP --------------------

void loop()
{
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
