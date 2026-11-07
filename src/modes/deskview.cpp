#include "deskview.h"

#include "../net/ap_sta.h"
#include "../storage/littlefs_ops.h"
#include <SD.h>
#include "../audio/sfx.h"
#include "../piglet/avatar.h"
#include "../core/app.h"
#include "../ui/display.h"
#include "../ui/keys.h"
#include "badusb_hid.h"
#include "usbsd.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>

namespace DeskMode {

static const uint16_t kPort = 1234;
static const uint16_t kChunk = 1400;
static const uint8_t kMaxChunks = 12;
static const size_t kBufBytes = (size_t)kChunk * kMaxChunks;
// WiFiUDP::begin mallocs this tx buffer. stop() frees it.
static const size_t kUdpTxBytes = 1460;

// A later start only runs on3desk.py. The long pip install is for the first PC.
static const char kPs[] = "powershell\n";
static const char kHead[] =
    "Set-Content -Path on3desk.py -Encoding ascii -Value @'\n"
    "import mss,cv2,socket,numpy as np,struct,time,os,sys,ctypes\n"
    "UDP_IP=sys.argv[1] if len(sys.argv)>1 else '0.0.0.0'\n";
static const char kTail[] =
    R"PY(ctypes.windll.user32.ShowWindow(ctypes.windll.kernel32.GetConsoleWindow(),6)
UDP_PORT=1234
sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET,socket.SO_SNDBUF,2097152)
sock.setblocking(False)
sct=mss.mss()
mon=sct.monitors[1]
fid=0
z=1.0
q=50
px=0.5
py=0.5
w=mon['width']
h=mon['height']
while True:
    try:
        while True:
            d,a=sock.recvfrom(64)
            c=d.decode()
            if c=='STOP':
                os._exit(0)
            elif c=='Z+':
                z=max(0.05,z-0.05)
            elif c=='Z-':
                z=min(1.0,z+0.05)
            elif c=='Z0':
                z=1.0
                px=0.5
                py=0.5
            elif c=='U':
                py=max(0.0,py-z/2)
            elif c=='D':
                py=min(1.0,py+z/2)
            elif c=='L':
                px=max(0.0,px-z/2)
            elif c=='R':
                px=min(1.0,px+z/2)
    except:
        pass
    img=np.array(sct.grab(mon))
    if z<1.0:
        cw=max(1,int(w*z))
        ch=max(1,int(h*z))
        cx=int(px*max(0,w-cw))
        cy=int(py*max(0,h-ch))
        img=img[cy:cy+ch,cx:cx+cw]
    img=cv2.resize(img,(240,135),interpolation=cv2.INTER_LINEAR)
    rq=int(q*0.85) if q>10 else 10
    ok,buf=cv2.imencode('.jpg',img,[int(cv2.IMWRITE_JPEG_QUALITY),rq])
    if not ok:
        continue
    data=buf.tobytes()
    step=1400
    n=(len(data)+step-1)//step
    if n>12:
        q=max(10,q-10)
        continue
    fid=(fid+1)&0xFFFFFFFF
    for i in range(n):
        part=data[i*step:(i+1)*step]
        hdr=struct.pack('<BIBB',0xAA,fid,n,i)
        sock.sendto(hdr+part,(UDP_IP,UDP_PORT))
    time.sleep(0.02)
'@
)PY";

enum class Act : uint8_t { Connect, Install, Remove, Wifi };

enum class Ph : uint8_t {
    Menu,
    Info,
    Note,
    Intro,
    JoinDraw,
    Join,
    Ready,
    UsbDraw,
    Usb,
    Type,
    WaitYes,
    WaitPc,
    Serve,
    Live,
    Fail
};

enum class Stage : uint8_t {
    Lang,
    WaitLang,
    Win,
    WaitRun,
    Ps,
    WaitPs,
    Head,
    Ip,
    Tail,
    WaitFile,
    WaitCmd,
    Ask,
    Pause,
    Run,
    Done
};

static bool s_run = false;
static bool s_joined = false;
static bool s_usb = false;
static bool s_udpOn = false;
static Ph s_ph = Ph::Intro;
static Stage s_stage = Stage::Win;
static char s_msg[96];
static char s_ip[16];
static char s_cmd[320];
static bool s_quick = true;
static Act s_act = Act::Connect;
static uint8_t s_pick = 0;
static uint8_t s_page = 0;
static uint32_t s_waitSince = 0;
static const char* s_cur = nullptr;
static size_t s_pos = 0;
static size_t s_typed = 0;
static size_t s_total = 1;
static uint32_t s_deadline = 0;
static uint8_t s_pct = 0;

static WiFiUDP s_udp;
static WebServer* s_web = nullptr;
static char* s_py = nullptr;
static bool s_cmdWin = false;
static bool s_confirmed = false;
static uint8_t* s_buf = nullptr;
static uint16_t s_partLen[kMaxChunks];
static uint16_t s_asmFid = 0xFFFF;
static uint8_t s_asmNeed = 0;
static uint8_t s_asmGot = 0;
static uint32_t s_asmMask = 0;
static size_t s_jpgLen = 0;
static bool s_dirty = false;
static bool s_haveFrame = false;
static bool s_havePeer = false;
static IPAddress s_peer;
static uint16_t s_peerPort = 0;

