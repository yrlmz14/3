#include <Arduino.h>
#include <SPI.h>
#include <FS.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

// ------------------------------- Hardware config ------------------------------
// Adjust these pins for your specific ESP32-S3 + Waveshare wiring if needed.
static constexpr int TFT_CS = 10;
static constexpr int TFT_DC = 9;
static constexpr int TFT_RST = 14;
static constexpr int TFT_SCLK = 12;
static constexpr int TFT_MISO = 13;
static constexpr int TFT_MOSI = 11;
static constexpr int TFT_BL = 48;  // Set to -1 if your board has no BL control pin.

// ------------------------------- Wi-Fi AP config ------------------------------
static constexpr const char* AP_SSID = "ESP32S3-3D-Viewer";
static constexpr const char* AP_PASSWORD = "12345678";

// ------------------------------- Render config --------------------------------
static constexpr uint16_t TARGET_FRAME_MS = 80;  // ~12.5 FPS keeps web server responsive
static constexpr size_t MAX_VERTICES = 3500;
static constexpr size_t MAX_EDGES = 12000;
static constexpr size_t MAX_DRAW_EDGES_PER_FRAME = 2500;

SPIClass displaySPI(FSPI);
Adafruit_ILI9341 tft(&displaySPI, TFT_DC, TFT_CS, TFT_RST);
WebServer server(80);

struct Vec3 {
  float x;
  float y;
  float z;
};

struct Edge {
  uint32_t a;
  uint32_t b;
};

struct Model {
  std::vector<Vec3> vertices;
  std::vector<Edge> edges;
  String filename;
  bool loaded = false;
};

struct ScreenPoint {
  int16_t x;
  int16_t y;
  bool visible;
};

Model currentModel;
std::vector<ScreenPoint> projected;

String overlayText = "ESP32-S3 3D Engine";
String activeModelName = "";
float modelScale = 1.2f;
float angleY = 0.0f;
float angleX = 0.45f;
uint32_t lastFrame = 0;

File uploadFile;
bool uploadFailed = false;
String uploadError = "";
String uploadModelName = "";
String uploadTargetPath = "";

