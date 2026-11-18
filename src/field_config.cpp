#include "field_config.h"
#include "config.h"
#include "settings.h"

bool FieldConfig::addBindings(const Binding *bindings, size_t count) {
  if (_groupCount >= MAX_GROUPS || bindings == nullptr) return false;
  _groups[_groupCount++] = {bindings, count};
  return true;
}

const Binding *FieldConfig::find(const String &name) const {
  for (size_t g = 0; g < _groupCount; g++)
    for (size_t i = 0; i < _groups[g].count; i++)
      if (name.equalsIgnoreCase(_groups[g].items[i].name)) return &_groups[g].items[i];
  return nullptr;
}

void FieldConfig::begin() { _io.print("\r\n> "); }

void FieldConfig::printHelp(const String &name) {
  if (name.length() > 0) {
    const Binding *b = find(name);
    if (!b) {
      _io.print("ERR unknown setting: ");
      _io.println(name);
      return;
    }
    _io.printf("%s  (%s)\r\n  %s\r\n", b->name, b->setter ? "read/write" : "read-only",
               b->description);
    return;
  }
  _io.println("Commands:");
  _io.println("  help [NAME]       describe a setting");
  _io.println("  list              print every setting and its value");
  _io.println("  get NAME");
  _io.println("  set NAME VALUE    (use \"\" for an empty value)");
  _io.println("  reset NAME|ALL    restore default value(s)");
  if (_at) _io.println("  at COMMAND        raw modem AT command, e.g. at AT+CSQ");
}

void FieldConfig::printList() {
  for (size_t g = 0; g < _groupCount; g++)
    for (size_t i = 0; i < _groups[g].count; i++) {
      const Binding &b = _groups[g].items[i];
      _io.printf("%s%s=%s\r\n", b.name, b.setter ? "" : " (ro)", b.getter().c_str());
    }
}

void FieldConfig::doGet(const String &name) {
  const Binding *b = find(name);
  if (!b) {
    _io.print("ERR unknown setting: ");
    _io.println(name);
    return;
  }
  _io.printf("OK %s=%s\r\n", b->name, b->getter().c_str());
}

void FieldConfig::doSet(const String &name, String value) {
  const Binding *b = find(name);
  if (!b) {
    _io.print("ERR unknown setting: ");
    _io.println(name);
    return;
  }
  if (!b->setter) {
    _io.print("ERR setting is read-only: ");
    _io.println(b->name);
    return;
  }
  if (value == "\"\"") value = "";  // explicit empty value
  if (!b->setter(value)) {
    _io.print("ERR invalid value for ");
    _io.println(b->name);
    return;
  }
  _io.printf("OK %s=%s\r\n", b->name, b->getter().c_str());
}

void FieldConfig::doReset(const String &name) {
  if (name.length() == 0) {
    _io.println("ERR usage: reset NAME|ALL");
    return;
  }
  if (!name.equalsIgnoreCase("ALL") && !find(name)) {
    _io.print("ERR unknown setting: ");
    _io.println(name);
    return;
  }
  if (!Settings::resetToDefault(name)) {
    _io.print("ERR cannot reset ");
    _io.println(name);
    return;
  }
  _io.printf("OK reset %s\r\n", name.c_str());
}

void FieldConfig::execute(const String &raw) {
  String line = raw;
  line.trim();
  if (line.length() == 0) return;

  int sp = line.indexOf(' ');
  String cmd = (sp < 0) ? line : line.substring(0, sp);
  String rest = (sp < 0) ? "" : line.substring(sp + 1);
  rest.trim();
  cmd.toLowerCase();

  if (cmd == "help") {
    printHelp(rest);
  } else if (cmd == "list") {
    printList();
  } else if (cmd == "get") {
    if (rest.length() == 0 || rest.indexOf(' ') >= 0) _io.println("ERR usage: get NAME");
    else doGet(rest);
  } else if (cmd == "set") {
    int sp2 = rest.indexOf(' ');
    if (sp2 < 0) {
      _io.println("ERR usage: set NAME VALUE");
    } else {
      String value = rest.substring(sp2 + 1);
      value.trim();
      doSet(rest.substring(0, sp2), value);
    }
  } else if (cmd == "reset") {
    doReset(rest);
  } else if (cmd == "at") {
    if (!_at) _io.println("ERR AT passthrough not available");
    else if (rest.length() == 0) _io.println("ERR usage: at COMMAND");
    else _io.println(_at(rest));
  } else {
    _io.print("ERR unknown command: ");
    _io.println(cmd);
  }
}

bool FieldConfig::poll() {
  bool ran = false;
  while (_io.available() > 0) {
    char c = (char)_io.read();

    if (c == '\b' || c == 127) {  // backspace
      _lastWasCR = false;
      if (_line.length() > 0) {
        _line.remove(_line.length() - 1);
        _io.print("\b \b");
      }
      continue;
    }
    if (c == '\r' || c == '\n') {
      if (c == '\n' && _lastWasCR) {  // second half of CRLF
        _lastWasCR = false;
        continue;
      }
      _lastWasCR = (c == '\r');
      if (_line.length() > 0) {
        _io.println();
        execute(_line);
        ran = true;
      }
      _io.print("\r\n> ");
      _line = "";
      continue;
    }
    _lastWasCR = false;
    if (c < 32 || c > 126) continue;  // drop control bytes / ANSI escapes

    if (_line.length() >= cfg::CONSOLE_MAX_LINE) {
      _io.println("\r\nERR command too long");
      _line = "";
      while (_io.available() > 0 && _io.read() != '\n') {}
      ran = true;
      continue;
    }
    _line += c;
    _io.write(c);  // echo
  }
  return ran;
}
