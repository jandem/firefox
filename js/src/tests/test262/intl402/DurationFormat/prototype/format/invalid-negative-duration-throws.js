// |reftest| skip-if(!Intl.hasOwnProperty('DurationFormat')) -- Intl.DurationFormat is not enabled unconditionally
// Copyright (C) 2022 Igalia, S.L. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-Intl.DurationFormat.prototype.format
description: format basic tests for invalid negative duration objects that should throw RangeError exception.
features: [Intl.DurationFormat]
---*/



const df = new Intl.DurationFormat();

assert.throws(RangeError, () => { df.format({
  hours : -1,
  minutes: 10
}), "Throws when mixing negative and positive values" });

assert.throws(RangeError, () => { df.format({
  hours : 2,
  minutes: -10
}), "Throws when mixing negative and positive values" });

reportCompare(0, 0);
