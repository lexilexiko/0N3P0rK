// WiFi AP + DOS Commander web file manager
#include "xfer.h"
#include "../cap/sniffer.h"
#include "../ui/display.h"
#include "../ui/keys.h"
#include "../core/app.h"
#include "../core/config.h"
#include "../piglet/avatar.h"
#include "../modes/usbsd.h"
#include <M5Cardputer.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SD.h>
#include <string.h>
#include <stdio.h>

extern const char XFER_HTML[] PROGMEM;

namespace XferMode {

static const char* apSsid() {
    const char* s = Config::xfer().ssid;
    return (s && s[0]) ? s : "0N3P0rK";
}
static const char* apPass() {
    const char* s = Config::xfer().pass;
    return (s && s[0]) ? s : "0N3-P0rK";
}
static const IPAddress AP_IP(192, 168, 4, 1);
static const IPAddress AP_GW(192, 168, 4, 1);
static const IPAddress AP_MASK(255, 255, 255, 0);

static bool s_run = false;
static bool s_keyLatch = false;
static WebServer* s_srv = nullptr;
static uint32_t s_hits = 0;
static char s_status[48] = "ready";

static void setStatus(const char* s) {
    strncpy(s_status, s, sizeof(s_status) - 1);
    s_status[sizeof(s_status) - 1] = 0;
}

// ---------- path helpers (SD only, stay under root) ----------
static bool safePath(const String& in, String& out) {
    String p = in;
    if (!p.length()) p = "/";
    if (p[0] != '/') p = "/" + p;
    // no .. escape
    if (p.indexOf("..") >= 0) return false;
    out = p;
    return true;
}

static String jsonEscape(const String& s) {
    String o;
    o.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '"' || c == '\\') {
            o += '\\';
            o += c;
        } else if (c == '\n' || c == '\r') {
            o += ' ';
        } else {
            o += c;
        }
    }
    return o;
}

static void handleRoot() {
    s_hits++;
    s_srv->send_P(200, "text/html", XFER_HTML);
}

static void handleList() {
    s_hits++;
    String path;
    if (!safePath(s_srv->arg("path"), path)) {
        s_srv->send(400, "text/plain", "bad path");
        return;
    }
    if (!SD.exists(path)) {
        s_srv->send(404, "text/plain", "not found");
        return;
    }
    File dir = SD.open(path);
    if (!dir || !dir.isDirectory()) {
        s_srv->send(400, "text/plain", "not a dir");
        return;
    }
    String json = "{\"path\":\"" + jsonEscape(path) + "\",\"entries\":[";
    bool first = true;
    int n = 0;
    File f = dir.openNextFile();
    while (f && n < 200) {
        const char* name = f.name();
        // strip path prefix if present
        const char* base = strrchr(name, '/');
        base = base ? base + 1 : name;
        if (base[0] == '.') {
            f = dir.openNextFile();
            continue;
        }
        String full = path;
        if (!full.endsWith("/")) full += "/";
        if (path == "/") full = String("/") + base;
        else full = path + "/" + base;

        if (!first) json += ",";
        first = false;
        if (f.isDirectory()) {
            json += "{\"name\":\"" + jsonEscape(base) + "\",\"dir\":true,\"path\":\"" +
                    jsonEscape(full) + "\",\"size\":0}";
        } else {
            json += "{\"name\":\"" + jsonEscape(base) + "\",\"dir\":false,\"path\":\"" +
                    jsonEscape(full) + "\",\"size\":" + String((uint32_t)f.size()) + "}";
        }
        n++;
        f = dir.openNextFile();
    }
    dir.close();
    json += "]}";
    s_srv->send(200, "application/json", json);
}

static void handleDownload() {
    s_hits++;
    String path;
    if (!safePath(s_srv->arg("path"), path)) {
        s_srv->send(400, "text/plain", "bad path");
        return;
    }
    if (!SD.exists(path)) {
        s_srv->send(404, "text/plain", "missing");
        return;
    }
    File f = SD.open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        s_srv->send(400, "text/plain", "not a file");
        return;
    }
    s_srv->streamFile(f, "application/octet-stream");
    f.close();
}

static void handleDelete() {
    s_hits++;
    String path;
    if (!safePath(s_srv->arg("path"), path)) {
        s_srv->send(400, "text/plain", "bad path");
        return;
    }
    if (path == "/" || path.length() < 2) {
        s_srv->send(403, "text/plain", "refuse root");
        return;
    }
    bool ok = false;
    if (SD.exists(path)) {
        File f = SD.open(path);
        bool isDir = f && f.isDirectory();
        f.close();
        if (isDir) ok = SD.rmdir(path);
        else ok = SD.remove(path);
    }
    s_srv->send(ok ? 200 : 500, "text/plain", ok ? "ok" : "fail");
}

static File s_upload;
static String s_uploadPath;

