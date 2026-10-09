#pragma once
// QCOMPARE on JSON values. QtTest prints a mismatch through toString(), and its default probes for a QDebug
// operator<<. Under Q_OS_WIN that probe sees Qt's operator<<(QDebug, const MSG &) with MSG only forward-declared,
// and nlohmann's templated implicit conversion then needs the incomplete type: a hard compile error, Windows only.
// This overload, found by argument-dependent lookup, is what QtTest uses for json instead, on every platform.
#include <nlohmann/json.hpp>
#include <QtTest/qtestcase.h>

namespace nlohmann {
inline char *toString(const ordered_json &J) { return qstrdup(J.dump().c_str()); }
inline char *toString(const json &J) { return qstrdup(J.dump().c_str()); }
}