const char kFallbackHtml[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>ESP32 3D Controller</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 16px; background: #111; color: #eee; }
    .card { background:#1c1c1c; border-radius:12px; padding:16px; max-width:540px; }
    label { display:block; margin-top:12px; font-weight:600; }
    input, select, button { width:100%; margin-top:8px; padding:10px; border-radius:8px; border:1px solid #333; background:#222; color:#eee; }
    button { cursor:pointer; font-weight:700; }
    .row { margin-top:10px; }
    .ok { color:#66d07f; margin-top:10px; min-height:22px; }
    .err { color:#ff6d6d; margin-top:10px; min-height:22px; }
    a { color:#8dc3ff; }
  </style>
</head>
<body>
  <div class="card">
    <h2>ESP32-S3 3D Screen</h2>
    <div id="ap">Connect to AP: <b>ESP32S3-3D-Viewer</b> / <b>12345678</b></div>
    <label for="model">Model (.obj)</label>
    <select id="model"></select>
    <label for="text">Text overlay</label>
    <input id="text" maxlength="48" placeholder="Text on screen" />
    <label for="scale">Scale (0.2 - 4.0)</label>
    <input id="scale" type="number" min="0.2" max="4.0" step="0.1" value="1.2" />
    <div class="row"><button id="apply">Apply to display</button></div>
    <hr />
    <label for="objFile">Upload OBJ file</label>
    <input id="objFile" type="file" accept=".obj" />
    <div class="row"><button id="upload">Upload</button></div>
    <div id="ok" class="ok"></div>
    <div id="err" class="err"></div>
  </div>
  <script>
    const modelEl = document.getElementById('model');
    const textEl = document.getElementById('text');
    const scaleEl = document.getElementById('scale');
    const okEl = document.getElementById('ok');
    const errEl = document.getElementById('err');

    function setMsg(ok, err) { okEl.textContent = ok || ''; errEl.textContent = err || ''; }

    async function refreshState() {
      const r = await fetch('/api/models');
      const j = await r.json();
      modelEl.innerHTML = '';
      (j.models || []).forEach(m => {
        const o = document.createElement('option');
        o.value = m;
        o.textContent = m;
        modelEl.appendChild(o);
      });
      if (j.active) modelEl.value = j.active;
      if (j.text != null) textEl.value = j.text;
      if (j.scale != null) scaleEl.value = j.scale;
    }

    document.getElementById('apply').addEventListener('click', async () => {
      const body = new URLSearchParams({
        model: modelEl.value,
        text: textEl.value,
        scale: scaleEl.value
      });
      const r = await fetch('/api/select', { method: 'POST', body });
      const t = await r.text();
      if (!r.ok) setMsg('', t);
      else setMsg('Display updated.', '');
    });

    document.getElementById('upload').addEventListener('click', async () => {
      const fileInput = document.getElementById('objFile');
      if (!fileInput.files.length) { setMsg('', 'Choose an .obj file first.'); return; }
      const fd = new FormData();
      fd.append('obj', fileInput.files[0]);
      const r = await fetch('/api/upload', { method: 'POST', body: fd });
      const t = await r.text();
      if (!r.ok) setMsg('', t);
      else { setMsg('Upload complete.', ''); await refreshState(); }
    });

    refreshState().catch(e => setMsg('', e.message));
  </script>
</body>
</html>
)HTML";

static String jsonEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    if (c == '\\' || c == '"') {
      out += '\\';
      out += c;
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else {
      out += c;
    }
  }
  return out;
}

static String toLowerCopy(String value) {
  value.toLowerCase();
  return value;
}

static String baseName(const String& pathOrName) {
  const int slashPos = pathOrName.lastIndexOf('/');
  const int backslashPos = pathOrName.lastIndexOf('\\');
  const int sepPos = std::max(slashPos, backslashPos);
  if (sepPos < 0) return pathOrName;
  if (sepPos + 1 >= pathOrName.length()) return "";
  return pathOrName.substring(sepPos + 1);
}

static bool isObjName(const String& name) {
  return toLowerCopy(name).endsWith(".obj");
}

static bool isSafeModelName(const String& name) {
  if (!isObjName(name)) return false;
  if (name.indexOf('/') >= 0 || name.indexOf('\\') >= 0) return false;
  if (name.indexOf("..") >= 0) return false;
  return name.length() > 0 && name.length() <= 64;
}

static String sanitizeUploadName(const String& rawName) {
  String out;
  out.reserve(rawName.length());
  for (size_t i = 0; i < rawName.length(); ++i) {
    const char c = rawName[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
      out += c;
    } else if (c == ' ') {
      out += '_';
    }
  }
  out.toLowerCase();
  if (!out.endsWith(".obj")) out += ".obj";
  if (!isSafeModelName(out)) return "";
  return out;
}

static int parseObjIndexToken(const String& token, size_t vertexCount) {
  if (token.length() == 0) return -1;
  const int slashPos = token.indexOf('/');
  const String idxPart = (slashPos >= 0) ? token.substring(0, slashPos) : token;
  if (idxPart.length() == 0) return -1;

  const int idx = idxPart.toInt();
  if (idx == 0) return -1;
  if (idx > 0) {
    const int zeroBased = idx - 1;
    if (zeroBased < 0 || static_cast<size_t>(zeroBased) >= vertexCount) return -1;
    return zeroBased;
  }

  const int zeroBased = static_cast<int>(vertexCount) + idx;
  if (zeroBased < 0 || static_cast<size_t>(zeroBased) >= vertexCount) return -1;
  return zeroBased;
}

static void normalizeVertices(std::vector<Vec3>& vertices) {
  if (vertices.empty()) return;

  Vec3 vMin = vertices.front();
  Vec3 vMax = vertices.front();
  for (const Vec3& v : vertices) {
    vMin.x = std::min(vMin.x, v.x);
    vMin.y = std::min(vMin.y, v.y);
    vMin.z = std::min(vMin.z, v.z);
    vMax.x = std::max(vMax.x, v.x);
    vMax.y = std::max(vMax.y, v.y);
    vMax.z = std::max(vMax.z, v.z);
  }

  const Vec3 center = {(vMin.x + vMax.x) * 0.5f, (vMin.y + vMax.y) * 0.5f, (vMin.z + vMax.z) * 0.5f};
  const float dx = vMax.x - vMin.x;
  const float dy = vMax.y - vMin.y;
  const float dz = vMax.z - vMin.z;
  float maxExtent = std::max(dx, std::max(dy, dz));
  if (maxExtent < 0.0001f) {
    maxExtent = 1.0f;
  }

  // Normalize model into roughly [-1, 1] range on the largest axis.
  const float normScale = 2.0f / maxExtent;
  for (Vec3& v : vertices) {
    v.x = (v.x - center.x) * normScale;
    v.y = (v.y - center.y) * normScale;
    v.z = (v.z - center.z) * normScale;
  }
}

static bool loadObjFromPath(const String& path, Model& outModel, String& err) {
  File f = SPIFFS.open(path, "r");
  if (!f) {
    err = "Cannot open: " + path;
    return false;
  }

  std::vector<Vec3> vertices;
  std::vector<Edge> edges;
  std::set<uint64_t> edgeSet;

  String line;
  vertices.reserve(1000);
  edges.reserve(3000);

  while (f.available()) {
    line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line.startsWith("#")) continue;

    if (line.startsWith("v ")) {
      float x = 0, y = 0, z = 0;
      if (sscanf(line.c_str(), "v %f %f %f", &x, &y, &z) == 3) {
        if (vertices.size() >= MAX_VERTICES) {
          err = "Model has too many vertices";
          f.close();
          return false;
        }
        vertices.push_back({x, y, z});
      }
      continue;
    }

    if (line.startsWith("f ")) {
      String rest = line.substring(2);
      rest.trim();
      std::vector<int> face;
      face.reserve(8);

      int start = 0;
      while (start < rest.length()) {
        while (start < rest.length() && rest[start] == ' ') ++start;
        if (start >= rest.length()) break;
        int end = start;
        while (end < rest.length() && rest[end] != ' ') ++end;
        const String token = rest.substring(start, end);
        const int idx = parseObjIndexToken(token, vertices.size());
        if (idx >= 0) face.push_back(idx);
        start = end + 1;
      }

      if (face.size() >= 2) {
        for (size_t i = 0; i < face.size(); ++i) {
          uint32_t a = static_cast<uint32_t>(face[i]);
          uint32_t b = static_cast<uint32_t>(face[(i + 1) % face.size()]);
          if (a == b) continue;
          const uint32_t lo = std::min(a, b);
          const uint32_t hi = std::max(a, b);
          const uint64_t key = (static_cast<uint64_t>(lo) << 32) | hi;
          if (edgeSet.insert(key).second) {
            if (edges.size() >= MAX_EDGES) {
              err = "Model has too many edges";
              f.close();
              return false;
            }
            edges.push_back({a, b});
          }
        }
      }
    }
  }
  f.close();

  if (vertices.empty() || edges.empty()) {
    err = "Invalid OBJ (needs vertices and faces)";
    return false;
  }

  normalizeVertices(vertices);

  outModel.vertices = std::move(vertices);
  outModel.edges = std::move(edges);
  outModel.filename = path.substring(path.lastIndexOf('/') + 1);
  outModel.loaded = true;
  return true;
}

static std::vector<String> listModels() {
  std::vector<String> out;
  File root = SPIFFS.open("/");
  if (!root) {
    Serial.println("listModels: root open failed");
    return out;
  }

  if (!root.isDirectory()) {
    Serial.println("listModels: root is not directory, still trying openNextFile()");
  }

  File entry = root.openNextFile();
  while (entry) {
    const String name = entry.name();
    const String modelName = baseName(name);
    if (isSafeModelName(modelName)) {
      bool duplicate = false;
      for (const String& existing : out) {
        if (existing == modelName) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) out.push_back(modelName);
    }
    entry = root.openNextFile();
  }

  if (out.empty()) {
    if (SPIFFS.exists("/models/cube.obj")) out.push_back("cube.obj");
    if (SPIFFS.exists("/cube.obj")) {
      bool hasCube = false;
      for (const String& existing : out) {
        if (existing == "cube.obj") {
          hasCube = true;
          break;
        }
      }
      if (!hasCube) out.push_back("cube.obj");
    }
  }

  std::sort(out.begin(), out.end());
  Serial.printf("listModels: found %u model(s)\n", static_cast<unsigned>(out.size()));
  return out;
}

static String resolveModelPath(const String& modelName) {
  const String cleanName = baseName(modelName);
  if (!isSafeModelName(cleanName)) return "";

  const String pathInModels = "/models/" + cleanName;
  if (SPIFFS.exists(pathInModels)) return pathInModels;

  const String pathInRoot = "/" + cleanName;
  if (SPIFFS.exists(pathInRoot)) return pathInRoot;

  if (SPIFFS.exists(cleanName)) return cleanName;
  return "";
}

static File openModelForWrite(const String& modelName, String& outPath) {
  outPath = "";
  const String cleanName = baseName(modelName);
  if (!isSafeModelName(cleanName)) return File();

  File f = SPIFFS.open("/models/" + cleanName, "w");
  if (f) {
    outPath = "/models/" + cleanName;
    return f;
  }

  f = SPIFFS.open("/" + cleanName, "w");
  if (f) {
    outPath = "/" + cleanName;
    return f;
  }

  return File();
}

static bool loadModelByName(const String& modelName, String& err) {
  const String cleanName = baseName(modelName);
  if (!isSafeModelName(cleanName)) {
    err = "Bad model name";
    return false;
  }

  const String path = resolveModelPath(cleanName);
  if (path.length() == 0) {
    err = "Model not found in SPIFFS";
    return false;
  }

  Model m;
  if (!loadObjFromPath(path, m, err)) return false;
  currentModel = std::move(m);
  activeModelName = cleanName;
  projected.resize(currentModel.vertices.size());
  Serial.printf("Model loaded: %s (%s)\n", activeModelName.c_str(), path.c_str());
  return true;
}

static void ensureDefaultModel() {
  if (SPIFFS.exists("/models/cube.obj") || SPIFFS.exists("/cube.obj")) return;

  String targetPath;
  File f = openModelForWrite("cube.obj", targetPath);
  if (!f) {
    Serial.println("ensureDefaultModel: cannot create cube.obj");
    return;
  }

  f.print(
      "# Unit cube\n"
      "v -1 -1 -1\n"
      "v  1 -1 -1\n"
      "v  1  1 -1\n"
      "v -1  1 -1\n"
      "v -1 -1  1\n"
      "v  1 -1  1\n"
      "v  1  1  1\n"
      "v -1  1  1\n"
      "f 1 2 3 4\n"
      "f 5 6 7 8\n"
      "f 1 2 6 5\n"
      "f 2 3 7 6\n"
      "f 3 4 8 7\n"
      "f 4 1 5 8\n");
  f.close();
  Serial.printf("ensureDefaultModel: wrote %s\n", targetPath.c_str());
}

static void sendRootPage() {
  if (SPIFFS.exists("/index.html")) {
    File file = SPIFFS.open("/index.html", "r");
    if (file) {
      server.streamFile(file, "text/html");
      file.close();
      return;
    }
    Serial.println("sendRootPage: /index.html exists but open failed, using fallback");
  }
  server.send(200, "text/html", kFallbackHtml);
}

static void handleModelsApi() {
  const std::vector<String> models = listModels();
  String json = "{\"active\":\"" + jsonEscape(activeModelName) + "\",";
  json += "\"text\":\"" + jsonEscape(overlayText) + "\",";
  json += "\"scale\":" + String(modelScale, 2) + ",";
  json += "\"models\":[";
  for (size_t i = 0; i < models.size(); ++i) {
    if (i) json += ",";
    json += "\"" + jsonEscape(models[i]) + "\"";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

static void handleSelectApi() {
  if (!server.hasArg("model")) {
    server.send(400, "text/plain", "Missing model");
    return;
  }

  const String model = server.arg("model");
  if (model.length() == 0) {
    server.send(400, "text/plain", "Model name is empty");
    return;
  }

  const String text = server.hasArg("text") ? server.arg("text") : overlayText;
  const String scaleStr = server.hasArg("scale") ? server.arg("scale") : String(modelScale);

  float nextScale = scaleStr.toFloat();
  if (nextScale < 0.2f) nextScale = 0.2f;
  if (nextScale > 4.0f) nextScale = 4.0f;

  String err;
  if (!loadModelByName(model, err)) {
    server.send(400, "text/plain", "Load failed: " + err);
    return;
  }

  overlayText = text.substring(0, 48);
  modelScale = nextScale;
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleUploadStart() {
  uploadFailed = false;
  uploadError = "";
  uploadModelName = "";
  uploadTargetPath = "";
}

static void handleUploadData() {
  HTTPUpload& up = server.upload();

  if (up.status == UPLOAD_FILE_START) {
    handleUploadStart();
    const String cleanName = sanitizeUploadName(up.filename);
    if (cleanName.length() == 0) {
      uploadFailed = true;
      uploadError = "Invalid filename";
      Serial.printf("upload start rejected: raw='%s'\n", up.filename.c_str());
      return;
    }
    uploadModelName = cleanName;
    uploadFile = openModelForWrite(cleanName, uploadTargetPath);
    if (!uploadFile) {
      uploadFailed = true;
      uploadError = "Cannot create file in SPIFFS";
      Serial.printf("upload start failed: clean='%s'\n", cleanName.c_str());
    } else {
      Serial.printf("upload start: raw='%s' clean='%s' path='%s'\n", up.filename.c_str(), cleanName.c_str(), uploadTargetPath.c_str());
    }
    return;
  }

  if (up.status == UPLOAD_FILE_WRITE) {
    if (uploadFailed || !uploadFile) return;
    if (uploadFile.write(up.buf, up.currentSize) != up.currentSize) {
      uploadFailed = true;
      uploadError = "Write failed";
      uploadFile.close();
    }
    return;
  }

  if (up.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
    Serial.printf("upload end: bytes=%u path='%s'\n", static_cast<unsigned>(up.totalSize), uploadTargetPath.c_str());
    return;
  }

  if (up.status == UPLOAD_FILE_ABORTED) {
    uploadFailed = true;
    uploadError = "Upload aborted";
    if (uploadFile) uploadFile.close();
  }
}

static void handleUploadDone() {
  if (!uploadFailed && uploadModelName.length() > 0) {
    String err;
    Model modelTest;
    const String path = resolveModelPath(uploadModelName);
    if (path.length() == 0 || !loadObjFromPath(path, modelTest, err)) {
      uploadFailed = true;
      uploadError = "Invalid OBJ: " + err;
      if (path.length() > 0) {
        SPIFFS.remove(path);
      } else if (uploadTargetPath.length() > 0) {
        SPIFFS.remove(uploadTargetPath);
      }
      Serial.printf("upload validation failed: '%s' err='%s'\n", uploadModelName.c_str(), uploadError.c_str());
    } else {
      Serial.printf("upload validation OK: '%s'\n", uploadModelName.c_str());
    }
  }

  if (uploadFailed) {
    server.send(500, "text/plain", "Upload failed: " + uploadError);
    return;
  }
  server.send(200, "text/plain", "OK");
}

static void handleFsApi() {
  String out;
  out.reserve(1024);
  out += "total=" + String(SPIFFS.totalBytes()) + " used=" + String(SPIFFS.usedBytes()) + "\n";
  File root = SPIFFS.open("/");
  if (!root || !root.isDirectory()) {
    out += "root-not-directory\n";
    server.send(200, "text/plain", out);
    return;
  }

  File entry = root.openNextFile();
  while (entry) {
    out += entry.name();
    out += " (";
    out += String(static_cast<unsigned>(entry.size()));
    out += ")\n";
    entry = root.openNextFile();
  }
  server.send(200, "text/plain", out);
}

static void setupWebServer() {
  server.on("/", HTTP_GET, sendRootPage);
  server.on("/api/models", HTTP_GET, handleModelsApi);
  server.on("/api/select", HTTP_POST, handleSelectApi);
  server.on("/api/upload", HTTP_POST, handleUploadDone, handleUploadData);
  server.on("/api/fs", HTTP_GET, handleFsApi);

  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found");
  });

  server.begin();
  Serial.println("HTTP server started on port 80");
}

static void renderFrame() {
  const int16_t w = tft.width();
  const int16_t h = tft.height();

  tft.fillScreen(ILI9341_BLACK);

  if (!currentModel.loaded) {
    tft.setTextColor(ILI9341_YELLOW, ILI9341_BLACK);
    tft.setTextSize(2);
    tft.setCursor(10, 20);
    tft.print("No OBJ loaded");
    return;
  }

  if (projected.size() != currentModel.vertices.size()) {
    projected.resize(currentModel.vertices.size());
  }

  const float cy = cosf(angleY);
  const float sy = sinf(angleY);
  const float cx = cosf(angleX);
  const float sx = sinf(angleX);
  const float focal = 130.0f;
  const float zOffset = 4.0f;

  for (size_t i = 0; i < currentModel.vertices.size(); ++i) {
    const Vec3& v = currentModel.vertices[i];
    const float x = v.x * modelScale;
    const float y = v.y * modelScale;
    const float z = v.z * modelScale;

    const float x1 = (x * cy) - (z * sy);
    const float z1 = (x * sy) + (z * cy);
    const float y2 = (y * cx) - (z1 * sx);
    const float z2 = (y * sx) + (z1 * cx) + zOffset;

    if (z2 <= 0.1f) {
      projected[i] = {0, 0, false};
      continue;
    }

    const int16_t sxp = static_cast<int16_t>((x1 * focal / z2) + (w * 0.5f));
    const int16_t syp = static_cast<int16_t>((y2 * focal / z2) + (h * 0.5f));
    const bool visible = (sxp > -80 && sxp < (w + 80) && syp > -80 && syp < (h + 80));
    projected[i] = {sxp, syp, visible};
  }

  tft.startWrite();
  size_t step = 1;
  if (currentModel.edges.size() > MAX_DRAW_EDGES_PER_FRAME) {
    step = (currentModel.edges.size() + MAX_DRAW_EDGES_PER_FRAME - 1) / MAX_DRAW_EDGES_PER_FRAME;
  }

  size_t drawn = 0;
  size_t visibleLines = 0;
  for (size_t i = 0; i < currentModel.edges.size(); i += step) {
    const Edge& e = currentModel.edges[i];
    if (e.a >= projected.size() || e.b >= projected.size()) continue;
    const ScreenPoint& p0 = projected[e.a];
    const ScreenPoint& p1 = projected[e.b];
    if (!p0.visible && !p1.visible) continue;
    tft.drawLine(p0.x, p0.y, p1.x, p1.y, ILI9341_CYAN);
    ++visibleLines;
    ++drawn;
    if ((drawn & 0x7F) == 0) {
      yield();
    }
  }
  tft.endWrite();

  tft.setTextColor(ILI9341_WHITE, ILI9341_BLACK);
  tft.setTextSize(2);
  tft.setCursor(6, 6);
  if (overlayText.length() == 0) {
    tft.print(" ");
  } else {
    tft.print(overlayText);
  }

  tft.setTextSize(1);
  tft.setCursor(6, h - 10);
  tft.print(activeModelName);

  if (visibleLines == 0) {
    tft.setTextSize(1);
    tft.setTextColor(ILI9341_YELLOW, ILI9341_BLACK);
    tft.setCursor(6, h - 22);
    tft.print("Model has no visible edges");
  }
}

static void initDisplay() {
  if (TFT_BL >= 0) {
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
  }

  displaySPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_WHITE, ILI9341_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 20);
  tft.print("Starting...");
}

static void initWifiAp() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  const IPAddress ip = WiFi.softAPIP();

  Serial.printf("AP SSID: %s\n", AP_SSID);
  Serial.printf("AP Pass: %s\n", AP_PASSWORD);
  Serial.printf("Open: http://%s\n", ip.toString().c_str());

  tft.fillScreen(ILI9341_BLACK);
  tft.setCursor(10, 20);
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_GREEN, ILI9341_BLACK);
  tft.print("AP Ready");
  tft.setTextSize(1);
  tft.setCursor(10, 55);
  tft.print("SSID: ");
  tft.print(AP_SSID);
  tft.setCursor(10, 70);
  tft.print("PASS: ");
  tft.print(AP_PASSWORD);
  tft.setCursor(10, 85);
  tft.print("http://");
  tft.print(ip);
  delay(1500);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  initDisplay();

  if (!SPIFFS.begin(true)) {
    tft.fillScreen(ILI9341_BLACK);
    tft.setTextColor(ILI9341_RED, ILI9341_BLACK);
    tft.setCursor(10, 20);
    tft.print("SPIFFS failed");
    while (true) delay(1000);
  }

  ensureDefaultModel();
  Serial.printf("SPIFFS total=%u used=%u\n", static_cast<unsigned>(SPIFFS.totalBytes()), static_cast<unsigned>(SPIFFS.usedBytes()));

  std::vector<String> models = listModels();
  for (const String& m : models) {
    Serial.printf("model: %s\n", m.c_str());
  }

  String startupModel = "";
  for (const String& m : models) {
    if (m == "cube.obj") {
      startupModel = m;
      break;
    }
  }
  if (startupModel.length() == 0 && !models.empty()) {
    startupModel = models.front();
  }

  if (!models.empty()) {
    String err;
    if (!loadModelByName(startupModel, err)) {
      Serial.printf("Initial model load failed: %s\n", err.c_str());
    }
  }

  initWifiAp();
  setupWebServer();
}

void loop() {
  server.handleClient();
  server.handleClient();
  yield();

  const uint32_t now = millis();
  if (now - lastFrame >= TARGET_FRAME_MS) {
    lastFrame = now;
    angleY += 0.035f;
    if (angleY > 6.283185f) angleY -= 6.283185f;
    renderFrame();
  }
  delay(1);
}