static bool s_paused = false;
static bool s_follow = false;
static bool s_info = false;
static int s_q = 50;
static int s_sharp = 0;
static float s_zoom = 1.0f;
static int s_fps = 0;
static int s_frames = 0;
static uint32_t s_fpsT = 0;
static uint8_t s_low = 0;
static uint8_t s_high = 0;
static char s_osd[40];
static uint32_t s_osdUntil = 0;
static bool s_osdWas = false;

static void setMsg(const char* m) {
    strncpy(s_msg, m ? m : "", sizeof(s_msg) - 1);
    s_msg[sizeof(s_msg) - 1] = '\0';
}

static void osd(const char* t) {
    strncpy(s_osd, t ? t : "", sizeof(s_osd) - 1);
    s_osd[sizeof(s_osd) - 1] = '\0';
    s_osdUntil = millis() + 1400;
}

static bool ensureBuf() {
    if (s_buf) return true;
    s_buf = (uint8_t*)heap_caps_malloc(kBufBytes, MALLOC_CAP_8BIT);
    if (!s_buf) {
        setMsg("Not enough memory for the picture.");
        s_ph = Ph::Fail;
        return false;
    }
    Serial.printf("[DESK] took picture %u heap=%u\n",
                  (unsigned)kBufBytes, (unsigned)ESP.getFreeHeap());
    return true;
}

static void freeBuf() {
    if (s_buf) {
        heap_caps_free(s_buf);
        s_buf = nullptr;
        Serial.printf("[DESK] picture returned heap=%u\n",
                      (unsigned)ESP.getFreeHeap());
    }
    s_jpgLen = 0;
    s_dirty = false;
    s_haveFrame = false;
}

static void sendCmd(const char* cmd) {
    if (!s_udpOn || !s_havePeer || !cmd || !cmd[0]) return;
    s_udp.beginPacket(s_peer, s_peerPort);
    s_udp.write((const uint8_t*)cmd, strlen(cmd));
    s_udp.endPacket();
}

static void resetAsm(uint16_t fid, uint8_t need) {
    s_asmFid = fid;
    s_asmNeed = need;
    s_asmGot = 0;
    s_asmMask = 0;
    memset(s_partLen, 0, sizeof(s_partLen));
}

static void noteFrame() {
    s_frames++;
    uint32_t now = millis();
    if (s_fpsT == 0) s_fpsT = now;
    if (now - s_fpsT >= 1000) {
        s_fps = s_frames;
        s_frames = 0;
        s_fpsT = now;
        if (s_fps > 0 && s_fps < 8) {
            s_low++;
            s_high = 0;
        } else if (s_fps > 15) {
            s_high++;
            s_low = 0;
        } else {
            s_low = 0;
            s_high = 0;
        }
        if (s_low >= 2 && s_q > 20) {
            s_q -= 5;
            if (s_q < 20) s_q = 20;
            char q[8];
            snprintf(q, sizeof(q), "Q%d", s_q);
            sendCmd(q);
            s_low = 0;
        } else if (s_high >= 2 && s_q < 65) {
            s_q += 5;
            if (s_q > 65) s_q = 65;
            char q[8];
            snprintf(q, sizeof(q), "Q%d", s_q);
            sendCmd(q);
            s_high = 0;
        }
    }
}

// The UDP packet is already current: caller used parsePacket().
static void takeCurrent(int n) {
    if (n < 7) {
        if (n > 0) s_udp.flush();
        return;
    }
    if (!s_havePeer) {
        s_peer = s_udp.remoteIP();
        s_peerPort = s_udp.remotePort();
        s_havePeer = s_peerPort != 0;
    }
    if (s_paused || !s_buf) {
        s_udp.flush();
        return;
    }
    uint8_t pkt[7 + kChunk];
    if (n > (int)sizeof(pkt)) n = (int)sizeof(pkt);
    int got = s_udp.read(pkt, n);
    if (got < 7 || pkt[0] != 0xAA) return;
    uint16_t fid = (uint16_t)pkt[1] | ((uint16_t)pkt[2] << 8);
    uint8_t need = pkt[5];
    uint8_t idx = pkt[6];
    uint16_t clen = (uint16_t)(got - 7);
    if (need == 0 || need > kMaxChunks || idx >= need) return;
    if (clen == 0 || clen > kChunk) return;
    if (fid != s_asmFid || need != s_asmNeed) resetAsm(fid, need);
    uint32_t bit = 1ul << idx;
    if (s_asmMask & bit) return;
    if (idx + 1 < need && clen != kChunk) return;
    memcpy(s_buf + (size_t)idx * kChunk, pkt + 7, clen);
    s_partLen[idx] = clen;
    s_asmMask |= bit;
    s_asmGot++;
    if (s_asmGot != need) return;
    for (uint8_t i = 0; i + 1 < need; i++) {
        if (s_partLen[i] != kChunk) {
            resetAsm(0xFFFF, 0);
            return;
        }
    }
    size_t total = (size_t)(need - 1) * kChunk + s_partLen[need - 1];
    if (total < 4 || s_buf[0] != 0xFF || s_buf[1] != 0xD8) {
        resetAsm(0xFFFF, 0);
        return;
    }
    s_jpgLen = total;
    s_dirty = true;
    s_haveFrame = true;
    noteFrame();
    resetAsm(0xFFFF, 0);
    Display::resetDimTimer();
}

static void typeChar(char ch) {
    if (ch == '\n') {
        BadUsbHid::usbPress(BAD_KEY_RETURN);
        delay(24);
        BadUsbHid::usbRelease(BAD_KEY_RETURN);
        delay(36);
    } else {
        char b[2] = { ch, 0 };
        BadUsbHid::usbPrint(b);
        delay(6);
    }
    s_typed++;
    if (s_total) s_pct = (uint8_t)((s_typed * 100) / s_total);
    if (s_pct > 100) s_pct = 100;
}

