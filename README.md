# Desktop Hub — виджеты рабочего стола Windows

Серия небольших виджетов для рабочего стола Windows 10/11 и центр, который ими управляет.
Каждая программа — один `.exe` без установки и зависимостей (C++17, Win32, GDI+).

**Как начать:** скачайте `WidgetHub.exe` из [последнего релиза](https://github.com/namalkin/desktophub/releases/latest)
и запустите. В центре видно описание и скриншоты каждого виджета; кнопка «Скачать»
забирает нужный виджет из того же релиза, «Запустить» — запускает. Значок в трее один —
у центра.

![Центр виджетов](hub/docs/screenshot.png)

| Папка | Что это |
|---|---|
| [`hub`](hub/README.md) | Центр виджетов: каталог, скачивание, запуск, настройки, один значок в трее |
| [`widget 1`](widget%201/README.md) | Счётчик — обратный отсчёт до события и учёт долга |
| [`widget 2`](widget%202/README.md) | Полки для ярлыков — блоки на рабочем столе, куда перетаскиваются ярлыки |
| [`widget 3`](widget%203/README.md) | Планер — задачи дня, цели недели, привычки, дневник, заметки |
| [`widget 4`](widget%204/README.md) | Диск-пылесос — что занимает диск и что можно безопасно удалить |
| [`widget 5`](widget%205/README.md) | Плеер — музыка из папки с обложками и списком треков |

## Если браузер или Defender блокирует скачивание

Программы пока не подписаны, а у нового неподписанного `.exe` нет «репутации»: Microsoft
Defender может удалить его при скачивании с пометкой `Trojan:Win32/Wacatac.B!ml`. Окончание
`!ml` — вердикт эвристической модели, а не найденный вирус; исходники и сборка открыты в этом
репозитории. Подпись через SignPath Foundation в процессе оформления. Пока её нет — можно
скачать `WidgetHub.zip` или сообщить о ложном срабатывании на
[microsoft.com/wdsi/filesubmission](https://www.microsoft.com/wdsi/filesubmission).

## Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by
[SignPath Foundation](https://signpath.org).

- Committers and reviewers: [Namalkin](https://github.com/namalkin)
- Approvers: [Namalkin](https://github.com/namalkin)

Подписываются только файлы, собранные GitHub Actions из этого репозитория
([`.github/workflows/release.yml`](.github/workflows/release.yml)).

**Privacy.** Центр виджетов сам обращается только к GitHub: `api.github.com` — узнать
последний релиз, `github.com` — скачать центр и виджеты, когда пользователь нажимает
«Скачать» или «Обновить». Никакие данные о пользователе или компьютере не отправляются.
Виджеты в сеть не ходят.

## Сборка

Нужен только Docker Desktop: компилятор MinGW-w64 живёт в контейнере.

```powershell
.\hub\scripts\build.ps1            # центр  -> hub\dist\WidgetHub.exe
& '.\widget 5\scripts\build.ps1'   # виджет -> widget 5\dist\TuneBox.exe
```

Новый виджет — новая папка `widget N`, запись в `hub\catalog.json` и копия
`hub\src\hub_link.h` с обработкой команд центра.

**Релиз:** поднять версию в `VERSION`, закоммитить и отправить тег:

```powershell
git tag v1.2.0; git push origin main v1.2.0
```

GitHub Actions соберёт центр и все виджеты, подпишет их (когда подключён SignPath) и выложит
релиз — центр у всех пользователей сам предложит обновиться. Локальный
`.\hub\scripts\release.ps1` остаётся запасным путём: он выкладывает неподписанные сборки
с этого компьютера.

Код распространяется под лицензией [MIT](LICENSE).
