var CONFIG_URL    = "https://hakki-max.github.io/Pebble_SgBusLaiLiao/config.html";
var BUS_ARRIVAL_URL = "https://datamall2.mytransport.sg/ltaodataservice/v3/BusArrival";
var BUS_STOPS_URL   = "https://datamall2.mytransport.sg/ltaodataservice/BusStops";

var KEY_CMD           = 0;
var KEY_STOP_CODE     = 1;
var KEY_ARRIVALS_JSON = 2;
var KEY_STOPS_JSON    = 3;
var KEY_ERROR         = 4;

var stopsCache       = {};
var stopsCacheLoaded = false;

var CACHE_KEY     = "sg_nextbus_stops";
var CACHE_TS_KEY  = "sg_nextbus_stops_ts";
var CACHE_MAX_AGE = 30 * 24 * 60 * 60 * 1000; // 30 days

// ── API key helpers ───────────────────────────────────────
function getApiKey() {
  try {
    var key = localStorage.getItem("lta_api_key") || "";
    // ── EMULATOR FALLBACK — remove before publishing ──
    if (!key) key = "";
    // ─────────────────────────────────────────────────
    return key;
  } catch(e) {
    return "YOUR_ACTUAL_KEY_HERE"; // emulator fallback
  }
}

function hasApiKey() {
  var key = getApiKey();
  return key !== null && key.length > 0;
}

// ── Ready ─────────────────────────────────────────────────
Pebble.addEventListener("ready", function() {
  console.log("SG NextBus JS ready");
  if (hasApiKey()) {
    loadStopsCache(function() {
      console.log("Stops cache ready: " + Object.keys(stopsCache).length + " stops");
    });
  } else {
    console.log("No API key set — open settings to configure");
  }
});

// ── Configuration page ────────────────────────────────────
Pebble.addEventListener("showConfiguration", function() {
  var currentKey = "";
  try { currentKey = localStorage.getItem("lta_api_key") || ""; } catch(e) {}
  var params = encodeURIComponent(JSON.stringify({ apiKey: currentKey }));
  var url = CONFIG_URL + "?" + params;
  console.log("Opening config page");
  Pebble.openURL(url);
});

Pebble.addEventListener("webviewclosed", function(e) {
  if (!e.response || e.response === "") {
    console.log("Config cancelled");
    return;
  }
  try {
    var result = JSON.parse(decodeURIComponent(e.response));
    if (result.apiKey && result.apiKey.length > 0) {
      localStorage.setItem("lta_api_key", result.apiKey);
      console.log("API key saved OK");
      // Reset and reload stops cache with new key
      stopsCache = {};
      stopsCacheLoaded = false;
      try {
        localStorage.removeItem(CACHE_KEY);
        localStorage.removeItem(CACHE_TS_KEY);
      } catch(e) {}
      loadStopsCache(function() {
        console.log("Cache reloaded: " + Object.keys(stopsCache).length + " stops");
      });
    }
  } catch(err) {
    console.log("Config parse error: " + err);
  }
});

// ── AppMessage from watch ─────────────────────────────────
Pebble.addEventListener("appmessage", function(e) {
  var msg = e.payload;
  var cmd = msg[KEY_CMD];
  console.log("CMD received: " + cmd);

  if (!hasApiKey()) {
    sendError("No API key");
    return;
  }

  if (cmd === "arrivals") {
    fetchArrivals(msg[KEY_STOP_CODE]);
  } else if (cmd === "nearby") {
    fetchNearby();
  }
});

// ── Stops cache (localStorage, 30-day expiry) ─────────────
function loadStopsCache(callback) {
  if (stopsCacheLoaded) { callback(); return; }

  try {
    var ts  = localStorage.getItem(CACHE_TS_KEY);
    var raw = localStorage.getItem(CACHE_KEY);
    var age = ts ? (Date.now() - parseInt(ts)) : Infinity;

    if (raw && age < CACHE_MAX_AGE) {
      stopsCache = JSON.parse(raw);
      stopsCacheLoaded = true;
      console.log("Stops from localStorage: " + Object.keys(stopsCache).length + " stops, age " + Math.round(age / 86400000) + " days");
      callback();
      return;
    } else {
      console.log("Cache expired or missing — fetching from LTA...");
    }
  } catch(e) {
    console.log("localStorage read error: " + e);
  }

  stopsCache = {};
  loadStopsCachePage(0, callback);
}

function loadStopsCachePage(skip, callback) {
  console.log("Fetching stops page skip=" + skip);
  var xhr = new XMLHttpRequest();
  xhr.open("GET", BUS_STOPS_URL + "?$skip=" + skip, true);
  xhr.setRequestHeader("AccountKey", getApiKey());
  xhr.setRequestHeader("accept", "application/json");
  xhr.onload = function() {
    if (xhr.status === 200) {
      try {
        var stops = JSON.parse(xhr.responseText).value;
        if (stops && stops.length > 0) {
          for (var i = 0; i < stops.length; i++) {
            stopsCache[stops[i].BusStopCode] = {
              desc: stops[i].Description || stops[i].BusStopCode,
              lat:  parseFloat(stops[i].Latitude),
              lon:  parseFloat(stops[i].Longitude)
            };
          }
          console.log("Cached " + Object.keys(stopsCache).length + " stops so far...");
          if (stops.length === 500) {
            loadStopsCachePage(skip + 500, callback);
            return;
          }
        }
        stopsCacheLoaded = true;
        console.log("All stops cached: " + Object.keys(stopsCache).length);
        try {
          localStorage.setItem(CACHE_KEY,    JSON.stringify(stopsCache));
          localStorage.setItem(CACHE_TS_KEY, Date.now().toString());
          console.log("Stops saved to localStorage");
        } catch(e) {
          console.log("localStorage write error: " + e);
        }
        callback();
      } catch(e) {
        console.log("Cache parse error: " + e);
        stopsCacheLoaded = true;
        callback();
      }
    } else {
      console.log("Cache HTTP error: " + xhr.status);
      stopsCacheLoaded = true;
      callback();
    }
  };
  xhr.onerror = function() {
    stopsCacheLoaded = true;
    callback();
  };
  xhr.send();
}