static const size_t kPyCap = 3072;

static size_t fillScript(char* dst, size_t cap, const char* ip) {
    if (!dst || cap < 64) return 0;
    const char* use = (ip && ip[0]) ? ip : "255.255.255.255";
    const char* cut = strstr(kTail, "\n'@");
    size_t tail = cut ? (size_t)(cut - kTail) : strlen(kTail);
    int h = snprintf(dst, cap,
                     "import mss,cv2,socket,numpy as np,struct,time,os,sys,ctypes\n"
                     "UDP_IP=sys.argv[1] if len(sys.argv)>1 else '%s'\n",
                     use);
    if (h < 0 || (size_t)h >= cap || (size_t)h + tail + 2 > cap) return 0;
    memcpy(dst + h, kTail, tail);
    dst[h + tail] = '\n';
    dst[h + tail + 1] = '\0';
    return (size_t)h + tail + 1;
}

static void endHttp() {
    if (s_web) {
        s_web->stop();
        delete s_web;
        s_web = nullptr;
    }
    if (s_py) {
        free(s_py);
        s_py = nullptr;
    }
}

size_t heapHeld() {
    size_t n = 0;
    if (s_buf) n += kBufBytes;
    if (s_py) n += kPyCap;
    if (s_web) n += sizeof(WebServer);
    if (s_udpOn) n += kUdpTxBytes;
    return n;
}

static void sendPy() {
    if (!s_web) return;
    if (s_py) s_web->send(200, "text/plain", s_py);
    else s_web->send(404, "text/plain", "missing");
}

static bool startHttp() {
    endHttp();
    s_py = (char*)malloc(kPyCap);
    if (!s_py) return false;
    if (!fillScript(s_py, kPyCap, s_ip)) {
        free(s_py);
        s_py = nullptr;
        return false;
    }
    s_web = new WebServer(80);
    if (!s_web) {
        free(s_py);
        s_py = nullptr;
        return false;
    }
    s_web->on("/", HTTP_GET, sendPy);
    s_web->on("/d", HTTP_GET, sendPy);
    s_web->begin();
    Serial.printf("[DESK] http://%s/d\n", s_ip);
    return true;
}

void publishScript(const char* ip) {
    if (!Storage::available()) return;
    char* buf = (char*)malloc(kPyCap);
    if (!buf) return;
    size_t n = fillScript(buf, kPyCap, ip);
    if (n) {
        File f = SD.open("/0N3P0rK/on3desk.py", FILE_WRITE);
        if (!f) {
            Serial.println("[DESK] could not write SD script");
        } else {
            f.write((const uint8_t*)buf, n);
            f.close();
            Serial.printf("[DESK] SD /0N3P0rK/on3desk.py ip=%s\n",
                          (ip && ip[0]) ? ip : "255.255.255.255");
        }
    }
    free(buf);
}

static void buildRun() {
    if (s_act == Act::Remove) {
        snprintf(s_cmd, sizeof(s_cmd),
                 "powershell -WindowStyle Minimized -Command \"Get-CimInstance Win32_Process | Where-Object {$_.CommandLine -like '*on3desk.py*'} | ForEach-Object {Stop-Process -Id $_.ProcessId -Force}; Remove-Item on3desk.py -Force\"\n");
    } else if (s_act == Act::Install) {
        snprintf(s_cmd, sizeof(s_cmd),
                 "pip install mss opencv-python numpy; python on3desk.py %s\n", s_ip);
    } else if (s_act == Act::Wifi) {
        int n = snprintf(s_cmd, sizeof(s_cmd),
                         "start /min cmd /c \"curl.exe -o %%USERPROFILE%%\\on3desk.py http://%s/d & pip install mss opencv-python numpy & python %%USERPROFILE%%\\on3desk.py %s\"\n"
                         "exit\n",
                         s_ip, s_ip);
        if (n < 0 || n >= (int)sizeof(s_cmd)) s_cmd[0] = '\0';
    } else {
        snprintf(s_cmd, sizeof(s_cmd),
                 "powershell -WindowStyle Minimized -Command \"python on3desk.py %s\"\n", s_ip);
    }
}

static void armText(const char* text, Stage nextAfterWait) {
    (void)nextAfterWait;
    s_cur = text ? text : "";
    s_pos = 0;
}

static const char kCmdOpen[] = "cmd\n";
static const char kAsk[] = "choice /c 1 /m \"Press 1 on the pig\"\n";

