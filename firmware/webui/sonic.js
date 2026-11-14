(function () {
  var MARK = 1800;
  var SPACE = 1500;
  var SAMPLE_RATE = 44100;
  var BIT_RATE = 100;
  var START_BYTES = [0x01, 0x02];
  var END_BYTES = [0x03, 0x04];

  function $(id) {
    return document.getElementById("sonic-" + id) || document.getElementById(id);
  }

  function checksum(data) {
    return data.reduce(function (sum, b) {
      return (sum + b) & 0xff;
    }, 0);
  }

  function toBits(byte) {
    var bits = [];
    for (var i = 7; i >= 0; i--) bits.push((byte >> i) & 1);
    return bits;
  }

  function afskModulate(bits) {
    var samplesPerBit = SAMPLE_RATE / BIT_RATE;
    var totalSamples = Math.floor(bits.length * samplesPerBit);
    var buffer = new Float32Array(totalSamples);
    for (var i = 0; i < bits.length; i++) {
      var freq = bits[i] ? MARK : SPACE;
      for (var j = 0; j < samplesPerBit; j++) {
        var t = (i * samplesPerBit + j) / SAMPLE_RATE;
        buffer[i * samplesPerBit + j] = Math.sin(2 * Math.PI * freq * t);
      }
    }
    return buffer;
  }

  function floatTo16BitPCM(floatSamples) {
    var buffer = new Uint8Array(floatSamples.length * 2);
    for (var i = 0; i < floatSamples.length; i++) {
      var s = Math.max(-1, Math.min(1, floatSamples[i]));
      var val = s < 0 ? s * 0x8000 : s * 0x7fff;
      buffer[i * 2] = val & 0xff;
      buffer[i * 2 + 1] = (val >> 8) & 0xff;
    }
    return buffer;
  }

  function buildWav(pcm) {
    var wavHeader = new Uint8Array(44);
    var dataLen = pcm.length;
    var fileLen = 36 + dataLen;
    function writeStr(offset, str) {
      for (var i = 0; i < str.length; i++) wavHeader[offset + i] = str.charCodeAt(i);
    }
    function write32(offset, value) {
      wavHeader[offset] = value & 0xff;
      wavHeader[offset + 1] = (value >> 8) & 0xff;
      wavHeader[offset + 2] = (value >> 16) & 0xff;
      wavHeader[offset + 3] = (value >> 24) & 0xff;
    }
    function write16(offset, value) {
      wavHeader[offset] = value & 0xff;
      wavHeader[offset + 1] = (value >> 8) & 0xff;
    }
    writeStr(0, "RIFF");
    write32(4, fileLen);
    writeStr(8, "WAVE");
    writeStr(12, "fmt ");
    write32(16, 16);
    write16(20, 1);
    write16(22, 1);
    write32(24, SAMPLE_RATE);
    write32(28, SAMPLE_RATE * 2);
    write16(32, 2);
    write16(34, 16);
    writeStr(36, "data");
    write32(40, dataLen);
    return new Blob([wavHeader, pcm], { type: "audio/wav" });
  }

  window.stopPlay = function () {
    var audio = $("player") || $("loopCheck") && document.getElementById("player");
    audio = document.getElementById("sonic-player") || document.getElementById("player");
    if (!audio) return;
    audio.pause();
    audio.onended = null;
  };

  window.generate = function () {
    window.stopPlay();
    var ssidEl = document.getElementById("sonic-ssid") || document.getElementById("ssid");
    var pwdEl = document.getElementById("sonic-pwd") || document.getElementById("pwd");
    var loopEl = document.getElementById("sonic-loop") || document.getElementById("loopCheck");
    var audio = document.getElementById("sonic-player") || document.getElementById("player");
    if (!ssidEl || !pwdEl || !audio) return;
    var ssid = ssidEl.value.trim();
    var pwd = pwdEl.value.trim();
    var dataStr = ssid + "\n" + pwd;
    var textBytes = Array.from(new TextEncoder().encode(dataStr));
    var fullBytes = START_BYTES.concat(textBytes, [checksum(textBytes)], END_BYTES);
    var bits = [];
    fullBytes.forEach(function (b) {
      bits = bits.concat(toBits(b));
    });
    var wavBlob = buildWav(floatTo16BitPCM(afskModulate(bits)));
    audio.src = URL.createObjectURL(wavBlob);
    audio.load();
    audio.play();
    if (loopEl && loopEl.checked) {
      audio.onended = function () {
        audio.currentTime = 0;
        audio.play();
      };
    }
  };
})();
