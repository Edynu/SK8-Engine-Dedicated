// Draws the markers the client publishes each sample.
//
// Elements are REUSED per marker id rather than rebuilt each message: at
// ~30 messages a second, replacing the DOM every time would restart the
// hold-fill transition and make the progress bar stutter, quite apart from
// the garbage.
(function () {
  'use strict';

  var layer = document.getElementById('layer');
  var radar = document.getElementById('radar');
  var blipLayer = document.getElementById('blips');
  var nodes = {};   // marker id -> { root, glyph, name, prompt, fill, label }
  var blips = {};   // marker id -> element

  var GLYPHS = {
    signup: '▲',
    info: 'ℹ',
    start: '▶',
  };

  function create(marker) {
    var root = document.createElement('div');
    root.className = 'marker';

    var glyph = document.createElement('div');
    glyph.className = 'glyph';
    glyph.textContent = GLYPHS[marker.type] || '▲';

    var name = document.createElement('div');
    name.className = 'name';

    var prompt = document.createElement('div');
    prompt.className = 'prompt';
    prompt.hidden = true;

    var fill = document.createElement('div');
    fill.className = 'fill';

    var dpad = document.createElement('div');
    dpad.className = 'dpad';
    dpad.textContent = '▲';

    var label = document.createElement('span');

    prompt.appendChild(fill);
    prompt.appendChild(dpad);
    prompt.appendChild(label);

    root.appendChild(glyph);
    root.appendChild(name);
    root.appendChild(prompt);
    layer.appendChild(root);

    return { root: root, glyph: glyph, name: name, prompt: prompt,
             fill: fill, label: label };
  }

  function render(data) {
    var seen = {};
    var list = data.markers || [];

    for (var i = 0; i < list.length; i++) {
      var marker = list[i];
      seen[marker.id] = true;

      var node = nodes[marker.id];
      if (!node) {
        node = nodes[marker.id] = create(marker);
      }

      // Percentages, not pixels: the client sends normalized 0..1 screen
      // fractions, so the overlay stays correct at any resolution or
      // render scale without knowing either.
      node.root.style.left = (marker.x * 100) + '%';
      node.root.style.top = (marker.y * 100) + '%';
      node.root.className = 'marker ' + (marker.near ? 'near' : 'far');
      node.glyph.textContent = GLYPHS[marker.type] || '▲';
      node.name.textContent = marker.text || '';

      var active = data.activeId === marker.id;
      node.prompt.hidden = !active;
      if (active) {
        node.label.textContent = data.prompt || 'Sign Up';
        node.fill.style.width = ((data.progress || 0) * 100) + '%';
      }
    }

    // Anything not in this message is out of range or behind the camera.
    for (var id in nodes) {
      if (!seen[id]) {
        layer.removeChild(nodes[id].root);
        delete nodes[id];
      }
    }
  }

  // Blips are placed as percentages of the radar, from the -1..1 radar-local
  // coordinates the client computes. Reused per id for the same reason the
  // markers are: rebuilding them would restart the near-marker pulse every
  // frame and leave it permanently at the start of its animation.
  function renderRadar(list) {
    // Hidden when there is nothing to point at. An empty ring is furniture:
    // this resource is always started so a game mode can call CreateMarker,
    // and a permanent radar with no blips is a cost every player pays for a
    // feature nobody asked for yet.
    if (!list || list.length === 0) {
      radar.hidden = true;
      return;
    }
    radar.hidden = false;

    var seen = {};
    for (var i = 0; i < list.length; i++) {
      var blip = list[i];
      seen[blip.id] = true;

      var node = blips[blip.id];
      if (!node) {
        node = blips[blip.id] = document.createElement('div');
        blipLayer.appendChild(node);
      }
      node.className = 'blip' + (blip.near ? ' near' : '');
      node.style.left = ((blip.x * 0.5 + 0.5) * 100) + '%';
      node.style.top = ((blip.y * 0.5 + 0.5) * 100) + '%';
    }

    for (var id in blips) {
      if (!seen[id]) {
        blipLayer.removeChild(blips[id]);
        delete blips[id];
      }
    }
  }

  window.addEventListener('message', function (event) {
    var data = event.data;
    if (data && data.action === 'markers') {
      render(data);
      renderRadar(data.radar);
    }
  });
})();
