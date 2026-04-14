#include "shared.h"

// ================= WIFI FILE SERVER =================
// WiFi server global — only used in this module.
AsyncWebServer *webServer = nullptr;

// Embedded HTML — mobile-first dark theme file manager with tabbed photo/video views.
// Served from PROGMEM to avoid PSRAM/heap allocation for static content.
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Retro Cam</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',system-ui,sans-serif;background:#111;color:#e0e0e0;padding:16px;max-width:600px;margin:0 auto}
h1{font-size:1.4em;margin-bottom:12px;color:#fff}
.bar{background:#222;border-radius:8px;overflow:hidden;height:24px;margin-bottom:12px;position:relative}
.bar-fill{height:100%;background:linear-gradient(90deg,#2ecc71,#27ae60);transition:width .3s}
.bar-text{position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);font-size:.8em;color:#fff;white-space:nowrap}
.tabs{display:flex;gap:8px;margin-bottom:12px}
.tab{flex:1;padding:10px;border:none;border-radius:8px;cursor:pointer;font-size:.9em;font-weight:600;background:#222;color:#888;text-align:center;transition:all .2s}
.tab.active{background:#2980b9;color:#fff}
.tab .badge{display:inline-block;background:rgba(0,0,0,.3);border-radius:10px;padding:1px 7px;font-size:.75em;margin-left:4px}
.file{display:flex;align-items:center;background:#1a1a1a;border-radius:8px;padding:10px 12px;margin-bottom:8px;border:1px solid #2a2a2a}
.thumb{width:80px;height:45px;border-radius:4px;object-fit:cover;flex-shrink:0;background:#222}
.file-info{flex:1;min-width:0;margin-left:10px}
.file-name{font-size:.9em;word-break:break-all;color:#fff}
.file-size{font-size:.75em;color:#888;margin-top:2px}
.btns{display:flex;gap:6px;margin-left:8px;flex-shrink:0}
.btn{border:none;padding:8px 12px;border-radius:6px;cursor:pointer;font-size:.8em;font-weight:600;text-decoration:none;text-align:center}
.btn-dl{background:#2980b9;color:#fff}
.btn-del{background:#c0392b;color:#fff}
.btn:active{opacity:.7}
.empty{text-align:center;color:#666;padding:40px 0}
.err{text-align:center;color:#e74c3c;padding:30px 0;font-size:.9em}
.modal-bg{display:none;position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,.7);z-index:10;align-items:center;justify-content:center}
.modal-bg.show{display:flex}
.modal{background:#222;border-radius:12px;padding:24px;max-width:320px;width:90%;text-align:center}
.modal p{margin-bottom:16px;font-size:.95em}
.modal .btns{justify-content:center}
#status{text-align:center;color:#2ecc71;font-size:.85em;margin-bottom:12px;min-height:1.2em}
</style>
</head>
<body>
<h1>&#128247; Retro Cam Files</h1>
<div id="status"></div>
<div class="bar"><div class="bar-fill" id="barFill"></div><div class="bar-text" id="barText">Loading...</div></div>
<div class="tabs">
 <button class="tab active" id="tabPhotos" onclick="switchTab('photos')">&#128248; Photos <span class="badge" id="cntPhotos">0</span></button>
 <button class="tab" id="tabVideos" onclick="switchTab('videos')">&#127916; Videos <span class="badge" id="cntVideos">0</span></button>
</div>
<div id="list"><div class="empty">Loading...</div></div>
<div class="modal-bg" id="modalBg"><div class="modal"><p id="modalMsg">Delete?</p><div class="btns"><button class="btn btn-dl" onclick="closeModal()">Cancel</button><button class="btn btn-del" id="modalDel">Delete</button></div></div></div>
<script>
let delTarget='',allFiles=[],curTab='photos';
function switchTab(t){
 curTab=t;
 document.getElementById('tabPhotos').className='tab'+(t==='photos'?' active':'');
 document.getElementById('tabVideos').className='tab'+(t==='videos'?' active':'');
 renderFiles();
}
function renderFiles(){
 let el=document.getElementById('list');
 let filtered=allFiles.filter(f=>curTab==='videos'?f.isVideo:!f.isVideo);
 filtered.sort((a,b)=>a.name.localeCompare(b.name));
 if(!filtered.length){el.innerHTML='<div class="empty">No '+(curTab==='videos'?'videos':'photos')+' on SD card</div>';return;}
 let h='';
 filtered.forEach(f=>{
  let sz=f.size<1048576?(f.size/1024).toFixed(1)+' KB':(f.size/1048576).toFixed(1)+' MB';
  h+='<div class="file">';
  h+='<img class="thumb" loading="lazy" src="/api/preview?file='+encodeURIComponent(f.name)+'" alt="">';
  h+='<div class="file-info"><div class="file-name">'+f.name+'</div><div class="file-size">'+sz+'</div></div><div class="btns">';
  h+='<a class="btn btn-dl" download href="/api/download?file='+encodeURIComponent(f.name)+'">&#11015;</a>';
  h+='<button class="btn btn-del" onclick="confirmDel(\''+f.name.replace(/'/g,"\\'")+'\')">\&#128465;</button>';
  h+='</div></div>';
 });
 el.innerHTML=h;
}
function load(){
 fetch('/api/info').then(r=>r.json()).then(d=>{
  if(d.error){document.getElementById('barText').textContent='SD card unavailable';return;}
  let pct=((d.used/d.total)*100).toFixed(1);
  document.getElementById('barFill').style.width=pct+'%';
  let fmt=b=>(b/1048576).toFixed(1)+' MB';
  document.getElementById('barText').textContent=fmt(d.used)+' / '+fmt(d.total)+' ('+fmt(d.free)+' free)';
 }).catch(()=>{document.getElementById('barText').textContent='Connection error';});
 fetch('/api/files').then(r=>r.json()).then(files=>{
  if(files.error){document.getElementById('list').innerHTML='<div class="err">'+files.error+'</div>';return;}
  allFiles=files;
  let photos=files.filter(f=>!f.isVideo).length;
  let videos=files.filter(f=>f.isVideo).length;
  document.getElementById('cntPhotos').textContent=photos;
  document.getElementById('cntVideos').textContent=videos;
  renderFiles();
 }).catch(()=>{document.getElementById('list').innerHTML='<div class="err">Connection error</div>';});
}
function confirmDel(name){delTarget=name;document.getElementById('modalMsg').textContent='Delete '+name+'?';document.getElementById('modalBg').classList.add('show');}
function closeModal(){document.getElementById('modalBg').classList.remove('show');delTarget='';}
document.getElementById('modalDel').onclick=function(){
 if(!delTarget)return;
 let s=document.getElementById('status');
 s.textContent='Deleting...';s.style.color='#e67e22';
 fetch('/api/delete?file='+encodeURIComponent(delTarget)).then(r=>r.json()).then(d=>{
  closeModal();
  s.textContent=d.ok?'Deleted!':'Error: '+d.msg;
  s.style.color=d.ok?'#2ecc71':'#c0392b';
  setTimeout(()=>{s.textContent='';load();},1200);
 });
};
load();
</script>
</body>
</html>
)rawliteral";

// Start WiFi AP mode: deinit camera, start softAP + web server, show TFT info.
void startWiFiMode() {
  if (appState == STATE_WIFI_MODE) return;

  Serial.println("[WIFI] Entering WiFi mode...");

  // Tell other tasks to stop using camera & TFT immediately
  appState = STATE_WIFI_MODE;
  vTaskDelay(pdMS_TO_TICKS(150));

  // Deinit camera — frees PSRAM for WiFi stack
  esp_camera_deinit();

  // Start WiFi AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_SSID, WIFI_PASS);
  delay(100);
  IPAddress ip = WiFi.softAPIP();
  Serial.printf("[WIFI] AP started: %s / %s\n", WIFI_SSID, ip.toString().c_str());

  // Allocate and configure web server
  webServer = new AsyncWebServer(80);

  // Route: main page
  webServer->on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", INDEX_HTML);
  });

  // Route: SD info JSON — validates SD mount before access
  webServer->on("/api/info", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!globalSDState.isMounted) {
      request->send(200, "application/json", "{\"error\":\"SD card not mounted\",\"total\":0,\"used\":0,\"free\":0}");
      return;
    }
    uint64_t total = 0, used = 0;
    if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
      total = SD.totalBytes();
      used  = SD.usedBytes();
      xSemaphoreGive(spiMutex);
    }
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"total\":%llu,\"used\":%llu,\"free\":%llu}",
             total, used, total - used);
    request->send(200, "application/json", buf);
  });

  // Route: file list JSON — validates SD mount before scan
  webServer->on("/api/files", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!globalSDState.isMounted) {
      request->send(200, "application/json", "{\"error\":\"SD card not mounted\"}");
      return;
    }
    String json = "[";
    bool first = true;
    if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
      File root = SD.open("/");
      if (root) {
        File f = root.openNextFile();
        while (f) {
          if (!f.isDirectory()) {
            String name = f.name();
            // Only list .jpg and .avi files
            if (name.endsWith(".jpg") || name.endsWith(".avi")) {
              if (!first) json += ",";
              first = false;
              json += "{\"name\":\"";
              json += name;
              json += "\",\"size\":";
              json += String((unsigned long)f.size());
              json += ",\"isVideo\":";
              json += name.endsWith(".avi") ? "true" : "false";
              json += "}";
            }
          }
          f = root.openNextFile();
        }
        root.close();
      }
      xSemaphoreGive(spiMutex);
    }
    json += "]";
    request->send(200, "application/json", json);
  });

  // Route: download file (chunked stream, forced download via HTML download attr)
  // Uses shared_ptr<File> so the file is auto-closed if client disconnects mid-transfer.
  webServer->on("/api/download", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("file")) {
      request->send(400, "text/plain", "Missing file param");
      return;
    }
    if (!globalSDState.isMounted) {
      request->send(503, "text/plain", "SD card not mounted");
      return;
    }
    String filename = request->getParam("file")->value();
    if (!filename.startsWith("/")) filename = "/" + filename;

    // Open file with mutex — shared_ptr ensures RAII cleanup
    auto fp = std::make_shared<File>();
    bool opened = false;
    if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
      *fp = SD.open(filename, FILE_READ);
      opened = (bool)*fp;
      xSemaphoreGive(spiMutex);
    }
    if (!opened) {
      request->send(404, "text/plain", "File not found");
      return;
    }

    String contentType = filename.endsWith(".jpg") ? "image/jpeg" : "application/octet-stream";

    AsyncWebServerResponse *response = request->beginChunkedResponse(
      contentType.c_str(),
      [fp](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
        if (!*fp) return 0;
        size_t bytesRead = 0;
        if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
          bytesRead = fp->read(buffer, maxLen);
          if (bytesRead == 0) {
            fp->close();
          }
          xSemaphoreGive(spiMutex);
        }
        return bytesRead;
      }
    );
    request->send(response);
  });

  // Route: preview file inline (for thumbnail <img> tags — no Content-Disposition)
  // For .jpg files: streams the full file as before.
  // For .avi files: extracts frame 12 JPEG from MOVI list and sends it.
  webServer->on("/api/preview", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("file")) {
      request->send(400, "text/plain", "Missing file param");
      return;
    }
    if (!globalSDState.isMounted) {
      request->send(503, "text/plain", "SD not mounted");
      return;
    }
    String filename = request->getParam("file")->value();
    if (!filename.startsWith("/")) filename = "/" + filename;

    bool isAvi = filename.endsWith(".avi");

    if (isAvi) {
      // AVI video thumbnail: extract a single JPEG frame into PSRAM, send it.
      // We use a temporary ps_malloc buffer because the async callback would need
      // the file open across multiple chunks — but we want to extract and close.
      uint8_t *thumbBuf = (uint8_t*)ps_malloc(REC_BUF_SIZE);
      if (!thumbBuf) {
        request->send(500, "text/plain", "PSRAM alloc failed");
        return;
      }
      size_t thumbLen = 0;
      if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
        File aviFile = SD.open(filename, FILE_READ);
        if (aviFile) {
          thumbLen = extractAVIFrame(aviFile, thumbBuf, REC_BUF_SIZE, 12);
          aviFile.close();
        }
        xSemaphoreGive(spiMutex);
      }
      if (thumbLen == 0) {
        free(thumbBuf);
        request->send(404, "text/plain", "No frame found");
        return;
      }
      // Wrap buffer in shared_ptr for automatic cleanup after response
      auto bufPtr = std::shared_ptr<uint8_t>(thumbBuf, free);
      size_t totalLen = thumbLen;
      AsyncWebServerResponse *response = request->beginChunkedResponse(
        "image/jpeg",
        [bufPtr, totalLen](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
          if (index >= totalLen) return 0;
          size_t remain = totalLen - index;
          size_t toSend = (remain < maxLen) ? remain : maxLen;
          memcpy(buffer, bufPtr.get() + index, toSend);
          return toSend;
        }
      );
      response->addHeader("Cache-Control", "max-age=300");
      request->send(response);
    } else {
      // JPEG photo: stream directly from file
      auto fp = std::make_shared<File>();
      bool opened = false;
      if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
        *fp = SD.open(filename, FILE_READ);
        opened = (bool)*fp;
        xSemaphoreGive(spiMutex);
      }
      if (!opened) {
        request->send(404, "text/plain", "Not found");
        return;
      }

      AsyncWebServerResponse *response = request->beginChunkedResponse(
        "image/jpeg",
        [fp](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
          if (!*fp) return 0;
          size_t bytesRead = 0;
          if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
            bytesRead = fp->read(buffer, maxLen);
            if (bytesRead == 0) {
              fp->close();
            }
            xSemaphoreGive(spiMutex);
          }
          return bytesRead;
        }
      );
      // Cache thumbnails in browser for 5 min — avoid re-downloading on page reload
      response->addHeader("Cache-Control", "max-age=300");
      request->send(response);
    }
  });

  // Route: delete file — validates SD mount
  webServer->on("/api/delete", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("file")) {
      request->send(400, "application/json", "{\"ok\":false,\"msg\":\"Missing file param\"}");
      return;
    }
    if (!globalSDState.isMounted) {
      request->send(200, "application/json", "{\"ok\":false,\"msg\":\"SD card not mounted\"}");
      return;
    }
    String filename = request->getParam("file")->value();
    if (!filename.startsWith("/")) filename = "/" + filename;

    bool ok = false;
    if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
      ok = SD.remove(filename);
      xSemaphoreGive(spiMutex);
    }
    if (ok) {
      request->send(200, "application/json", "{\"ok\":true}");
      Serial.printf("[WIFI] Deleted: %s\n", filename.c_str());
    } else {
      request->send(200, "application/json", "{\"ok\":false,\"msg\":\"Delete failed\"}");
    }
  });

  webServer->begin();
  Serial.println("[WIFI] Web server started on port 80.");

  // Draw info screen on TFT
  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(middle_center);
    tft.setTextColor(TFT_CYAN);
    tft.setTextSize(2);
    tft.drawString("WiFi Active", tft.width() / 2, 50);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(2);
    tft.drawString("SSID:", tft.width() / 2, 95);
    tft.setTextColor(TFT_GREEN);
    tft.drawString(WIFI_SSID, tft.width() / 2, 120);
    tft.setTextColor(TFT_WHITE);
    tft.drawString("IP:", tft.width() / 2, 155);
    tft.setTextColor(TFT_YELLOW);
    tft.drawString(ip.toString().c_str(), tft.width() / 2, 180);
    tft.setTextColor(0x7BEF);  // dim grey
    tft.setTextSize(1);
    tft.drawString("Press to exit", tft.width() / 2, 220);
    tft.setTextDatum(top_left);
    xSemaphoreGive(spiMutex);
  }
}

// Stop WiFi mode: tear down server + AP, reinit camera, return to IDLE.
void stopWiFiMode() {
  if (appState != STATE_WIFI_MODE) return;

  Serial.println("[WIFI] Exiting WiFi mode...");

  if (webServer) {
    webServer->end();
    delete webServer;
    webServer = nullptr;
  }

  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("[WIFI] AP stopped.");

  // Reinit camera
  initCamera(IDLE_MODE, IDLE_RESOLUTION, IDLE_JPEG_QUALITY);

  // Clear TFT
  if (xSemaphoreTake(spiMutex, portMAX_DELAY)) {
    tft.fillScreen(TFT_BLACK);
    xSemaphoreGive(spiMutex);
  }

  appState = STATE_IDLE;
  Serial.println("[WIFI] Back to IDLE.");
}