static void handleUpload() {
    HTTPUpload& up = s_srv->upload();
    if (up.status == UPLOAD_FILE_START) {
        String dir;
        if (!safePath(s_srv->arg("path"), dir)) dir = "/";
        if (!dir.endsWith("/")) dir += "/";
        if (dir == "//") dir = "/";
        s_uploadPath = dir + up.filename;
        // reject weird names
        if (s_uploadPath.indexOf("..") >= 0) {
            s_uploadPath = "";
            return;
        }
        if (SD.exists(s_uploadPath)) SD.remove(s_uploadPath);
        s_upload = SD.open(s_uploadPath, FILE_WRITE);
        setStatus("uploading...");
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (s_upload) s_upload.write(up.buf, up.currentSize);
    } else if (up.status == UPLOAD_FILE_END) {
        if (s_upload) {
            s_upload.close();
            setStatus("upload done");
        }
    }
}

static void handleUploadDone() {
    s_hits++;
    s_srv->send(200, "text/plain", "ok");
}

void start() {
    s_run = true;
    s_keyLatch = false;
    s_hits = 0;
    Avatar::suspendScene();
    if (Cap::isRunning()) Cap::stop();
    if (UsbSdMode::isRunning()) UsbSdMode::stop();

    WiFi.persistent(false);
    WiFi.disconnect(true, true);
    delay(50);
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_GW, AP_MASK);
    bool ok = WiFi.softAP(apSsid(), apPass(), 6, 0, 4);
    if (!ok) {
        setStatus("AP fail");
        Display::showToast("XFER AP FAIL", 1200);
    } else {
        setStatus("AP up");
        Display::showToast("XFER AP", 800);
    }

    if (!s_srv) s_srv = new WebServer(80);
    s_srv->on("/", HTTP_GET, handleRoot);
    s_srv->on("/api/list", HTTP_GET, handleList);
    s_srv->on("/api/download", HTTP_GET, handleDownload);
    s_srv->on("/api/delete", HTTP_POST, handleDelete);
    s_srv->on(
        "/api/upload", HTTP_POST, handleUploadDone,
        handleUpload);
    s_srv->onNotFound([]() {
        s_srv->sendHeader("Location", "/", true);
        s_srv->send(302, "text/plain", "");
    });
    s_srv->begin();
    setStatus(ok ? "open 192.168.4.1" : "AP fail");
}

void stop() {
    s_run = false;
    if (s_srv) {
        s_srv->stop();
        delete s_srv;
        s_srv = nullptr;
    }
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(40);
    Avatar::resumeScene();
}

void update() {
    if (!s_run) return;
    if (s_srv) s_srv->handleClient();

    if (!keyNewPress(s_keyLatch)) return;
    if (keyEsc()) {
        stop();
        return;
    }
}

void draw(M5Canvas& canvas) {
    canvas.fillSprite(0x0000);
    canvas.setTextSize(1);
    canvas.setTextColor(0x07E0, 0x0000);
    canvas.setCursor(4, 4);
    canvas.print("0N3P0rK COMMANDER");
    canvas.setCursor(4, 18);
    canvas.setTextColor(0x05A0, 0x0000);
    canvas.print("DOS file fortress");

    canvas.drawRect(2, 32, 236, 52, 0x07E0);
    canvas.setTextColor(0x07E0, 0x0000);
    canvas.setCursor(8, 38);
    canvas.printf("SSID  %s", apSsid());
    canvas.setCursor(8, 50);
    canvas.printf("PASS  %s", apPass());
    canvas.setCursor(8, 62);
    canvas.printf("URL   http://192.168.4.1");
    canvas.setCursor(8, 74);
    canvas.printf("STA   %u   HITS %u", (unsigned)WiFi.softAPgetStationNum(),
                  (unsigned)s_hits);

    canvas.setTextColor(0x05A0, 0x0000);
    canvas.setCursor(4, 92);
    canvas.print(s_status);
    canvas.setCursor(4, 104);
    canvas.setTextColor(0x03A0, 0x0000);
    canvas.print("` exit — phone joins AP");
}

bool isRunning() { return s_run; }

void getStatusLine(char* out, size_t n) {
    if (!out || !n) return;
    if (!s_run) {
        out[0] = 0;
        return;
    }
    snprintf(out, n, "XFER %u sta %s", (unsigned)WiFi.softAPgetStationNum(), s_status);
}

}  // namespace XferMode

