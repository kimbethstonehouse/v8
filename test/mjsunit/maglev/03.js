// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Flags: --allow-natives-syntax

function f(x) {
  var y = 0;
  for (var i = 0; i < x; i++) {
    y = 1;
  }
  return y;
}

%PrepareFunctionForOptimization(f);
assertEquals(1, f(true));
assertEquals(0, f(false));

%OptimizeMaglevOnNextCall(f);
assertEquals(1, f(true));
assertEquals(0, f(false));
