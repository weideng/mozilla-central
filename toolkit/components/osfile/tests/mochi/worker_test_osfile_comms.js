/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

function log(text) {
  dump("WORKER "+text+"\n");
}

function send(message) {
  self.postMessage(message);
}

function finish() {
  send({kind: "finish"});
}

function ok(condition, description) {
  send({kind: "ok", condition: condition, description:description});
}
function is(a, b, description) {
  let outcome = a == b; // Need to decide outcome here, as not everything can be serialized
  send({kind: "is", outcome: outcome, description: description, a:""+a, b:""+b});
}
function isnot(a, b, description) {
  let outcome = a != b; // Need to decide outcome here, as not everything can be serialized
  send({kind: "isnot", outcome: outcome, description: description, a:""+a, b:""+b});
}

// The set of samples for communications test. Declare as a global
// variable to prevent this from being garbage-collected too early.
let samples;

self.onmessage = function(msg) {
  ok(true, "Initializing");
  self.onmessage = function on_unexpected_message(msg) {
    throw new Error("Unexpected message " + JSON.stringify(msg.data));
  };
  importScripts("resource:///modules/osfile.jsm");
  ok(true, "Initialization complete");

  samples = [
    { typename: "OS.Shared.Type.char.in_ptr",
      valuedescr: "String",
      value: "This is a test",
      type: OS.Shared.Type.char.in_ptr,
      check: function check_string(candidate, prefix) {
        is(candidate, "This is a test", prefix);
      }},
    { typename: "OS.Shared.Type.char.in_ptr",
      valuedescr: "ArrayBuffer",
      value: (function() {
                let buf = new ArrayBuffer(15);
                let view = new Uint8Array(buf);
                for (let i = 0; i < 15; ++i) {
                  view[i] = i;
                }
                return buf;
              })(),
      type: OS.Shared.Type.char.in_ptr,
      check: function check_ArrayBuffer(candidate, prefix) {
        let cast = ctypes.cast(candidate, ctypes.uint8_t.ptr);
        for (let i = 0; i < 15; ++i) {
          is(cast.contents, i % 256, prefix + "Checking that the contents of the ArrayBuffer were preserved");
          cast = cast.increment();
        }
      }},
    { typename: "OS.Shared.Type.char.in_ptr",
      valuedescr: "Pointer",
      value: new OS.Shared.Type.char.in_ptr.implementation(1),
      type: OS.Shared.Type.char.in_ptr,
      check: function check_ptr(candidate, prefix) {
        let address = ctypes.cast(candidate, ctypes.uintptr_t).value.toString();
        is(address, "1", prefix + "Checking that the pointer address was preserved");
      }},
    { typename: "OS.Shared.Type.char.in_ptr",
      valuedescr: "C array",
      value: (function() {
                let buf = new (ctypes.ArrayType(ctypes.uint8_t, 15))();
                for (let i = 0; i < 15; ++i) {
                  buf[i] = i % 256;
                }
                return buf;
              })(),
      type: OS.Shared.Type.char.in_ptr,
      check: function check_array(candidate, prefix) {
        let cast = ctypes.cast(candidate, ctypes.uint8_t.ptr);
        for (let i = 0; i < 15; ++i) {
          is(cast.contents, i % 256, prefix + "Checking that the contents of the C array were preserved, index " + i);
          cast = cast.increment();
        }
      }
    }
  ];
  samples.forEach(function test(sample) {
    let type = sample.type;
    let value = sample.value;
    let check = sample.check;
    ok(true, "Testing handling of type " + sample.typename + " communicating " + sample.valuedescr);

    // 1. Test serialization
    let serialized;
    let exn;
    try {
      serialized = type.toMsg(value);
    } catch (ex) {
      exn = ex;
    }
    is(exn, null, "Can I serialize the following value? " + value +
      " aka " + JSON.stringify(value));
    if (exn) {
      return;
    }

    // 2. Test deserialization
    let deserialized;
    try {
      deserialized = type.fromMsg(serialized);
    } catch (ex) {
      exn = ex;
    }
    is(exn, null, "Can I deserialize the following message? " + serialized
     + " aka " + JSON.stringify(serialized));
    if (exn) {
      return;
    }

    // 3. Local test deserialized value
    ok(true, "Running test on deserialized value " + serialized);
    check(deserialized, "Local test: ");

    // 4. Test sending serialized
    ok(true, "Attempting to send message");
    try {
      self.postMessage({kind:"value",
        typename: sample.typename,
        value: serialized,
        check: check.toSource()});
    } catch (ex) {
      exn = ex;
    }
    is(exn, null, "Can I send the following message? " + serialized
     + " aka " + JSON.stringify(serialized));
  });

  finish();
 };