static bool typingDone() {
    uint32_t now = millis();
    if (s_stage == Stage::Lang) {
        // Win+Space selects the next Windows input language. Russian -> English
        // when those two are installed.
        BadUsbHid::usbModifierCombo(true, false, false, false, ' ');
        s_stage = Stage::WaitLang;
        s_deadline = now + 700;
        return false;
    }
    if (s_stage == Stage::Win) {
        BadUsbHid::usbModifierCombo(true, false, false, false, 'r');
        s_stage = Stage::WaitRun;
        s_deadline = now + 800;
        return false;
    }
    if (s_stage == Stage::WaitLang || s_stage == Stage::WaitRun ||
        s_stage == Stage::WaitPs || s_stage == Stage::WaitFile ||
        s_stage == Stage::WaitCmd || s_stage == Stage::Pause) {
        if (now < s_deadline) return false;
        if (s_stage == Stage::WaitLang) {
            s_stage = Stage::Win;
        } else if (s_stage == Stage::WaitRun) {
            if (s_act == Act::Install) {
                s_stage = Stage::Ps;
                armText(kPs, Stage::Ps);
            } else if (s_act == Act::Wifi) {
                s_stage = Stage::Run;
                armText(kCmdOpen, Stage::Run);
            } else {
                s_stage = Stage::Run;
                armText(s_cmd, Stage::Run);
            }
        } else if (s_stage == Stage::WaitCmd) {
            s_stage = Stage::Ask;
            armText(kAsk, Stage::Ask);
        } else if (s_stage == Stage::Pause) {
            s_stage = Stage::Run;
            armText(s_cmd, Stage::Run);
        } else if (s_stage == Stage::WaitPs) {
            if (s_quick) {
                s_stage = Stage::Run;
                armText(s_cmd, Stage::Run);
            } else {
                s_stage = Stage::Head;
                armText(kHead, Stage::Head);
            }
        } else {
            s_stage = Stage::Run;
            armText(s_cmd, Stage::Run);
        }
        return false;
    }
    int budget = 24;
    while (budget-- > 0 && s_cur && s_cur[s_pos]) {
        typeChar(s_cur[s_pos++]);
        if (keyEsc()) return true;
    }
    if (s_cur && s_cur[s_pos]) return false;
    switch (s_stage) {
        case Stage::Ps:
            s_stage = Stage::WaitPs;
            s_deadline = now + 1600;
            break;
        case Stage::Head:
            s_stage = Stage::Tail;
            armText(kTail, Stage::Tail);
            break;
        case Stage::Tail:
            s_stage = Stage::WaitFile;
            s_deadline = now + 400;
            break;
        case Stage::Run:
            if (s_act == Act::Wifi && !s_cmdWin) {
                s_cmdWin = true;
                s_stage = Stage::WaitCmd;
                s_deadline = now + 1400;
                break;
            }
            s_stage = Stage::Done;
            s_pct = 100;
            return true;
        case Stage::Ask:
            s_stage = Stage::Done;
            s_pct = 40;
            return true;
        default:
            return true;
    }
    return false;
}

static void lines(M5Canvas& c, const char* title, const char* const* rows, int n) {
    c.fillSprite(0x2145);
    c.setTextDatum(top_center);
    c.setTextSize(2);
    c.setTextColor(0xFFE0);
    c.drawString(title, DISPLAY_W / 2, 2);
    c.setTextDatum(top_left);
    c.setTextSize(1);
    c.setTextColor(0xEF5D);
    for (int i = 0; i < n; i++) c.drawString(rows[i], 6, 24 + i * 11);
}

void start() {
    if (s_run) return;
    s_run = true;
    s_joined = false;
    s_usb = false;
    s_udpOn = false;
    s_ph = Ph::Menu;
    s_pick = 0;
    s_havePeer = false;
    s_peerPort = 0;
    s_paused = false;
    s_follow = false;
    s_info = false;
    s_q = 50;
    s_sharp = 0;
    s_zoom = 1.0f;
    s_fps = 0;
    s_frames = 0;
    s_fpsT = 0;
    s_pct = 0;
    s_ip[0] = '\0';
    setMsg("");
    Avatar::suspendScene();
    Serial.println("[DESK] open");
}

static void releaseSession() {
    size_t held = heapHeld();
    if (s_havePeer && s_udpOn) {
        for (int i = 0; i < 4; i++) {
            sendCmd("STOP");
            delay(25);
        }
    }
    // stop() frees the 1460-byte tx buffer even if begin() failed halfway.
    s_udp.stop();
    s_udpOn = false;
    endHttp();
    s_havePeer = false;
    s_haveFrame = false;
    if (s_usb) {
        BadUsbHid::usbReleaseAll();
        BadUsbHid::usbEnd();
        s_usb = false;
    }
    freeBuf();
    if (s_joined) {
        Net::leaveHome();
        s_joined = false;
    }
    s_cmdWin = false;
    s_confirmed = false;
    if (held) {
        Serial.printf("[DESK] returned %u heap=%u\n",
                      (unsigned)held, (unsigned)ESP.getFreeHeap());
    }
}

static void backMenu() {
    releaseSession();
    s_ph = Ph::Menu;
    setMsg("");
    SFX::play(SFX::BACK_NAV);
}

void stop() {
    if (!s_run && !s_buf && !s_udpOn && !s_usb && !s_joined && !s_web && !s_py) return;
    releaseSession();
    s_run = false;
    s_ph = Ph::Intro;
    Avatar::resumeScene();
    Serial.println("[DESK] stop");
}

bool isRunning() { return s_run; }

static void pollNet() {
    if (!s_udpOn) return;
    if (s_ph != Ph::WaitPc && s_ph != Ph::Live && s_ph != Ph::Serve) return;
    if (!ensureBuf()) return;
    for (int i = 0; i < 10; i++) {
        int n = s_udp.parsePacket();
        if (n <= 0) break;
        takeCurrent(n);
    }
}

// FN does not change the letter. Shift does. Accept both, with FN held or not.
static bool keyZoomIn(char c) { return c == '=' || c == '+'; }
static bool keyZoomOut(char c) { return c == '-' || c == '_'; }

