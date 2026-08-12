#include "console.h"

#include <Arduino.h>

#include "commands.h"

namespace {

char line[192];
size_t len = 0;

void prompt() { Serial.print("\nbridge> "); }

} // namespace

namespace Console {

void begin() {
  Serial.println();
  Serial.println("Type `help` for the command list.");
  prompt();
}

void loop() {
  while (Serial.available() > 0) {
    const char c = (char)Serial.read();

    if (c == '\b' || c == 0x7f) {
      if (len > 0) {
        --len;
        Serial.print("\b \b");
      }
      continue;
    }

    if (c == '\r' || c == '\n') {
      /* CRLF arrives as two characters; an empty line just redraws the prompt. */
      if (len == 0) {
        prompt();
        continue;
      }
      line[len] = '\0';
      len = 0;
      Serial.println();

      const String answer = Commands::execute(String(line));
      if (answer.length()) {
        Serial.println(answer);
      }
      prompt();
      continue;
    }

    if (len + 1 < sizeof(line)) {
      line[len++] = c;
      Serial.print(c); /* echo, so typing feels like a terminal */
    }
  }
}

} // namespace Console
