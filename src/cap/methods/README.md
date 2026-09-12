# capture methods / методики захвата

Эта папка — **plug-and-play** для методик захвата PMKID/handshake.

Чтобы добавить новую методику — **просто создай тут один `.cpp` файл**.
Ничего больше править не нужно: ни `sniffer.cpp`, ни `method_ctx.h`,
ни `method_registry.cpp`, ни настройки, ни `platformio.ini`.

---

## Минимальный шаблон

```cpp
#include "method_ctx.h"
// при необходимости:
// #include "../hc22000.h"
// #include "../../core/wsl_bypasser.h"
// #include <Arduino.h>

namespace Cap {
namespace Methods {

// точка входа — вызывается каждый kick-tick
void mymethod(const Ctx& ctx) {
    // ctx.beacons, ctx.beaconCount, ctx.channel, ...
    // ctx.sendRawMgmt(0xC0, bssid, dest);   // deauth
    // ctx.sendRawMgmt(0xA0, bssid, dest);   // disassoc
    // ctx.isOwnAp(bssid), ctx.skipPin(bssid), ctx.bcast
    // ctx.kickBurst, ctx.deauthReason, ctx.bidirKick
    // ctx.eapolTx, ctx.csaHerd, ctx.authFlood, ctx.pmkidProbe
    // *ctx.framesDeauth++;                   // счётчик отправленных фреймов
}

// опционально: PMKID-зонд (вызывается параллельно с kick'ом, если ctx.pmkidProbe)
// сигнатура: void probe(const Ctx& ctx)
void myprobe(const Ctx& ctx) {
    // например WSLBypasser::sendAuthentication(bssid);
    //        WSLBypasser::sendAssociationRequest(bssid, ssid);
}

// опционально: сброс внутреннего состояния при старте/стопе сессии
// сигнатура: void reset()
void myreset() {
    // обнулить свои static-переменные
}

// ОБЯЗАТЕЛЬНО: одна строка регистрации в самом конце файла.
// Любой из PROBE/RESET может быть nullptr, если не нужен.
CAP_METHOD_REGISTER("MYMETHOD", mymethod, myprobe, myreset)
// или без probe/reset:
// CAP_METHOD_REGISTER("MYMETHOD", mymethod, nullptr, nullptr)

} // namespace Methods
} // namespace Cap
```

---

## Что обязательно

1. **Файл лежит в этой папке** (`src/cap/methods/`) — PlatformIO сам его подхватит.
2. **`#include "method_ctx.h"`** — даёт `Ctx`, `BeaconSlot`, макрос регистрации.
3. **Тело функции kick** — принимает `const Ctx& ctx`. Сигнатура фиксированная.
4. **`CAP_METHOD_REGISTER("NAME", kick, probe, reset)`** в namespace `Cap::Methods`, в самом конце файла, после тела функций. Без `;` в конце вызова — макрос сам её ставит.
5. **Имя (`NAME`)** — 4..7 символов, ASCII. Будет показано в UI как есть, поэтому лучше короткое и читаемое (`OURS`, `PAN`, `KARMA`, `PMKID`...).

## Что опционально

- **`probe`** — отдельная функция для PMKID-зонда (open auth + assoc req).
  Вызывается параллельно с `kick`, если `ctx.pmkidProbe == true`.
  Если не нужна — `nullptr`.
- **`reset`** — сброс своего внутреннего состояния (счётчики, индексы).
  Вызывается при старте/стопе сессии захвата.
  Если не нужна — `nullptr`.

## Что нельзя

- ❌ Менять сигнатуру `kick(const Ctx&)` — диспетчер не найдёт твою функцию.
- ❌ Использовать то же `NAME` в двух файлах — таблица оставит последний
  (по линк-ордеру), но для предсказуемости лучше давать уникальные имена.
- ❌ Дёргать static'и `sniffer.cpp` напрямую — только через `ctx`.
  Если нужно что-то, чего нет в `Ctx` — добавь поле в `Ctx` (см. `method_ctx.h`).

## Что будет после добавления

- Методика появится в меню **Settings → Capture method** автоматически.
- Будет участвовать в **AUTO-ротации** (если в настройках выбран AUTO).
- Будет доступна через `Methods::findByName("MYMETHOD")`.
- Счётчик отправленных фреймов пишется через `*ctx.framesDeauth++`.

## Доступные хелперы (через ctx)

