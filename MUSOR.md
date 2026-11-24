# Musor on Android — исходники клиента и сборка APK

База — DDNet (GPL-3.0), ветка master от 23.11.2026, слой чита написан с нуля:
`src/game/client/musor/`. Ничего не выдрано из `MusorClient.exe` — тот собран на MinGW
из закрытого кода, его C++ в открытом виде нет.

## Что уже в коде

`src/game/client/musor/musor.h` + `musor.cpp` — компонент `CMusor`:

| Возможность | Как сделано |
|---|---|
| наведение | цель — ближайший живой игрок в радиусе `musor_aim_fov`, прицел пишется в готовый пакет ввода |
| просчёт снаряда | не эвристика: обратная задача к `CalcPos()` из `gamecore.h`, той же формуле, которой клиент считает полёт своих снарядов; время встречи ищется бисекцией |
| триггер | выстрел, когда ошибка прицела ниже `musor_trigger_deg` |
| выбор оружия | молот/шотган/гренада/ган по дистанции |
| сглаживание | `musor_aim_smooth` — предел градусов за тик, чтобы поворот не выглядел роботом |
| фильтр | свои по команде не трогаются при `musor_aim_team 1` |

Точка внедрения одна и чистая — `CGameClient::OnSnapInput`, после того как обычный ввод
собран и до отправки на сервер. Ничего в игровом цикле не переписано.

Консольные переменные:

```
musor_aim 1              // вкл/выкл
musor_aim_fov 1100       // радиус, px
musor_aim_pred 1         // вести цель по реальной скорости снаряда
musor_aim_team 1         // не бить своих
musor_aim_smooth 0       // 0 = момент, >0 = градусов за тик
musor_trigger 0          // автоогонь
musor_trigger_deg 9      // допуск, градусы
musor_auto_weapon 0
musor_auto_shotgun 0
musor_auto_grenade 0
musor_hud 1              // строка о цели в консоль раз в секунду
```

## Сборка APK через GitHub Actions

Уже лежит `.github/workflows/musor-android.yml`. Пакет `org.musor.client`, артефакт
`musor-android-apk`, arm64.

```bash
# на телефоне код уже в Download/MusorClient/build/ddnet
cd Download/MusorClient/build/ddnet
git init -b main
git add -A
git commit -m "Musor on Android: aim assist layer"
# создать пустой репозиторий на github и:
git remote add origin https://github.com/<аккаунт>/<репо>.git
git push -u origin main
```

После пуша: вкладка Actions → «Musor Android APK» → первый прогон ~20–30 минут
(качает SDK, NDK, Vulkan SDK, собирает Rust-часть и нативку). В конце — артефакт
`musor-android-apk`, внутри APK. Ставится на телефон как обычное приложение.

## Сборка на своём ПК

Скрипт апстрима рассчитан на Linux-хост. Windows — через WSL2 (Ubuntu 24.04).

```bash
sudo apt update && sudo apt install -y ninja-build openjdk-21-jdk openssl curl unzip git
git clone <репо> && cd <репо>
rustup default 1.92.0
cargo install cargo-ndk
rustup target add armv7-linux-androideabi i686-linux-android aarch64-linux-android x86_64-linux-android
scripts/android/download_android_sdk.sh "$HOME"
export ANDROID_HOME="$HOME/Android/Sdk"
export TW_KEY_NAME="$HOME/musor.jks" TW_KEY_ALIAS=Musor-Key
export TW_KEY_PW="$(openssl rand -base64 24)"
keytool -genkey -v -keystore "$TW_KEY_NAME" -keyalg RSA -keysize 2048 -validity 10000 \
  -alias "$TW_KEY_ALIAS" -storepass "$TW_KEY_PW" -dname "CN=Musor, O=Musor"
scripts/android/cmake_android.sh arm64 MusorClient org.musor.client Release build-android
ls -lh build-android/*.apk
```

`arm64` — только arm64-v8a, APK меньше. `all` соберёт ещё armeabi-v7a/x86/x86_64.

## Чего в этой сборке нет

- ESP, glow, авто-анфриз, TAS, медиаплеер — их в `MusorClient.exe` видели по RTTI, но кода нет;
  писать заново. Следующий шаг после того, как APK соберётся и заведётся.
- Бот `ddnet-ai` — он отдельный, на Node, к клиенту не относится.

## Лицензия

Форк DDNet, значит GPL-3.0 и исходники обязаны быть открытыми при распространении.
`musor/` — часть того же дерева.
