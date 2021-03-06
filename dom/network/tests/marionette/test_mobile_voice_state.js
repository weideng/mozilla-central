/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

MARIONETTE_TIMEOUT = 30000;

const WHITELIST_PREF = "dom.mobileconnection.whitelist";
let uriPrePath = window.location.protocol + "//" + window.location.host;
SpecialPowers.setCharPref(WHITELIST_PREF, uriPrePath);

let connection = navigator.mozMobileConnection;
ok(connection instanceof MozMobileConnection,
   "connection is instanceof " + connection.constructor);

function setEmulatorVoiceState(state) {
  runEmulatorCmd("gsm voice " + state, function (result) {
    is(result[0], "OK");
  });
}

function testConnectionInfo() {
  let voice = connection.voice;
  is(voice.connected, true);
  is(voice.state, "registered");
  is(voice.emergencyCallsOnly, false);
  is(voice.roaming, false);

  testUnregistered();
}

function testUnregistered() {
  setEmulatorVoiceState("unregistered");

  connection.addEventListener("voicechange", function onvoicechange() {
    connection.removeEventListener("voicechange", onvoicechange);

    is(connection.voice.connected, false);
    is(connection.voice.state, "notSearching");
    is(connection.voice.emergencyCallsOnly, false);
    is(connection.voice.roaming, false);

    testSearching();
  });
}

function testSearching() {
  // For some reason, requesting the "searching" state puts the fake modem
  // into "registered"... Skipping this test for now.
  testDenied();
  return;

  setEmulatorVoiceState("searching");

  connection.addEventListener("voicechange", function onvoicechange() {
    connection.removeEventListener("voicechange", onvoicechange);

    is(connection.voice.connected, false);
    is(connection.voice.state, "searching");
    is(connection.voice.emergencyCallsOnly, false);
    is(connection.voice.roaming, false);

    testDenied();
  });
}

function testDenied() {
  setEmulatorVoiceState("denied");

  connection.addEventListener("voicechange", function onvoicechange() {
    connection.removeEventListener("voicechange", onvoicechange);

    is(connection.voice.connected, false);
    is(connection.voice.state, "denied");
    is(connection.voice.emergencyCallsOnly, false);
    is(connection.voice.roaming, false);

    testRoaming();
  });
}

function testRoaming() {
  setEmulatorVoiceState("roaming");

  connection.addEventListener("voicechange", function onvoicechange() {
    connection.removeEventListener("voicechange", onvoicechange);

    is(connection.voice.connected, true);
    is(connection.voice.state, "registered");
    is(connection.voice.emergencyCallsOnly, false);
    is(connection.voice.roaming, true);

    testHome();
  });
}

function testHome() {
  setEmulatorVoiceState("home");

  connection.addEventListener("voicechange", function onvoicechange() {
    connection.removeEventListener("voicechange", onvoicechange);

    is(connection.voice.connected, true);
    is(connection.voice.state, "registered");
    is(connection.voice.emergencyCallsOnly, false);
    is(connection.voice.roaming, false);

    cleanUp();
  });
}

function cleanUp() {
  SpecialPowers.clearUserPref(WHITELIST_PREF);
  finish();
}

testConnectionInfo();
