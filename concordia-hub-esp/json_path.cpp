/*
 * A JSON path walker with no dependencies beyond String, kept apart from the
 * HTTP code so it can be compiled and tested on a normal computer. It has
 * needed that: an earlier version matched a bare key against a units string
 * and quietly returned zero.
 */
#include "json_path.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

namespace {

int skipWs(const String &d, int i) {
  while (i < (int)d.length() && isspace((unsigned char)d[i])) {
    ++i;
  }
  return i;
}

/* `i` points at the opening quote; returns the index just past the closing one. */
int skipString(const String &d, int i) {
  ++i;
  while (i < (int)d.length()) {
    const char c = d[i];
    if (c == '\\') {
      i += 2;
      continue;
    }
    if (c == '"') {
      return i + 1;
    }
    ++i;
  }
  return i;
}

int skipValue(const String &d, int i) {
  i = skipWs(d, i);
  if (i >= (int)d.length()) {
    return i;
  }

  const char c = d[i];
  if (c == '"') {
    return skipString(d, i);
  }
  if (c == '{' || c == '[') {
    const char open = c;
    const char close = (c == '{') ? '}' : ']';
    int depth = 0;
    while (i < (int)d.length()) {
      const char k = d[i];
      if (k == '"') {
        i = skipString(d, i);
        continue;
      }
      if (k == open) {
        ++depth;
      } else if (k == close) {
        if (--depth == 0) {
          return i + 1;
        }
      }
      ++i;
    }
    return i;
  }
  /* number, true, false, null */
  while (i < (int)d.length() && strchr(",}] \t\r\n", d[i]) == nullptr) {
    ++i;
  }
  return i;
}

/* `i` points at '{'. Returns the index of the value for `key`, or -1. */
int findMember(const String &d, int i, const String &key) {
  i = skipWs(d, i);
  if (i >= (int)d.length() || d[i] != '{') {
    return -1;
  }
  ++i;
  while (true) {
    i = skipWs(d, i);
    if (i >= (int)d.length() || d[i] == '}') {
      return -1;
    }
    if (d[i] != '"') {
      return -1;
    }
    const int nameStart = i + 1;
    const int afterName = skipString(d, i);
    const String name = d.substring(nameStart, afterName - 1);

    i = skipWs(d, afterName);
    if (i >= (int)d.length() || d[i] != ':') {
      return -1;
    }
    i = skipWs(d, i + 1);
    if (name == key) {
      return i;
    }
    i = skipValue(d, i);
    i = skipWs(d, i);
    if (i < (int)d.length() && d[i] == ',') {
      ++i;
    }
  }
}

/* `i` points at '['. Returns the index of element `index`, or -1. */
int findElement(const String &d, int i, int index) {
  i = skipWs(d, i);
  if (i >= (int)d.length() || d[i] != '[') {
    return -1;
  }
  ++i;
  for (int n = 0;; ++n) {
    i = skipWs(d, i);
    if (i >= (int)d.length() || d[i] == ']') {
      return -1;
    }
    if (n == index) {
      return i;
    }
    i = skipValue(d, i);
    i = skipWs(d, i);
    if (i < (int)d.length() && d[i] == ',') {
      ++i;
    }
  }
}

/* Reads a scalar. Numbers, true/false, and numeric strings ("21.4") all count -
 * plenty of APIs quote their numbers and it would be unhelpful to refuse. */
bool readScalar(const String &d, int i, bool wantBoolean, double &out) {
  i = skipWs(d, i);
  if (i >= (int)d.length()) {
    return false;
  }

  String token;
  if (d[i] == '"') {
    token = d.substring(i + 1, skipString(d, i) - 1);
  } else {
    token = d.substring(i, skipValue(d, i));
  }
  token.trim();
  if (token.length() == 0 || token == "null") {
    return false;
  }

  if (token == "true" || token == "false") {
    out = (token == "true") ? 1 : 0;
    return true;
  }
  if (wantBoolean && (token == "yes" || token == "no" || token == "on" || token == "off")) {
    out = (token == "yes" || token == "on") ? 1 : 0;
    return true;
  }

  /* Reject anything that is not a plain number: a bare key used to match the
   * units string ("°C") and quietly became zero. */
  const char *s = token.c_str();
  char *end = nullptr;
  const double v = strtod(s, &end);
  if (end == s || *end != '\0') {
    return false;
  }
  out = v;
  return true;
}

/* Splits "hourly.temperature_2m[0]" into steps the walker can follow. */
bool walk(const String &d, const String &path, int &posOut, String &error) {
  int pos = skipWs(d, 0);
  int i = 0;

  while (i < (int)path.length()) {
    if (path[i] == '.') {
      ++i;
      continue;
    }

    if (path[i] == '[') {
      const int close = path.indexOf(']', i);
      if (close < 0) {
        error = "unbalanced [ in path";
        return false;
      }
      const int index = path.substring(i + 1, close).toInt();
      pos = findElement(d, pos, index);
      if (pos < 0) {
        error = "no element [" + String(index) + "]";
        return false;
      }
      i = close + 1;
      continue;
    }

    int end = i;
    while (end < (int)path.length() && path[end] != '.' && path[end] != '[') {
      ++end;
    }
    const String key = path.substring(i, end);
    pos = findMember(d, pos, key);
    if (pos < 0) {
      error = "no field \"" + key + "\"";
      return false;
    }
    i = end;
  }

  posOut = pos;
  return true;
}

/* Depth-first hunt for the first scalar we can use, remembering how we got
 * there so the UI can show the user a real path instead of "somewhere". */
bool firstScalar(const String &d, int pos, const String &prefix, bool wantBoolean,
                 uint8_t depth, double &out, String &path) {
  if (depth > 6) {
    return false;
  }
  pos = skipWs(d, pos);
  if (pos >= (int)d.length()) {
    return false;
  }

  if (d[pos] == '{') {
    int i = pos + 1;
    while (true) {
      i = skipWs(d, i);
      if (i >= (int)d.length() || d[i] == '}') {
        return false;
      }
      if (d[i] != '"') {
        return false;
      }
      const int nameStart = i + 1;
      const int afterName = skipString(d, i);
      const String name = d.substring(nameStart, afterName - 1);
      i = skipWs(d, afterName);
      if (i >= (int)d.length() || d[i] != ':') {
        return false;
      }
      i = skipWs(d, i + 1);

      const String child = prefix.length() ? prefix + "." + name : name;
      if (firstScalar(d, i, child, wantBoolean, depth + 1, out, path)) {
        return true;
      }
      i = skipValue(d, i);
      i = skipWs(d, i);
      if (i < (int)d.length() && d[i] == ',') {
        ++i;
      }
    }
  }

  if (d[pos] == '[') {
    const int first = findElement(d, pos, 0);
    if (first < 0) {
      return false;
    }
    return firstScalar(d, first, prefix + "[0]", wantBoolean, depth + 1, out, path);
  }

  if (readScalar(d, pos, wantBoolean, out)) {
    path = prefix;
    return true;
  }
  return false;
}

void collect(const String &d, int pos, const String &prefix, uint8_t depth,
             uint8_t maxFields, uint8_t &count, String &out) {
  if (depth > 5 || count >= maxFields) {
    return;
  }
  pos = skipWs(d, pos);
  if (pos >= (int)d.length()) {
    return;
  }

  if (d[pos] == '{') {
    int i = pos + 1;
    while (count < maxFields) {
      i = skipWs(d, i);
      if (i >= (int)d.length() || d[i] == '}' || d[i] != '"') {
        return;
      }
      const int nameStart = i + 1;
      const int afterName = skipString(d, i);
      const String name = d.substring(nameStart, afterName - 1);
      i = skipWs(d, afterName);
      if (i >= (int)d.length() || d[i] != ':') {
        return;
      }
      i = skipWs(d, i + 1);
      collect(d, i, prefix.length() ? prefix + "." + name : name, depth + 1, maxFields,
              count, out);
      i = skipValue(d, i);
      i = skipWs(d, i);
      if (i < (int)d.length() && d[i] == ',') {
        ++i;
      }
    }
    return;
  }

  if (d[pos] == '[') {
    const int first = findElement(d, pos, 0);
    if (first >= 0) {
      collect(d, first, prefix + "[0]", depth + 1, maxFields, count, out);
    }
    return;
  }

  double v = 0;
  if (readScalar(d, pos, false, v)) {
    if (out.length()) {
      out += '\n';
    }
    out += prefix + " = " + String(v, 3);
    ++count;
  }
}

} // namespace

namespace Json {

bool extract(const String &doc, const String &path, bool wantBoolean, double &out,
             String &usedPath, String &error) {
  if (doc.length() == 0) {
    error = "empty response";
    return false;
  }

  if (path.length() == 0) {
    String found;
    if (firstScalar(doc, 0, "", wantBoolean, 0, out, found)) {
      usedPath = found;
      return true;
    }
    error = "no number found in the response";
    return false;
  }

  int pos = 0;
  if (!walk(doc, path, pos, error)) {
    return false;
  }
  if (!readScalar(doc, pos, wantBoolean, out)) {
    error = "\"" + path + "\" is not a number";
    return false;
  }
  usedPath = path;
  return true;
}

String discover(const String &doc, uint8_t maxFields) {
  String out;
  uint8_t count = 0;
  collect(doc, 0, "", 0, maxFields, count, out);
  return out;
}

} // namespace Json

