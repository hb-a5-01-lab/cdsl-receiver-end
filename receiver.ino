#include <Adafruit_PWMServoDriver.h>
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "component_config.hpp"

// -----------------------------------------------------------------------------
// Runtime configuration
// -----------------------------------------------------------------------------

// Controls whether servo commands are sent to the PCA9685.
// Set to 0 to disable servo output while retaining command parsing and
// validation. With output disabled, successful commands do not move servos.
#define ENABLE_SERVO_OUTPUT 1

// Maximum number of characters stored for one incoming serial command,
// including space for the terminating null character.
constexpr size_t MAX_COMMAND_LENGTH = 64;

// The following configuration values, component definitions, component count,
// and lookup functions are declared in component_config.hpp:
//
//   BAUD_RATE
//   PCA9685_ADDRESS
//   PCA9685_FREQUENCY
//   SERVO_MIN_PULSE
//   SERVO_MAX_PULSE
//   ComponentConfig
//   component_configs
//   COMPONENT_COUNT
//   get_component_config_by_id()

// -----------------------------------------------------------------------------
// PCA9685 state
// -----------------------------------------------------------------------------

// Driver instance configured to communicate with the PCA9685 at the
// I2C address specified by the configuration header.
Adafruit_PWMServoDriver pca(PCA9685_ADDRESS);

// Indicates whether pca.begin() reported successful initialization.
bool pca_ready = false;

// Stores the serial characters received for the current command.
char command_buffer[MAX_COMMAND_LENGTH];

// Number of characters currently stored in command_buffer, excluding
// the terminating null character.
size_t command_length = 0;

// Indicates that the current command exceeded the available buffer capacity.
// An overflowing command is discarded when its newline terminator arrives.
bool command_overflow = false;

// -----------------------------------------------------------------------------
// Servo control
// -----------------------------------------------------------------------------

// Converts an angle within the supplied range into a PCA9685 pulse value
// and, when servo output is enabled, writes that value to the specified
// PCA9685 channel.
//
// Returns true when the angle passes validation and the operation permitted
// by ENABLE_SERVO_OUTPUT completes. Returns false when validation fails or
// enabled servo output cannot be performed.
bool set_servo_angle(uint8_t channel, float angle, float min_angle, float max_angle) {
    // Reject non-finite angles, angles outside the permitted range, and
    // invalid ranges in which the minimum is not less than the maximum.
    if (!isfinite(angle) || angle < min_angle || angle > max_angle || min_angle >= max_angle) {
        return false;
    }

#if ENABLE_SERVO_OUTPUT
    // Servo output requires successful PCA9685 initialization.
    if (!pca_ready) {
        return false;
    }

    // Normalize the angle to a fraction of the configured angular range.
    // The minimum angle maps to 0.0 and the maximum angle maps to 1.0.
    float fraction = (angle - min_angle) / (max_angle - min_angle);

    // Linearly map the normalized fraction to the configured PCA9685 pulse
    // range, then convert the result to the integer value accepted by setPWM().
    uint16_t pulse =
        static_cast<uint16_t>(SERVO_MIN_PULSE + fraction * (SERVO_MAX_PULSE - SERVO_MIN_PULSE));

    // Start the pulse at count 0 and end it at the calculated count.
    pca.setPWM(channel, 0, pulse);
#else
    // Prevent an unused-parameter warning when servo output is disabled.
    (void)channel;
#endif

    return true;
}

// -----------------------------------------------------------------------------
// Component command execution
// -----------------------------------------------------------------------------

// Validates the requested angle against the component's configured range,
// commands its primary servo, and commands its mirrored servo when one
// is configured.
//
// For a mirrored servo, the angle is reflected across the midpoint of the
// configured angular range:
//
//   mirrored_angle = min_angle + max_angle - angle
//
// Returns false if the requested angle is invalid or either servo operation
// fails. Returns true when all required operations succeed.
bool execute_command(const ComponentConfig& config, float angle) {
    if (!isfinite(angle) || angle < config.min_angle || angle > config.max_angle) {
        return false;
    }

    // Apply the requested angle to the component's primary PCA9685 channel.
    if (!set_servo_angle(config.channel, angle, config.min_angle, config.max_angle)) {
        return false;
    }

    // A negative mirror_channel indicates that no mirrored channel is
    // configured. Otherwise, calculate and apply the reflected angle.
    if (config.mirror_channel >= 0) {
        float mirrored_angle = config.min_angle + config.max_angle - angle;

        if (!set_servo_angle(static_cast<uint8_t>(config.mirror_channel), mirrored_angle,
                             config.min_angle, config.max_angle)) {
            return false;
        }
    }

    return true;
}

// -----------------------------------------------------------------------------
// Serial command parsing
// -----------------------------------------------------------------------------

