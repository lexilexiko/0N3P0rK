// mesh_text.cpp — channel chat, direct messages, UTF-8, and the keymap.
// Add a language in mapLay. The modem and the screen do not own this table.
// The modem appends messages. The screen reads them.

#include "mesh_priv.h"

#include <string.h>

Msg* s_chat = nullptr;
uint8_t s_chatHead = 0, s_chatN = 0, s_chatNew = 0;
Msg* s_dm = nullptr;
uint8_t s_dmHead = 0, s_dmN = 0, s_dmNew = 0;
uint32_t s_dmNode = 0;

void chatAdd(const Msg& m){
    s_chat[s_chatHead]=m; s_chatHead=(s_chatHead+1)%MAX_CHAT;
    if(s_chatN<MAX_CHAT) s_chatN++; s_chatNew++;
}
// get i-th message oldest-first from ring (0=oldest)
Msg* chatGet(uint8_t i){
    if(i>=s_chatN) return nullptr;
    uint8_t idx=(s_chatHead+MAX_CHAT-s_chatN+i)%MAX_CHAT;
    return &s_chat[idx];
}
void dmLoad(uint32_t node){
    meshFlushDm();
    s_dmNode=node; s_dmN=0; s_dmHead=0; s_dmNew=0;
    meshReloadDm(node);
}
void dmAdd(const Msg& m){
    s_dm[s_dmHead]=m; s_dmHead=(s_dmHead+1)%MAX_DM;
    if(s_dmN<MAX_DM) s_dmN++; s_dmNew++;
}
Msg* dmGet(uint8_t i){
    if(i>=s_dmN) return nullptr;
    uint8_t idx=(s_dmHead+MAX_DM-s_dmN+i)%MAX_DM;
    return &s_dm[idx];
}

int utf8Next(const char* s, uint32_t& cp) {
    if (!s || !s[0]) { cp = 0; return 0; }
    const uint8_t a = (uint8_t)s[0];
    if (a < 0x80) { cp = a; return 1; }
    if ((a & 0xE0) == 0xC0 && (uint8_t)s[1] >= 0x80) {
        cp = ((uint32_t)(a & 0x1F) << 6) | ((uint8_t)s[1] & 0x3F);
        return 2;
    }
    if ((a & 0xF0) == 0xE0 && (uint8_t)s[1] >= 0x80 && (uint8_t)s[2] >= 0x80) {
        cp = ((uint32_t)(a & 0x0F) << 12) |
             ((uint32_t)((uint8_t)s[1] & 0x3F) << 6) |
             ((uint8_t)s[2] & 0x3F);
        return 3;
    }
    cp = '?';
    return 1;
}

int utf8Fit(const char* s, int cap) {
    int i = 0;
    if (!s || cap < 1) return 0;
    while (s[i]) {
        uint32_t cp;
        int n = utf8Next(s + i, cp);
        if (n <= 0 || i + n > cap) break;
        i += n;
    }
    return i;
}

int utf8Cols(const char* s, int nbytes) {
    int cols = 0;
    int i = 0;
    if (!s) return 0;
    while (s[i] && i < nbytes) {
        uint32_t cp;
        int n = utf8Next(s + i, cp);
        if (n <= 0) break;
        i += n;
        cols++;
    }
    return cols;
}

// Cardputer Esc is the ` key, so yo and ghe-upturn sit on the backslash key.
// Each pair is one physical key: small letter, then Shift. K is Л, L is Д.
struct LayKey { char ascii; uint16_t ru; uint16_t ua; };
const LayKey LAY[] = {
    {'q',0x0439,0}, {'Q',0x0419,0}, // й Й
    {'w',0x0446,0}, {'W',0x0426,0}, // ц Ц
    {'e',0x0443,0}, {'E',0x0423,0}, // у У
    {'r',0x043A,0}, {'R',0x041A,0}, // к К
    {'t',0x0435,0}, {'T',0x0415,0}, // е Е
    {'y',0x043D,0}, {'Y',0x041D,0}, // н Н
    {'u',0x0433,0}, {'U',0x0413,0}, // г Г
    {'i',0x0448,0}, {'I',0x0428,0}, // ш Ш
    {'o',0x0449,0}, {'O',0x0429,0}, // щ Щ
    {'p',0x0437,0}, {'P',0x0417,0}, // з З
    {'[',0x0445,0}, {'{',0x0425,0}, // х Х
    {']',0x044A,0x0457}, {'}',0x042A,0x0407}, // ъ Ъ, UA ї Ї
    {'\\',0x0451,0x0491}, {'|',0x0401,0x0490}, // ё Ё, UA ґ Ґ
    {'a',0x0444,0}, {'A',0x0424,0}, // ф Ф
    {'s',0x044B,0x0456}, {'S',0x042B,0x0406}, // ы Ы, UA і І
    {'d',0x0432,0}, {'D',0x0412,0}, // в В
    {'f',0x0430,0}, {'F',0x0410,0}, // а А
    {'g',0x043F,0}, {'G',0x041F,0}, // п П
    {'h',0x0440,0}, {'H',0x0420,0}, // р Р
    {'j',0x043E,0}, {'J',0x041E,0}, // о О
    {'k',0x043B,0}, {'K',0x041B,0}, // л Л
    {'l',0x0434,0}, {'L',0x0414,0}, // д Д
    {';',0x0436,0}, {':',0x0416,0}, // ж Ж
    {'\'',0x044D,0x0454}, {'"',0x042D,0x0404},
    {'z',0x044F,0}, {'Z',0x042F,0}, {'x',0x0447,0}, {'X',0x0427,0},
    {'c',0x0441,0}, {'C',0x0421,0}, {'v',0x043C,0}, {'V',0x041C,0},
    {'b',0x0438,0}, {'B',0x0418,0}, {'n',0x0442,0}, {'N',0x0422,0},
    {'m',0x044C,0}, {'M',0x042C,0}, {',',0x0431,0}, {'<',0x0411,0},
    {'.',0x044E,0}, {'>',0x042E,0}, {'/',0x002E,0}, {'?',0x002C,0},
};

uint32_t mapLay(char c, uint8_t lang) {
    if (!lang) return (uint8_t)c;
    for (unsigned i = 0; i < sizeof(LAY) / sizeof(LAY[0]); i++) {
        if (LAY[i].ascii != c) continue;
        if (lang == 2 && LAY[i].ua) return LAY[i].ua;
        return LAY[i].ru;
    }
    return (uint8_t)c;
}

namespace MeshPig {

uint8_t   msgCount()        { return s_chatN; }
MeshMsg*  msgAt(uint8_t i)  {
    Msg* m=chatGet(i); if(!m) return nullptr;
    // convert Msg → MeshMsg on-the-fly using static buffer
    static MeshMsg mm;
    mm.from=m->from; mm.time=m->timeMs; mm.broadcast=(m->to==BROADCAST_ID);
    strncpy(mm.text,m->text,47); mm.text[47]=0;
    strncpy(mm.fromName,m->fromName,7); mm.fromName[7]=0;
    return &mm;
}
uint8_t   newMsgCount()     { return s_chatNew; }
void      clearNew()        { s_chatNew=0; s_dmNew=0; }

} // namespace MeshPig
