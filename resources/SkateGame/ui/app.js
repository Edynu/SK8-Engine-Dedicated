// Renders the scoreboard, the projected play-area box, and the trick menu.
//
// The scoreboard is rebuilt from the snapshot each message, which is fine at
// this size and removes any chance of it disagreeing with the server. The
// trick LIST is not: it arrives once on its own message and is kept here, so
// a state broadcast cannot wipe what the player is typing into the filter.
(function () {
  'use strict';

  var areaSvg = document.getElementById('area');
  var areaEdges = document.getElementById('areaEdges');
  var board = document.getElementById('board');
  var playersEl = document.getElementById('players');
  var statusEl = document.getElementById('status');
  var logEl = document.getElementById('log');
  var toast = document.getElementById('toast');
  var menu = document.getElementById('menu');
  var listEl = document.getElementById('list');
  var filterEl = document.getElementById('filter');
  var closeEl = document.getElementById('close');

  var lastMenuOpen = false;
  var toastTimer = null;

  function post(name, body) {
    // The resource's own https origin - every NUI callback is scoped to the
    // resource that registered it.
    fetch('https://SkateGame/' + name, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body || {}),
    }).catch(function () { /* the game closed the page; nothing to do */ });
  }

  // ------------------------------------------------------------ area box

  function renderArea(data) {
    var corners = data.corners;
    var edges = data.edges;
    if (!data.visible || !corners || !edges) {
      areaSvg.hidden = true;
      return;
    }
    areaSvg.hidden = false;
    areaSvg.classList.toggle('editing', !!data.editingArea);

    var svg = '';
    for (var i = 0; i < edges.length; i++) {
      var a = corners[edges[i][0]];
      var b = corners[edges[i][1]];
      // `false` means that corner is behind the camera. An edge with a
      // missing end is dropped rather than drawn to a fabricated point,
      // which would streak a line across the screen.
      if (!a || !b) {
        continue;
      }
      svg += '<line x1="' + (a.x * 1000) + '" y1="' + (a.y * 1000) +
             '" x2="' + (b.x * 1000) + '" y2="' + (b.y * 1000) + '"/>';
    }
    areaEdges.innerHTML = svg;
  }

  // ---------------------------------------------------------- scoreboard

  function renderWord(letters, total) {
    var html = '<div class="word">';
    for (var i = 0; i < total.length; i++) {
      html += '<span class="' + (i < letters ? 'got' : '') + '">' +
              total.charAt(i) + '</span>';
    }
    return html + '</div>';
  }

  function escapeHtml(text) {
    return String(text === undefined || text === null ? '' : text)
      .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  function renderBoard(data) {
    var players = data.players || [];
    // Not joined means not shown, even when a game IS running with other
    // players in it: the state broadcast reaches every client, and a
    // scoreboard for a game you are not in is clutter over someone's game.
    if (!data.visible || !data.joined) {
      board.hidden = true;
      return;
    }
    board.hidden = false;

    var word = data.word || 'SKATE';
    var html = '';
    for (var i = 0; i < players.length; i++) {
      var p = players[i];
      var classes = ['row'];
      if (p.isAttempting) { classes.push('attempting'); }
      if (p.isSetter) { classes.push('setter'); }
      if (p.out) { classes.push('out'); }
      if (p.id === data.me) { classes.push('self'); }
      html += '<div class="' + classes.join(' ') + '">' +
              '<span class="name">' + escapeHtml(p.name) + '</span>' +
              renderWord(p.letters || 0, word) +
              '</div>';
    }
    playersEl.innerHTML = html;

    var status = '';
    if (data.phase === 'lobby') {
      status = 'Lobby' + (data.isHost ? ' - "skatestart" when ready' : '') +
               ' - ' + players.length + ' in';
    } else if (data.phase === 'over') {
      var winner = null;
      for (var w = 0; w < players.length; w++) {
        if (players[w].id === data.winner) { winner = players[w]; }
      }
      status = winner
        ? '<span class="turn">' + escapeHtml(winner.name) + ' wins</span>'
        : 'Game over';
    } else if (data.phase === 'set') {
      status = data.trick
        ? 'Set called: <span class="trick">' + escapeHtml(data.trick.name) +
          '</span>'
        : 'Setting a trick';
      if (data.myTurn) {
        // Two distinct instructions inside one phase - see mustLand.
        status += data.mustLand
          ? '<br><span class="turn">Your set</span> - land it'
          : '<br><span class="turn">Your set</span> - pick a trick';
      }
    } else if (data.phase === 'match') {
      status = 'Match: <span class="trick">' +
               escapeHtml(data.trick ? data.trick.name : '?') + '</span>';
      if (data.myTurn) {
        status += '<br><span class="turn">Your go</span> - land it or take a ' +
                  'letter';
      }
    }
    if (data.remaining !== undefined && data.remaining !== null &&
        (data.phase === 'set' || data.phase === 'match')) {
      status += '<br><span class="clock">' + data.remaining + 's</span> left';
    }
    statusEl.innerHTML = status;

    var log = data.log || [];
    var logHtml = '';
    for (var l = 0; l < log.length; l++) {
      logHtml += escapeHtml(log[l]) + '<br>';
    }
    logEl.innerHTML = logHtml;
  }

  // ---------------------------------------------------------- trick menu

  function renderList(tricks) {
    var filter = (filterEl.value || '').toLowerCase();
    var html = '';
    var group = null;
    var shown = 0;
    for (var i = 0; i < tricks.length; i++) {
      var t = tricks[i];
      if (filter && t.name.toLowerCase().indexOf(filter) === -1) {
        continue;
      }
      if (t.group !== group) {
        group = t.group;
        html += '<div class="group">' + escapeHtml(group) + '</div>';
      }
      html += '<button class="trick" type="button" data-id="' + t.id +
              '" data-name="' + escapeHtml(t.name) + '">' +
              escapeHtml(t.name) + '</button>';
      shown++;
    }
    if (shown === 0) {
      html = '<div class="empty">No tricks match.</div>';
    }
    listEl.innerHTML = html;
  }

  // The list itself arrives on its own 'skateMenu' message and is kept here;
  // the per-frame state message only says whether the menu is up. That is
  // what stops a state broadcast from wiping what the player is typing.
  var trickList = [];

  function renderMenu(data) {
    if (!data.visible || !data.menuOpen) {
      menu.hidden = true;
      lastMenuOpen = false;
      return;
    }
    menu.hidden = false;
    if (!lastMenuOpen) {
      lastMenuOpen = true;
      filterEl.value = '';
      renderList(trickList);
      filterEl.focus();
    }
  }

  filterEl.addEventListener('input', function () {
    renderList(trickList);
  });

  listEl.addEventListener('click', function (event) {
    var button = event.target.closest('.trick');
    if (!button) {
      return;
    }
    post('pickTrick', {
      id: parseInt(button.getAttribute('data-id'), 10),
      name: button.getAttribute('data-name'),
    });
  });

  closeEl.addEventListener('click', function () {
    post('closeMenu', {});
  });

  document.addEventListener('keydown', function (event) {
    if (event.key === 'Escape' && !menu.hidden) {
      post('closeMenu', {});
    }
  });

  // -------------------------------------------------------------- events

  window.addEventListener('message', function (event) {
    var data = event.data;
    if (!data) {
      return;
    }
    if (data.action === 'skateToast') {
      toast.textContent = data.text || '';
      toast.hidden = false;
      if (toastTimer) {
        clearTimeout(toastTimer);
      }
      toastTimer = setTimeout(function () { toast.hidden = true; }, 3200);
      return;
    }
    if (data.action === 'skateMenu') {
      trickList = data.tricks || [];
      renderList(trickList);
      return;
    }
    if (data.action !== 'skate') {
      return;
    }
    if (!data.visible) {
      // Clear a toast still counting down, so nothing survives the hide.
      toast.hidden = true;
      if (toastTimer) {
        clearTimeout(toastTimer);
        toastTimer = null;
      }
    }
    renderArea(data);
    renderBoard(data);
    renderMenu(data);
  });
})();
