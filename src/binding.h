#pragma once
#include <Arduino.h>

// One named value reachable from the field configurator with "get NAME" /
// "set NAME VALUE". A null setter makes it read-only.
struct Binding {
  const char *name;         // case-insensitive on the wire, UPPER_SNAKE by convention
  const char *description;  // shown by "help NAME"
  String (*getter)();       // never null
  bool (*setter)(const String &value);  // null = read-only
};