static void liveKeys(bool edge) {
    if (!edge) return;
    Keyboard_Class::KeysState st = M5Cardputer.Keyboard.keysState();
    char c = 0;
    for (char k : st.word) {
        c = k;
        break;
    }
    if (keyZoomIn(c)) {
        s_zoom -= 0.08f;
        if (s_zoom < 0.08f) s_zoom = 0.08f;
        sendCmd("Z+");
        osd("ZOOM IN");
    } else if (keyZoomOut(c)) {
        s_zoom += 0.08f;
        if (s_zoom > 1.0f) s_zoom = 1.0f;
        sendCmd("Z-");
        osd("ZOOM OUT");
    } else if (c == ' ' || st.space) {
        s_zoom = 1.0f;
        sendCmd("Z0");
        osd("FULL SCREEN");
    } else if (c == ';' || c == ':') {
        sendCmd("U");
    } else if (c == '.' || c == '>') {
        sendCmd("D");
    } else if (c == ',' || c == '<') {
        sendCmd("L");
    } else if (c == '/' || c == '?') {
        sendCmd("R");
    } else if (c == 'p' || c == 'P') {
        s_paused = !s_paused;
        osd(s_paused ? "PAUSED" : "LIVE");
    }
}

static bool s_waitAbort = false;
static bool s_waitLeave = false;
static bool s_bsWas = false;
static bool s_escWas = false;

static void syncScene() {
    if (App::windowHidden()) {
        if (Avatar::isSceneSuspended()) Avatar::resumeScene();
    } else if (!Avatar::isSceneSuspended()) {
        Avatar::suspendScene();
    }
}

static void toggleHide() {
    App::setWindowHidden(!App::windowHidden());
    syncScene();
    SFX::play(SFX::MENU_CLICK);
    Display::showToast(App::windowHidden() ? "MIN" : "WIN", 500);
}

// Called from inside the Wi-Fi and USB waits, which otherwise block the loop.
static bool pumpWait() {
    M5Cardputer.update();
    bool bs = keyMin();
    if (bs && !s_bsWas) toggleHide();
    s_bsWas = bs;
    bool esc = keyEsc();
    if (esc && !s_escWas) {
        s_waitAbort = true;
        s_waitLeave = App::windowHidden();
    }
    s_escWas = esc;
    Display::update();
    SFX::update();
    yield();
    return s_waitAbort;
}

static void armWaitKeys() {
    s_waitAbort = false;
    s_waitLeave = false;
    s_bsWas = keyMin();
    s_escWas = keyEsc();
}

static void abortWait() {
    BadUsbHid::usbReleaseAll();
    BadUsbHid::usbEnd();
    s_usb = false;
    Net::leaveHome();
    s_joined = false;
    if (s_waitLeave) stop();
    else backMenu();
}

