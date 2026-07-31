/*
 * arduino_ping.ino — liveness responder for scanner_app
 *
 * Protocol (newline-delimited ASCII):
 *   host -> nano :  "PING\n"
 *   nano -> host :  "PONG nanov3 <uptime_ms>\n"
 *
 * Anything else is ignored.  The built-in LED blinks once per PONG so you
 * can confirm the link visually without looking at the app.
 *
 * Board: Arduino Nano v3 (ATmega328P).
 * If uploads fail on a clone, select the "ATmega328P (Old Bootloader)"
 * processor variant in the IDE.
 *
 * The baud rate here MUST match ARDUINO_BAUD in scanner_app.c.
 */

static const unsigned long BAUD = 115200;

void setup()
{
    Serial.begin(BAUD);
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
}

void loop()
{
    if (!Serial.available())
        return;

    String line = Serial.readStringUntil('\n');
    line.trim();

    if (line == "PING") {
        Serial.print("PONG nanov3 ");
        Serial.println(millis());

        digitalWrite(LED_BUILTIN, HIGH);
        delay(40);
        digitalWrite(LED_BUILTIN, LOW);
    }
}
