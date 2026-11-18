#pragma once
#include <Arduino.h>
#include "binding.h"

// Field configurator console: a line-based text protocol on a Stream (UART0)
// that lets a technician or a field-configurator device read and change
// settings. Full command list and every setting: docs/field-configurator.md.
//
//   help [NAME]        list commands, or describe one setting
//   list               print NAME=value for every readable setting
//   get NAME           print one value        -> "OK NAME=value"
//   set NAME VALUE     change + persist       -> "OK NAME=value"
//   reset NAME|ALL     restore default(s)
//   at COMMAND         raw AT command to the modem
//
// Replies start with "OK" or "ERR". Each finished line is followed by a
// "\r\n> " prompt.
class FieldConfig {
 public:
  explicit FieldConfig(Stream &io) : _io(io) {}

  // Register a group of bindings (max MAX_GROUPS). Names are looked up in
  // registration order, so register writable settings first.
  bool addBindings(const Binding *bindings, size_t count);

  // Optional "at COMMAND" passthrough; return the modem's reply text.
  void setAtHandler(String (*handler)(const String &command)) { _at = handler; }

  void begin();            // print the prompt
  bool poll();             // read pending input; true if a command ran
  void execute(const String &line);  // run one command line (also for tests)

 private:
  static constexpr size_t MAX_GROUPS = 4;
  struct Group { const Binding *items; size_t count; };

  const Binding *find(const String &name) const;
  void printHelp(const String &name);
  void printList();
  void doGet(const String &name);
  void doSet(const String &name, String value);
  void doReset(const String &name);

  Stream &_io;
  Group _groups[MAX_GROUPS];
  size_t _groupCount = 0;
  String (*_at)(const String &) = nullptr;
  String _line;
  bool _lastWasCR = false;
};