// Parses a null-terminated command containing a component ID and an optional
// angle. The function modifies neither the input buffer nor the hardware.
//
// Accepted formats:
//   <component_id>
//   <component_id> <angle>
//
// Examples:
//   0
//   0 120
//   1 45.5
//
// Leading whitespace before the component ID is accepted. The component ID
// must be a decimal integer in the range 0–255. When supplied, the angle must
// be a finite numeric value followed only by permitted trailing whitespace.
//
// Output parameters:
//   component_id - parsed component identifier.
//   angle        - parsed angle, or 0.0f when no angle is supplied.
//   has_angle    - true if an angle was supplied; false otherwise.
//
// Returns true if the entire command matches the accepted format.
bool parse_command(char* input, int& component_id, float& angle, bool& has_angle) {
    char* cursor = input;
    char* end = nullptr;

    // Skip permitted whitespace before the component ID.
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r') {
        ++cursor;
    }

    // An empty command contains no component ID.
    if (*cursor == '\0') {
        return false;
    }

    // Parse the component ID as a decimal integer.
    long parsed_id = strtol(cursor, &end, 10);

    // Reject missing IDs and values outside the representable command range.
    if (end == cursor || parsed_id < 0 || parsed_id > 255) {
        return false;
    }

    // Require the ID to end at whitespace or at the end of the input.
    // This rejects inputs such as "1abc".
    if (*end != '\0' && *end != ' ' && *end != '\t') {
        return false;
    }

    component_id = static_cast<int>(parsed_id);
    cursor = end;

    // Skip whitespace separating the ID from an optional angle.
    while (*cursor == ' ' || *cursor == '\t') {
        ++cursor;
    }

    // If the angle is omitted, the caller can use the component's configured
    // default angle. The value assigned here is only a placeholder.
    if (*cursor == '\0') {
        has_angle = false;
        angle = 0.0f;
        return true;
    }

    // Parse the supplied angle as a floating-point number.
    double parsed_angle = strtod(cursor, &end);

    if (end == cursor || !isfinite(parsed_angle)) {
        return false;
    }

    // Permit trailing spaces, tabs, or carriage returns, but reject any
    // additional non-whitespace characters.
    while (*end == ' ' || *end == '\t' || *end == '\r') {
        ++end;
    }

    if (*end != '\0') {
        return false;
    }

    // Convert the parsed value to the angle type used by the servo functions.
    angle = static_cast<float>(parsed_angle);

    // Check again after conversion in case the float representation is
    // non-finite even though the parsed double was finite.
    if (!isfinite(angle)) {
        return false;
    }

    has_angle = true;
    return true;
}

// -----------------------------------------------------------------------------
// Serial command handling
// -----------------------------------------------------------------------------

// Parses one complete command, resolves its component configuration, executes
// the requested operation, and writes a status response to the serial port.
//
// Response formats:
//   REJECTED
//   EXECUTED: <component_id> <angle>
//
// The response indicates the result reported by the parsing, lookup, and
// execution functions; it does not independently verify the physical servo
// position.
void process_command(char* input) {
    int component_id = -1;
    float angle = 0.0f;
    bool has_angle = false;

    // Reject commands that do not match the accepted input format.
    if (!parse_command(input, component_id, angle, has_angle)) {
        Serial.println(F("REJECTED"));
        return;
    }

    // Resolve the numeric ID to its configured component definition.
    // The lookup function returns nullptr when the ID is not configured.
    const ComponentConfig* config = get_component_config_by_id(static_cast<uint8_t>(component_id));

    if (config == nullptr) {
        Serial.println(F("REJECTED"));
        return;
    }

    // Use the configured default angle when the command contains only an ID.
    if (!has_angle) {
        angle = config->default_angle;
    }

    // Execute the command and reject it if validation or a required servo
    // operation fails.
    if (!execute_command(*config, angle)) {
        Serial.println(F("REJECTED"));
        return;
    }

    // Report the component ID and the angle accepted by the execution path.
    Serial.print(F("EXECUTED: "));
    Serial.print(component_id);
    Serial.print(' ');
    Serial.println(angle, 2);
}

// -----------------------------------------------------------------------------
// Arduino initialization
// -----------------------------------------------------------------------------

void setup() {
    // Initialize serial communication using the configured baud rate.
    Serial.begin(BAUD_RATE);

#if ENABLE_SERVO_OUTPUT
    // Initialize the I2C interface used to communicate with the PCA9685.
    Wire.begin();

    // Initialize the PCA9685 driver and retain the reported result.
    pca_ready = pca.begin();

    if (pca_ready) {
        // Configure the PCA9685 output frequency.
        pca.setPWMFreq(PCA9685_FREQUENCY);

        // Allow time for the frequency configuration to take effect.
        delay(10);

        // Request each configured component's default angle.
        // Components are processed in the order stored in component_configs.
        for (size_t i = 0; i < COMPONENT_COUNT; ++i) {
            execute_command(component_configs[i], component_configs[i].default_angle);
        }
    }
#endif

    // Start with an empty command buffer and no pending overflow condition.
    command_length = 0;
    command_overflow = false;
}

// -----------------------------------------------------------------------------
// Serial command reception
// -----------------------------------------------------------------------------

void loop() {
    // Read all serial bytes currently available without waiting for a
    // complete command to arrive.
    while (Serial.available() > 0) {
        char incoming = static_cast<char>(Serial.read());

        // A newline terminates the current command.
        if (incoming == '\n') {
            if (command_overflow) {
                // Reject the entire command if it exceeded the buffer limit.
                Serial.println(F("REJECTED"));
            } else {
                // Convert the buffered characters into a null-terminated
                // string before passing them to the parser.
                command_buffer[command_length] = '\0';

                // Ignore empty lines instead of passing them to the parser.
                if (command_length > 0) {
                    process_command(command_buffer);
                }
            }

            // Reset the receive state for the next command.
            command_length = 0;
            command_overflow = false;
            continue;
        }

        // Ignore carriage returns so both LF and CRLF line endings are
        // accepted. The parser also tolerates carriage returns in whitespace.
        if (incoming == '\r') {
            continue;
        }

        // Once the buffer overflows, discard further characters until the
        // terminating newline resets the receive state.
        if (command_overflow) {
            continue;
        }

        // Reserve one byte for the null terminator.
        if (command_length < MAX_COMMAND_LENGTH - 1) {
            command_buffer[command_length++] = incoming;
        } else {
            // Mark the current command as invalid due to excessive length.
            command_overflow = true;
        }
    }
}