// ── Fetch arrivals ────────────────────────────────────────
function fetchArrivals(stopCode) {
  console.log("Fetching arrivals for stop: " + stopCode);
  var url = BUS_ARRIVAL_URL + "?BusStopCode=" + stopCode;
  var xhr = new XMLHttpRequest();
  xhr.open("GET", url, true);
  xhr.setRequestHeader("AccountKey", getApiKey());
  xhr.setRequestHeader("accept", "application/json");

  xhr.onload = function() {
    console.log("Arrivals HTTP status: " + xhr.status);
    if (xhr.status === 200) {
      try {
        var data = JSON.parse(xhr.responseText);
        var services = data.Services;
        if (!services || services.length === 0) {
          sendError("No buses now");
          return;
        }

        var parts = [];
        for (var i = 0; i < services.length; i++) {
          var svc = services[i];
          var buses = [svc.NextBus, svc.NextBus2, svc.NextBus3];
          for (var j = 0; j < buses.length; j++) {
            var bus = buses[j];
            if (!bus || !bus.EstimatedArrival || bus.EstimatedArrival === "") continue;
            var eta = calcEta(bus.EstimatedArrival);
            if (eta === null) continue;
            var load = encodeLoad(bus.Load);
            parts.push(svc.ServiceNo + "|" + eta + "|" + load);
          }
        }

        if (parts.length === 0) {
          sendError("No buses soon");
          return;
        }

        console.log("Total buses found: " + parts.length);
        var payload = parts.join(",");
        console.log("Payload length (chars): " + payload.length);
        console.log("Payload: " + payload);

        var desc = (stopsCache[stopCode] && stopsCache[stopCode].desc)
          ? stopsCache[stopCode].desc
          : stopCode;
        console.log("Stop desc: " + desc);

Pebble.sendAppMessage(
  { "2": payload },
  function() {
    console.log("Arrivals sent OK");
    Pebble.sendAppMessage(
      { "3": desc },
      function() { console.log("Stop desc sent OK"); },
      function(err) { console.log("Desc send failed: " + JSON.stringify(err)); }
    );
  },
  function(err) { console.log("Arrivals send failed: " + JSON.stringify(err)); }
);

      } catch(err) {
        console.log("Parse error: " + err);
        sendError("Parse error");
      }
    } else if (xhr.status === 401) {
      sendError("Invalid API key");
    } else {
      sendError("HTTP " + xhr.status);
    }
  };

  xhr.onerror = function() { sendError("Network error"); };
  xhr.send();
}

// ── Nearby stops ──────────────────────────────────────────
function fetchNearby() {
  console.log("Getting GPS...");
  if (!navigator.geolocation) {
    sendError("No GPS");
    return;
  }
  navigator.geolocation.getCurrentPosition(
    function(pos) {
      var lat = pos.coords.latitude;
      var lon = pos.coords.longitude;
      console.log("GPS: " + lat + ", " + lon);
      loadStopsCache(function() {
        processNearbyFromCache(lat, lon);
      });
    },
    function(err) {
      console.log("GPS error: " + err.message);
      sendError("GPS failed");
    },
    { timeout: 10000, maximumAge: 60000 }
  );
}

function processNearbyFromCache(lat, lon) {
  var withDist = [];
  for (var code in stopsCache) {
    var s = stopsCache[code];
    if (isNaN(s.lat) || isNaN(s.lon)) continue;
    withDist.push({
      code: code,
      desc: s.desc,
      dist: Math.round(distance(lat, lon, s.lat, s.lon))
    });
  }

  withDist.sort(function(a, b) { return a.dist - b.dist; });
  var nearest = withDist.slice(0, 5);

  var parts = nearest.map(function(s) {
    var desc = s.desc.replace(/[|,]/g, " ");
    return s.code + "|" + desc + "|" + s.dist;
  });
  var payload = parts.join(",");
  console.log("Nearby payload: " + payload);

  Pebble.sendAppMessage(
    { "3": payload },
    function() { console.log("Nearby sent OK"); },
    function(err) { console.log("Nearby send failed: " + JSON.stringify(err)); }
  );
}

// ── Helpers ───────────────────────────────────────────────
function calcEta(isoString) {
  try {
    var arrival = new Date(isoString).getTime();
    var now = Date.now();
    var mins = Math.round((arrival - now) / 60000);
    if (mins < -1) return null;
    if (mins <= 0) return "Arr";
    return mins + " min";
  } catch(e) {
    return null;
  }
}

function encodeLoad(load) {
  var map = { "SEA": "G", "SDA": "Y", "LSD": "R", "NA": "G" };
  return map[load] || "G";
}

function distance(lat1, lon1, lat2, lon2) {
  var R = 6371000;
  var dLat = (lat2 - lat1) * Math.PI / 180;
  var dLon = (lon2 - lon1) * Math.PI / 180;
  var a = Math.sin(dLat / 2) * Math.sin(dLat / 2) +
          Math.cos(lat1 * Math.PI / 180) * Math.cos(lat2 * Math.PI / 180) *
          Math.sin(dLon / 2) * Math.sin(dLon / 2);
  return R * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
}

function sendError(msg) {
  console.log("Error: " + msg);
  Pebble.sendAppMessage(
    { "4": msg },
    function() {},
    function() {}
  );
}