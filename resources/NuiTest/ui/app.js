// NUI test page.
//
// Two directions, both the FiveM shapes:
//   script -> page   SendNUIMessage lands here as a window 'message' event
//   page -> script   a POST to https://<resource>/<callbackName>
//
// GetParentResourceName() is defined for every page NUI serves, so the
// callback URL never has to be hardcoded.

(function () {
  'use strict';

  var root = document.getElementById('root');
  var who = document.getElementById('who');
  var note = document.getElementById('mode-note');
  var log = document.getElementById('log');

  var MODE_NOTES = {
    menu:
      'Focus + cursor. Gameplay input is parked while this is open - ' +
      'SetNuiFocus(true, true).',
    hud:
      'Focus WITH gameplay still live - SetNuiFocusKeepInput(true). Ride ' +
      'around while this is on screen; the pad still drives both.'
  };

  function write(line) {
    var stamp = new Date().toLocaleTimeString();
    log.textContent = stamp + '  ' + line + '\n' + log.textContent;
  }

  // POST to this resource's own origin. The host answers with the JSON the
  // Lua handler's cb() was given, or {} if it never answered.
  function callback(name, body) {
    return fetch('https://' + GetParentResourceName() + '/' + name, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body || {})
    }).then(function (response) {
      return response.json();
    });
  }

  window.addEventListener('message', function (event) {
    var data = event.data;
    if (!data || data.action !== 'setMode') {
      return;
    }
    var visible = data.mode === 'menu' || data.mode === 'hud';
    root.hidden = !visible;
    root.classList.toggle('hud', data.mode === 'hud');
    note.textContent = MODE_NOTES[data.mode] || '';
    who.textContent = data.player
      ? data.player + '  (id ' + data.playerId + ')'
      : 'no name set - launch with --skate3_multiplayer_player_name';
    if (visible) {
      // Give the first button DOM focus, so a controller has somewhere to
      // start even before the pointer has moved.
      document.getElementById('where').focus();
      write('shown in ' + data.mode + ' mode');
    }
  });

  document.getElementById('where').addEventListener('click', function () {
    callback('whereAmI').then(function (result) {
      if (typeof result.x === 'number') {
        write(
          'position ' +
            result.x.toFixed(1) + ', ' +
            result.y.toFixed(1) + ', ' +
            result.z.toFixed(1)
        );
      } else {
        write('position unavailable (not spawned yet)');
      }
    });
  });

  document.getElementById('spawn').addEventListener('click', function () {
    write('teleporting to origin...');
    callback('teleport', { x: 0, y: 0, z: 0 });
  });

  document.getElementById('close').addEventListener('click', function () {
    callback('close');
  });

  // Announce the page to the script as soon as it is alive. A "ready"
  // handshake is the usual FiveM pattern (a script must not SendNUIMessage
  // before the page can listen), and it doubles as proof the whole loop is
  // wired: the page was served, GetParentResourceName() was injected, the
  // POST reached Lua, and the answer came back.
  callback('ready', { href: location.href }).then(function (result) {
    write('handshake ok, resource = ' + (result.resource || '?'));
  });

  // B on the pad arrives as Escape, so this is the pad's back button too.
  document.addEventListener('keydown', function (event) {
    if (event.key === 'Escape' && !root.hidden) {
      callback('close');
    }
  });
})();
