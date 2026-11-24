# MusorClient — исходники и Android-сборка

Два независимых результата в одной папке:

1. **`MusorClient.apk`** — рабочий клиент под этот телефон (arm64). Готов к установке.
2. **Исходники** — форк DDNet 20.2.1 с самописным читом (`MusorClient-android-src.zip`) плюс инжектор (`android/`).

Оригинальный Windows-клиент (`MusorClient-win64-client.zip`) и исходники его бота
(`MusorClient-src.zip`, `src/`) лежат рядом как есть.

---

## Android: установка

Файл: `MusorClient.apk` (132 МБ), пакет `org.ddnet.client`, подпись — моя (`CN=ChessHint`).

1. Удалить установленный DDNet (`org.ddnet.client`) — подписи разные, поверх не встанет.
2. Поставить `MusorClient.apk`.
3. Запустить. Всё, что делает мод, пишется в `musor.log` в корне внутренней памяти
   (`/sdcard/musor.log`) — там же дублируется в `Android/data/org.ddnet.client/files/musor.log`.

Настройки — файл `musor.cfg`, кладётся в корень внутренней памяти (`/sdcard/musor.cfg`)
или в `Android/data/org.ddnet.client/files/`. Готовый образец — `musor.cfg` рядом с APK.
Читается один раз при старте; правишь — перезапускай игру.

### Что умеет

| ключ | по умолчанию | смысл |
|---|---|---|
| `aim` | 1 | наведение на ближайшего |
| `fov` | 1100 | радиус поиска цели, юнитов |
| `pred` | 1 | упреждение по скорости цели + поправка на гравитацию |
| `team` | 1 | не целиться в своих |
| `smooth` | 0 | ограничение поворота, градусов за тик (0 — мгновенно) |
| `trigger` | 0 | автоогонь, когда прицел в пределах `trigger_deg` |
| `verbose` | 0 | писать в лог цель/дистанцию раз в секунду |
| `gun_speed` / `gun_curv` | 2200 / 1.25 | физика лазера |
| `shotgun_speed` / `shotgun_curv` | 2750 / 1.25 | физика дробовика |
| `grenade_speed` / `grenade_curv` | 1000 / 7.0 | физика гранаты |

Целится тем оружием, которое сейчас в руках: лазер, дробовик, граната — с упреждением,
остальное (молот, пистолет) — прямо по цели.

---

## Android: как это сделано

DDNet для Android — это `libDDNet.so` (38 МБ, arm64), который грузит `libmain.so` из
`ClientActivity`. Пересобирать игру с нуля на телефоне смысла нет, поэтому чит вставлен
в уже собранную библиотеку:

- `android/libmusor.c` — сам чит: настройки, решение траектории, хук. Собирается в
  `libmusor.so` (14 КБ) тем же clang из NDK.
- `android/patch_needed.py` — добавляет в `libDDNet.so` запись `DT_NEEDED libmusor.so`
  (имя в 32-байтную щель между `.eh_frame` и `.text`, вместо дублирующего `DT_FLAGS_1`,
  с расширенным `DT_STRSZ`). Дальше библиотеку грузит сам линкер Android — до любого
  Java-кода.
- Хук: три инструкции в `CGameClient::OnSnapInput` (RVA `0xb3ec60`, хвостовой вызов
  `CControls::SnapInput`) заменяются на `adrp x17 / add x17 / br x17`. Чит получает
  готовый ввод от игры и правит в нём только прицел.
- `android/repack.py` — пересборка APK байт-в-байт: чужие записи копируются как есть,
  меняются только две, выравнивание `resources.arsc` (4) и `.so` (4096) сохраняется.

Смещения под 20.2.1 вынуты из дизассемблера, не из головы: `m_Controls` +0x520bd0,
`m_aLocalIds` +0x7092f4, `m_aClients` +0x54aec0 (шаг 0x3770), `m_Predicted.m_Pos` +0x90,
`CClientData::m_Team` +0x50. На другой версии DDNet их надо пересчитывать заново.

Пересборка с нуля:

```bash
python3 android/patch_needed.py /tmp/libDDNet.patched.so
python3 android/repack.py DDNet-20.2.1.apk /tmp/new.apk \
    /tmp/libDDNet.patched.so=lib/arm64-v8a/libDDNet.so \
    android/libmusor.so=lib/arm64-v8a/libmusor.so
apksigner sign --ks debug.keystore --ks-key-alias chesshint --out MusorClient.apk /tmp/new.apk
```

---

## Исходники клиента

`MusorClient-android-src.zip` (91 МБ, с историей git, коммит `4233fc2`) — DDNet master
от 23.11.2026 плюс слой чита `src/game/client/musor/{musor.h,musor.cpp}`, врезанный в
`CGameClient::OnSnapInput`. Собирается как обычный DDNet под Android.

Сам C++ оригинального MusorClient закрыт: репозиторий `github.com/ConoF/MusorClient`,
на который ссылается exe, отдаёт 404. В `MusorClient.exe` остались только RTTI-имена
модулей и манглированные символы — по ним виден состав, не код.

---

## Windows-часть

Собрано из `Download/Telegram/MusorClient-win64.zip` (103 МБ, 2026-11-04).
Клиент — форк DDNet 20.x для Windows x64 с ботом на Node и модулями чита на C++.

| Путь | Размер | Что это |
|---|---|---|
| `MusorClient-win64-client.zip` | 103 МБ | клиент целиком: `MusorClient.exe`, 20 DLL, `data/`, `ddnet-ai/` с `node.exe` и `node_modules` |
| `MusorClient-src.zip` | 1.0 МБ | только исходники бота: 102 файла, 35 972 строки TS |
| `src/` | 5.0 МБ | те же исходники распакованными |
| `analysis/client-notes.txt` | 6 КБ | разбор `MusorClient.exe` и `steam_api.dll`: хеши, PE, модули чита, сетевые адреса |

Запуск: Windows 10/11 x64, распаковать, `MusorClient.exe`. Настройки идут в
`%APPDATA%\DDNet`, `ddnet-ai` и `data` удалять нельзя. `steam_api.dll` — заглушка на
14 экспортов, отдаёт «Steam запущен», наружу не ходит.

Бот отдельно:

```bash
cd src/ddnet-ai
npm ci
node start.mjs          # нужен Node 24+: start.mjs импортирует .ts напрямую
```