void update() {
    if (!s_run) return;
    syncScene();

    // isChange() flips false after the first read in this frame.
    const bool changed = M5Cardputer.Keyboard.isChange();
    const bool edge = changed && M5Cardputer.Keyboard.isPressed();
    Keyboard_Class::KeysState st = M5Cardputer.Keyboard.keysState();
    if (edge && keyMin()) toggleHide();
    const bool hidden = App::windowHidden();
    // Backspace only hides the window. Keys are ignored while it is hidden,
    // except ESC, which leaves, matching the other tools.
    const bool keys = edge && !keyMin() && !hidden;
    const bool esc = keys && keyEsc();
    const bool fnEsc = esc && st.fn;
    const bool enter = keys && st.enter;
    char c = 0;
    if (keys) {
        for (char k : st.word) {
            c = k;
            break;
        }
    }
    const bool up = c == ';' || c == ':';
    const bool down = c == '.' || c == '>';

    // Picture stays up on plain ESC. FN+ESC leaves from every screen.
    if (fnEsc) {
        if (s_ph == Ph::Menu) SFX::play(SFX::BACK_NAV);
        stop();
        return;
    }

    if (s_ph == Ph::Menu) {
        if (esc) {
            SFX::play(SFX::BACK_NAV);
            stop();
            return;
        }
        if (enter) {
            SFX::play(SFX::CONFIRM);
            if (s_pick == 4) {
                s_page = 0;
                s_ph = Ph::Info;
                return;
            }
            s_act = (Act)s_pick;
            s_quick = (s_act == Act::Connect);
            s_ph = (s_act == Act::Remove) ? Ph::UsbDraw : Ph::JoinDraw;
            return;
        }
        if (up && s_pick > 0) {
            s_pick--;
            SFX::play(SFX::MENU_CLICK);
        } else if (down && s_pick < 4) {
            s_pick++;
            SFX::play(SFX::MENU_CLICK);
        }
        return;
    }
    if (s_ph == Ph::Info) {
        if (esc) {
            SFX::play(SFX::BACK_NAV);
            s_ph = Ph::Menu;
            return;
        }
        if (up && s_page > 0) {
            s_page--;
            SFX::play(SFX::MENU_CLICK);
        } else if (down && s_page < 4) {
            s_page++;
            SFX::play(SFX::MENU_CLICK);
        }
        return;
    }
    if (s_ph == Ph::Note) {
        if (s_usb) {
            BadUsbHid::usbReleaseAll();
            BadUsbHid::usbEnd();
            s_usb = false;
        }
        if (esc) backMenu();
        else if (enter) s_ph = Ph::Menu;
        return;
    }
    if (s_ph == Ph::Intro) {
        if (esc) backMenu();
        else if (enter) s_ph = Ph::JoinDraw;
        return;
    }
    if (s_ph == Ph::JoinDraw) {
        s_ph = Ph::Join;
        return;
    }
    if (s_ph == Ph::Join) {
        if (!Net::hasStaCreds()) {
            setMsg("Save the home Wi-Fi in CONNECT, WIFI first.");
            s_ph = Ph::Fail;
            return;
        }
        armWaitKeys();
        s_joined = Net::joinHome(22000, pumpWait);
        if (s_waitAbort) {
            abortWait();
            return;
        }
        if (!s_joined) {
            setMsg("Home Wi-Fi did not connect.");
            s_ph = Ph::Fail;
            return;
        }
        IPAddress ip = WiFi.localIP();
        snprintf(s_ip, sizeof(s_ip), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
        publishScript(s_ip);
        if (!s_udpOn) {
            s_udpOn = s_udp.begin(kPort);
            if (!s_udpOn) {
                // begin() can malloc its tx buffer and then fail the socket.
                s_udp.stop();
                setMsg("UDP port did not open.");
                s_ph = Ph::Fail;
                return;
            }
        }
        if (s_act == Act::Wifi) {
            if (!startHttp()) {
                setMsg("Could not share the script over Wi-Fi.");
                s_ph = Ph::Fail;
                return;
            }
            s_ph = Ph::UsbDraw;
            return;
        }
        s_ph = Ph::UsbDraw;
        return;
    }
    if (s_ph == Ph::Serve) {
        if (esc) {
            backMenu();
            return;
        }
        if (s_web) s_web->handleClient();
        pollNet();
        if (s_ph != Ph::Serve) return;
        if (s_haveFrame) {
            endHttp();
            s_ph = Ph::Live;
        }
        return;
    }
    if (s_ph == Ph::Ready) {
        s_ph = Ph::UsbDraw;
        return;
    }
    if (s_ph == Ph::UsbDraw) {
        s_ph = Ph::Usb;
        return;
    }
    if (s_ph == Ph::Usb) {
        if (UsbSdMode::isRunning()) UsbSdMode::stop();
        uint32_t usbWait = (s_act == Act::Wifi) ? 8000 : 12000;
        armWaitKeys();
        if (!BadUsbHid::usbBegin() || !BadUsbHid::usbWaitHost(usbWait, pumpWait)) {
            if (s_waitAbort) {
                abortWait();
                return;
            }
            if (s_act == Act::Wifi) {
                BadUsbHid::usbEnd();
                s_ph = Ph::Serve;
                return;
            }
            setMsg("PC did not see the USB keyboard.");
            s_ph = Ph::Fail;
            return;
        }
        s_usb = true;
        buildRun();
        if (s_act == Act::Wifi && s_cmd[0] == '\0') {
            BadUsbHid::usbReleaseAll();
            BadUsbHid::usbEnd();
            s_usb = false;
            s_ph = Ph::Serve;
            return;
        }
        s_cmdWin = false;
        s_confirmed = false;
        s_stage = Stage::Lang;
        s_cur = nullptr;
        s_pos = 0;
        s_typed = 0;
        s_total = strlen(s_cmd);
        if (s_act == Act::Install) s_total += strlen(kPs) + strlen(kHead) + strlen(kTail);
        if (!s_total) s_total = 1;
        s_pct = 0;
        s_ph = Ph::Type;
        return;
    }
    if (s_ph == Ph::Type) {
        if (esc) {
            backMenu();
            return;
        }
        if (typingDone() && s_stage == Stage::Done) {
            BadUsbHid::usbReleaseAll();
            if (s_act == Act::Wifi) {
                for (int i = 0; i < 6; i++) {
                    if (s_web) s_web->handleClient();
                    delay(15);
                }
            }
            if (s_act == Act::Remove) {
                s_ph = Ph::Note;
            } else if (s_act == Act::Wifi && !s_confirmed) {
                s_ph = Ph::WaitYes;
            } else {
                s_waitSince = millis();
                s_ph = Ph::WaitPc;
            }
        }
        return;
    }
    if (s_ph == Ph::WaitYes) {
        if (esc) {
            backMenu();
            return;
        }
        if (s_web) s_web->handleClient();
        if (edge) {
            char c = 0;
            for (char k : st.word) {
                c = k;
                break;
            }
            if (c == '1' || c == 'y' || c == 'Y') {
                typeChar('1');
                s_confirmed = true;
                s_stage = Stage::Pause;
                s_deadline = millis() + 800;
                s_cur = nullptr;
                s_pos = 0;
                s_ph = Ph::Type;
            }
        }
        return;
    }
    if (s_ph == Ph::WaitPc) {
        if (esc) {
            backMenu();
            return;
        }
        if (s_web) s_web->handleClient();
        pollNet();
        if (s_haveFrame) {
            endHttp();
            s_ph = Ph::Live;
        }
        return;
    }
    if (s_ph == Ph::Live) {
        if (keys) liveKeys(true);
        if (!s_run) return;
        pollNet();
        return;
    }
    if (esc) backMenu();
}

void draw(M5Canvas& canvas) {
    if (s_ph == Ph::Menu) {
        static const char* items[] = { "CONNECT", "INSTALL", "DELETE", "WIFI", "INFO" };
        uiListBackground(canvas);
        canvas.setTextDatum(top_left);
        canvas.setTextSize(2);
        canvas.setTextColor(UiStyle::TITLE);
        canvas.drawString("DESK", 8, 0);
        canvas.drawLine(8, 18, DISPLAY_W - 8, 18, UiStyle::TITLE);
        canvas.setTextSize(1);
        const int y0 = 24;
        const int lh = 16;
        for (int i = 0; i < 5; i++) {
            int y = y0 + i * lh;
            bool sel = i == (int)s_pick;
            uiListRow(canvas, y, lh - 1, sel);
            canvas.setTextColor(sel ? UiStyle::BG : UiStyle::TEXT);
            canvas.drawString(items[i], 12, y + 3);
        }
        return;
    }
    if (s_ph == Ph::Info) {
        static const char* titles[] = { "MENU", "PICTURE", "PAN", "START", "FILE" };
        static const char* p0[] = { "^/v    move", "ENT    open", "ESC    back one step", "BKSP   hide window", "FN+ESC leave DESK" };
        static const char* p1[] = { "BKSP   hide this window", "FN+ESC leave the picture", "= +    zoom in", "-      zoom out", "SPACE  full screen", "P      pause" };
        static const char* p2[] = { "^/v    up / down", "< >    left / right", "Keys are ; . , /", "FN held works too" };
        static const char* p3[] = { "Win+Space sets English", "CONNECT runs the script", "INSTALL writes it", "WIFI opens CMD, press 1" };
        static const char* p4[] = { "SD /0N3P0rK/on3desk.py", "Same Wi-Fi as the pig", "pip install mss", "opencv-python numpy" };
        static const char* const* pages[] = { p0, p1, p2, p3, p4 };
        static const uint8_t counts[] = { 5, 6, 4, 4, 4 };
        uint8_t page = s_page;
        if (page > 4) page = 4;
        uiListBackground(canvas);
        canvas.setTextDatum(top_left);
        canvas.setTextSize(2);
        canvas.setTextColor(UiStyle::TITLE);
        canvas.drawString("INFO", 8, 0);
        canvas.setTextSize(1);
        canvas.setTextDatum(top_right);
        char pg[8];
        snprintf(pg, sizeof(pg), "%u/5", (unsigned)(page + 1));
        canvas.drawString(pg, DISPLAY_W - 8, 4);
        canvas.setTextDatum(top_left);
        canvas.drawLine(8, 18, DISPLAY_W - 8, 18, UiStyle::TITLE);
        canvas.setTextColor(UiStyle::GOLD);
        canvas.drawString(titles[page], 8, 22);
        canvas.setTextColor(UiStyle::TEXT);
        uint8_t n = counts[page];
        for (uint8_t i = 0; i < n; i++) canvas.drawString(pages[page][i], 8, 38 + i * 12);
        canvas.setTextColor(UiStyle::DIM);
        if (page > 0) canvas.drawString("^", DISPLAY_W - 12, 22);
        if (page < 4) canvas.drawString("v", DISPLAY_W - 12, 38 + (n - 1) * 12);
        return;
    }
    if (s_ph == Ph::Serve) {
        char url[48];
        char run[48];
        snprintf(url, sizeof(url), "http://%s/d", s_ip);
        snprintf(run, sizeof(run), "python on3desk.py %s", s_ip);
        canvas.fillSprite(0x2145);
        canvas.setTextDatum(top_left);
        canvas.setTextSize(1);
        canvas.setTextColor(0xFFE0);
        canvas.drawString("No USB. Type this in CMD.", 2, 0);
        canvas.setTextColor(0x07E0);
        canvas.drawString("curl.exe -o on3desk.py ^", 2, 14);
        canvas.drawString(url, 2, 26);
        canvas.setTextColor(0xEF5D);
        canvas.drawString("pip install mss opencv-python numpy", 2, 40);
        canvas.drawString(run, 2, 54);
        canvas.drawString("The ^ is a caret, then Enter.", 2, 68);
        canvas.drawString("ESC back.   BKSP hide.", 2, 80);
        canvas.drawString("Same Wi-Fi. Picture follows.", 2, 92);
        return;
    }
    if (s_ph == Ph::Note) {
        static const char* rows[] = {
            "on3desk.py removed.",
            "The PowerShell window",
            "closes itself.",
            "",
            "ENT    menu",
            "ESC    back",
            "BKSP   hide"
        };
        lines(canvas, "DESK", rows, 7);
        return;
    }
    if (s_ph == Ph::Intro) {
        static const char* rows[] = {
            "Your PC, same Wi-Fi as the pig.",
            "Plug USB into that PC.",
            "Win+Space sets English.",
            "Python 3 must be on PATH.",
            "Leave the PC keyboard alone.",
            "ENT open     ESC back",
            "BKSP hides this window"
        };
        lines(canvas, "DESK", rows, 7);
        return;
    }
    if (s_ph == Ph::JoinDraw || s_ph == Ph::Join) {
        static const char* rows[] = {
            "Joining the saved home Wi-Fi.",
            "This can take a few seconds.",
            "",
            "ESC    back to the menu",
            "BKSP   hide this window"
        };
        lines(canvas, "DESK", rows, 5);
        return;
    }
    if (s_ph == Ph::Ready) {
        char ipLine[40];
        snprintf(ipLine, sizeof(ipLine), "Pig  %s", s_ip);
        const char* rows[] = {
            ipLine,
            "PC must reach this address.",
            "Do not touch the PC keyboard",
            "while the script is typed.",
            "ESC back.   BKSP hide."
        };
        lines(canvas, "DESK", rows, 5);
        return;
    }
    if (s_ph == Ph::UsbDraw || s_ph == Ph::Usb) {
        static const char* rows[] = {
            "Win+Space switches to English.",
            "Then the command is typed.",
            "Hands off the PC keyboard.",
            "ESC    back",
            "BKSP   hide"
        };
        static const char* wifi[] = {
            "Win+Space sets English.",
            "Then a CMD window opens.",
            "Hands off the PC keyboard.",
            "ESC    back",
            "BKSP   hide"
        };
        lines(canvas, "DESK", s_act == Act::Wifi ? wifi : rows, 5);
        return;
    }
    if (s_ph == Ph::Type) {
        canvas.fillSprite(0x2145);
        canvas.setTextDatum(top_center);
        canvas.setTextSize(2);
        canvas.setTextColor(0xFFE0);
        canvas.drawString("TYPING", DISPLAY_W / 2, 8);
        canvas.setTextSize(1);
        canvas.setTextColor(0xEF5D);
        canvas.drawString(s_act == Act::Wifi ? "English, then CMD. Hands off." :
                          (s_quick ? "ENG, then start on3desk.py" : "ENG, then write on3desk.py"),
                          DISPLAY_W / 2, 36);
        canvas.drawRoundRect(20, 58, 200, 16, 3, 0xEF5D);
        int w = (int)s_pct * 196 / 100;
        if (w > 0) canvas.fillRect(22, 60, w, 12, 0x07E0);
        char pct[8];
        snprintf(pct, sizeof(pct), "%u%%", (unsigned)s_pct);
        canvas.drawString(pct, DISPLAY_W / 2, 80);
        canvas.drawString("ESC back.   BKSP hide.", DISPLAY_W / 2, 94);
        return;
    }
    if (s_ph == Ph::WaitYes) {
        static const char* rows[] = {
            "CMD is open. Layout is English.",
            "Press 1 on the pig to start.",
            "",
            "1      start the install",
            "ESC    back",
            "BKSP   hide"
        };
        lines(canvas, "DESK", rows, 6);
        return;
    }
    if (s_ph == Ph::WaitPc) {
        static const char* quick[] = {
            "Starting the script already",
            "on this PC.",
            "The picture starts itself.",
            "",
            "ESC    back",
            "BKSP   hide"
        };
        static const char* full[] = {
            "Installing, then the picture",
            "starts by itself.",
            "First pip can take a minute.",
            "",
            "ESC    back",
            "BKSP   hide"
        };
        static const char* wifi[] = {
            "Installing in a small window.",
            "First pip can take a minute.",
            "The picture starts after that.",
            "",
            "ESC    back",
            "BKSP   hide"
        };
        const char* const* rows = s_quick ? quick : full;
        if (s_act == Act::Wifi) rows = wifi;
        lines(canvas, "DESK", rows, 6);
        return;
    }
    if (s_ph == Ph::Fail) {
        canvas.fillSprite(0x2145);
        canvas.setTextDatum(top_center);
        canvas.setTextSize(2);
        canvas.setTextColor(0xF800);
        canvas.drawString("DESK", DISPLAY_W / 2, 6);
        canvas.setTextSize(1);
        canvas.setTextColor(0xEF5D);
        canvas.setTextDatum(top_left);
        // Wrap the message on spaces into two rows of ~38 chars.
        char a[48];
        char b[48];
        a[0] = b[0] = '\0';
        const char* p = s_msg;
        size_t n = strlen(p);
        if (n <= 38) {
            strncpy(a, p, sizeof(a) - 1);
        } else {
            size_t cut = 38;
            while (cut > 10 && p[cut] != ' ') cut--;
            if (p[cut] != ' ') cut = 38;
            size_t na = cut < sizeof(a) ? cut : sizeof(a) - 1;
            memcpy(a, p, na);
            a[na] = '\0';
            strncpy(b, p + cut + (p[cut] == ' ' ? 1 : 0), sizeof(b) - 1);
        }
        canvas.setTextDatum(top_center);
        canvas.drawString(a, DISPLAY_W / 2, 40);
        if (b[0]) canvas.drawString(b, DISPLAY_W / 2, 54);
        canvas.drawString("ESC back.   BKSP hide.", DISPLAY_W / 2, 88);
        return;
    }
    canvas.fillSprite(0x2145);
}

bool paintDirect() {
    if (!s_run || s_ph != Ph::Live || App::windowHidden()) return false;
    if (!s_haveFrame || !s_buf || s_jpgLen < 4) {
        M5.Display.fillScreen(BLACK);
        return true;
    }
    if (s_dirty) {
        M5.Display.drawJpg(s_buf, s_jpgLen, 0, 0, DISPLAY_W, DISPLAY_H);
        s_dirty = false;
    }
    return true;
}

void getStatusLine(char* out, size_t n) {
    if (!out || !n) return;
    if (s_ph == Ph::Menu) snprintf(out, n, "^/v  ENT  ESC BACK  BKSP HIDE");
    else if (s_ph == Ph::Info) snprintf(out, n, "^/v PAGE  ESC BACK  BKSP HIDE");
    else if (s_ph == Ph::Live) snprintf(out, n, "BKSP HIDE  FN+ESC LEAVE");
    else if (s_ph == Ph::WaitYes) snprintf(out, n, "1 START  ESC BACK  BKSP HIDE");
    else if (s_ph == Ph::Type) snprintf(out, n, "TYPE %u%%  ESC BACK  BKSP HIDE", (unsigned)s_pct);
    else if (s_ph == Ph::WaitPc) snprintf(out, n, "WAITING  ESC BACK  BKSP HIDE");
    else if (s_ph == Ph::Fail) snprintf(out, n, "ESC BACK  BKSP HIDE");
    else snprintf(out, n, "ESC BACK  BKSP HIDE");
}

}  // namespace DeskMode
