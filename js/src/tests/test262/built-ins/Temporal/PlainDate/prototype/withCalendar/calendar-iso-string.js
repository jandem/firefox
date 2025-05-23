// |reftest| shell-option(--enable-temporal) skip-if(!this.hasOwnProperty('Temporal')||!xulRuntime.shell) -- Temporal is not enabled unconditionally, requires shell-options
// Copyright (C) 2024 Igalia, S.L. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-temporal.plaindate.prototype.withcalendar
description: An ISO 8601 string can be converted to a calendar ID in Calendar
features: [Temporal]
---*/

const instance = new Temporal.PlainDate(1976, 11, 18, "iso8601");

for (const arg of [
  "2020-01-01",
  "2020-01-01[u-ca=iso8601]",
  "2020-01-01T00:00:00.000000000",
  "2020-01-01T00:00:00.000000000[u-ca=iso8601]",
  "01-01",
  "01-01[u-ca=iso8601]",
  "2020-01",
  "2020-01[u-ca=iso8601]",
]) {
  const result = instance.withCalendar(arg);
  assert.sameValue(result.calendarId, "iso8601", `Calendar created from string "${arg}"`);
}

reportCompare(0, 0);
