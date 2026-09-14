// Prompt shown while the player stands inside a zone. Receives the same
// window 'message' events every NUI page does.
(function () {
  'use strict';

  var prompt = document.getElementById('prompt');
  var text = document.getElementById('text');

  window.addEventListener('message', function (event) {
    var data = event.data;
    if (!data || data.action !== 'prompt') {
      return;
    }
    if (data.visible) {
      text.textContent = data.text || '';
    }
    prompt.hidden = !data.visible;
  });
})();