// DOS Commander HTML page
const char XFER_HTML[] PROGMEM = R"XFER(
<!DOCTYPE html>
<html><head>
<meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>0N3P0rK COMMANDER</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#000;color:#0f0;font:14px/1.35 "Courier New",Consolas,monospace;min-height:100vh}
.wrap{max-width:960px;margin:0 auto;padding:8px}
.box{border:2px solid #0f0;padding:8px;margin:8px 0;box-shadow:0 0 0 1px #040}
h1{font-size:16px;letter-spacing:2px;text-align:center;margin:4px 0 8px}
.bar{display:flex;flex-wrap:wrap;gap:6px;margin:6px 0}
.bar a,.bar button,button,label.btn{
  background:#000;color:#0f0;border:1px solid #0f0;padding:4px 10px;
  font:inherit;cursor:pointer;text-decoration:none;display:inline-block}
.bar a:hover,button:hover,label.btn:hover{background:#0f0;color:#000}
.path{color:#0a0;margin:4px 0}
table{width:100%;border-collapse:collapse}
td,th{border-bottom:1px solid #030;padding:3px 6px;text-align:left}
tr:hover td{background:#020}
a{color:#0f0}
.dir a{color:#0ff;font-weight:bold}
.size{color:#080;text-align:right;white-space:nowrap}
.foot{margin-top:10px;border-top:1px solid #0f0;padding-top:6px;font-size:12px;color:#080}
.fkeys span{display:inline-block;margin-right:10px}
.fkeys b{color:#0f0;border:1px solid #0a0;padding:0 3px;margin-right:2px}
#msg{min-height:1.2em;color:#ff0;margin:4px 0}
input[type=file]{display:none}
</style></head><body><div class=wrap>
<div class=box>
<h1>╔═ 0N3P0rK COMMANDER ═╗</h1>
<div class=path>PATH: <span id=path>/</span></div>
<div class=bar>
<a href="#" id=up>.. UP</a>
<label class=btn>UPLOAD<input type=file id=file multiple></label>
<button type=button id=ref>REFRESH</button>
</div>
<div id=msg></div>
<table><thead><tr><th>NAME</th><th class=size>SIZE</th><th></th></tr></thead>
<tbody id=list></tbody></table>
<div class=foot>
<div class=fkeys>
<span><b>F1</b>Help</span>
<span><b>F3</b>View</span>
<span><b>F5</b>Copy↓</span>
<span><b>F8</b>Del</span>
<span><b>Alt-F4</b>Close tab</span>
</div>
<div>SSID → join AP → open http://192.168.4.1 — lab only</div>
</div>
</div>
<script>
const $=(s)=>document.querySelector(s);
let cur='/';
function msg(t){$('#msg').textContent=t||'';}
async function api(url,opt){
  const r=await fetch(url,opt);
  if(!r.ok)throw new Error(await r.text()||r.status);
  const ct=r.headers.get('content-type')||'';
  if(ct.includes('json'))return r.json();
  return r.text();
}
function fmt(n){
  if(n<1024)return n+' B';
  if(n<1048576)return (n/1024).toFixed(1)+' K';
  return (n/1048576).toFixed(1)+' M';
}
async function load(p){
  cur=p||'/';
  $('#path').textContent=cur;
  msg('listing...');
  try{
    const j=await api('/api/list?path='+encodeURIComponent(cur));
    const tb=$('#list');tb.innerHTML='';
    (j.entries||[]).forEach(e=>{
      const tr=document.createElement('tr');
      if(e.dir){
        tr.className='dir';
        tr.innerHTML='<td><a href="#" data-p="'+e.path+'">'+e.name+'/</a></td><td class=size>&lt;DIR&gt;</td><td></td>';
      }else{
        tr.innerHTML='<td><a href="/api/download?path='+encodeURIComponent(e.path)+'">'+e.name+'</a></td>'+
          '<td class=size>'+fmt(e.size)+'</td>'+
          '<td><button data-del="'+e.path+'">DEL</button></td>';
      }
      tb.appendChild(tr);
    });
    msg((j.entries||[]).length+' item(s)');
  }catch(e){msg('ERR '+e.message);}
}
$('#list').onclick=(ev)=>{
  const a=ev.target.closest('a[data-p]');
  if(a){ev.preventDefault();load(a.getAttribute('data-p'));return;}
  const b=ev.target.closest('button[data-del]');
  if(b){
    const p=b.getAttribute('data-del');
    if(!confirm('Delete '+p+' ?'))return;
    api('/api/delete?path='+encodeURIComponent(p),{method:'POST'}).then(()=>load(cur)).catch(e=>msg(e.message));
  }
};
$('#up').onclick=(e)=>{e.preventDefault();
  if(cur==='/'||cur==='')return;
  const i=cur.replace(/\/$/,'').lastIndexOf('/');
  load(i<=0?'/':cur.substring(0,i)||'/');
};
$('#ref').onclick=()=>load(cur);
$('#file').onchange=async()=>{
  const files=$('#file').files;if(!files.length)return;
  msg('upload...');
  for(const f of files){
    const fd=new FormData();fd.append('file',f,f.name);fd.append('path',cur);
    try{await api('/api/upload',{method:'POST',body:fd});}
    catch(e){msg('upload fail '+e.message);return;}
  }
  $('#file').value='';load(cur);
};
load('/');
</script></body></html>

)XFER";