| Поле/метод | Что делает |
|---|---|
| `ctx.beacons[i]` | массив `BeaconSlot` (bssid, ch, rssi, ssid, clients[], pmfCapable) |
| `ctx.beaconCount` | сколько AP в массиве сейчас |
| `ctx.channel` | текущий радиоканал |
| `ctx.bcast` | `FF:FF:FF:FF:FF:FF` для широковещательных фреймов |
| `ctx.kickBurst` | сколько раз повторить deauth за тик (0 = 1) |
| `ctx.deauthReason` | reason code в deauth/disassoc |
| `ctx.bidirKick` | true → слать `WSLBypasser::sendBidirectionalKick` (AP→STA и STA→AP) |
| `ctx.eapolTx` | true → дополнительно `EAPOL-Start`/`Logoff` |
| `ctx.csaHerd` | true → пробовать CSA-beacon для PMF-AP |
| `ctx.authFlood` | true → если нечего кикать, auth flood fallback |
| `ctx.pmkidProbe` | true → диспетчер зовёт probe-функцию параллельно |
| `ctx.minRssi` | порог по RSSI — не трогать AP слабее |
| `ctx.isOwnAp(bssid)` | true → это наша AP, пропустить |
| `ctx.skipPin(bssid)` | true → юзер пометил «не трогать» |
| `ctx.sendRawMgmt(fc0, bssid, dest)` | отправить mgmt-фрейм (0xC0=deauth, 0xA0=disassoc) |
| `ctx.framesDeauth` | указатель на счётчик — `(*ctx.framesDeauth)++` |

Внешние хелперы (если нужно) — уже подключены в `method_pan.cpp`/`method_pmkid.cpp`
как пример: `WSLBypasser::sendAuthentication`, `WSLBypasser::sendAssociationRequest`,
`Hc22000::hasPair`, `yield()`.

---

## Пример: методика «только PMKID-зонд, без deauth»

```cpp
#include "method_ctx.h"
#include "../../core/wsl_bypasser.h"

namespace Cap {
namespace Methods {

static uint32_t s_lastMs = 0;
static uint8_t  s_idx = 0;

void probe_only(const Ctx& ctx) {
    uint32_t now = millis();
    if (now - s_lastMs < 2000) return;
    uint8_t n = ctx.beaconCount;
    if (!n) return;
    for (uint8_t k = 0; k < n; k++) {
        s_idx = (s_idx + 1) % n;
        const BeaconSlot& b = ctx.beacons[s_idx];
        if (b.channel != ctx.channel) continue;
        if (ctx.isOwnAp(b.bssid)) continue;
        if (b.rssi < ctx.minRssi) continue;
        if (!b.ssid[0]) continue;
        WSLBypasser::sendAuthentication(b.bssid);
        WSLBypasser::sendAssociationRequest(b.bssid, b.ssid);
        s_lastMs = now;
        (*ctx.framesDeauth)++;
        return;
    }
}

void reset_probe_only() {
    s_lastMs = 0;
    s_idx = 0;
}

// kick = nullptr — диспетчер просто не будет нас кикать.
CAP_METHOD_REGISTER("PMKIDONLY", nullptr, probe_only, reset_probe_only)

} // namespace Methods
} // namespace Cap
```

Сохранил как `method_pmkidonly.cpp` → в меню появится пункт `PMKIDONLY`,
AUTO будет по очереди крутить `OURS` → `PAN` → `PMKIDONLY`.

---

## PACK ↔ CUSTOM

Методика — это **что** делать (алгоритм kick/probe).
**PACK** — это **какую методику включить + как интенсивно** (готовый набор
параметров kick'а). Паки — отдельный, такой же plug-and-play реестр:
см. `src/cap/packs/README.md` — новый пак добавляется точно так же, одним
`.cpp`-файлом в `src/cap/packs/`, без правки `config.cpp`/`settings_menu.cpp`.

В настройках радио (`R`) пункт `PACK` показывает: `STOCK` (встроенный,
дефолт без агрессии, методика `AUTO`) → все паки из `src/cap/packs/` по
порядку → `CUSTOM` (встроенный, read-only флаг — взводится автоматически,
когда крутишь любую ручку вручную).

Если выбрать `STOCK` или любой пак — параметры перезаписываются по пресету.
Если крутишь `BIDIR`, `EAPOL TX`, `KICK N`, `PAUSE MS`, `HOP MS` и т.п. —
PACK автоматически переключается на `CUSTOM`, и пресет больше не
перезапишет твои ручные настройки, пока ты сам не выберешь другой PACK.

Хочешь вернуться к дефолту — выбери `STOCK` (или пункт `RESET` в меню).

Это значит, что твоя методика может **одинаково работать** под любым паком:
юзер берёт агрессивный пак, тихий пак, или руками выкручивает `KICK N=6` +
`EAPOL TX=on` — методика просто читает `ctx.bidirKick / ctx.eapolTx /
ctx.kickBurst / ...` и решает что делать. Никакой логики пресетов внутри
методики быть не должно.
