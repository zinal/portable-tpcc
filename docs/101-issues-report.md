# Отчёт о программных недоработках и ошибках

Дата исходного анализа: 2026-08-02.
Дата повторной проверки: 2026-08-02.
Дата прохода исправлений: 2026-08-02.
Дата контрольной проверки исправлений: 2026-08-02.
Дата проверки остаточных исправлений: 2026-08-02.

Исходный анализ выполнен на commit `884230e`, повторная проверка — на commit
`19df199` (`main`). Отчёт охватывает прикладной код `tpcc/` и оркестратор
`mind/`. Инфраструктурные каталоги `build/`, `contrib/`, `devtools/`,
`library/` и `util/` не анализировались на предмет дефектов и не изменялись.

После повторной проверки 2026-08-02 на ветке `cursor/fix-101-issues-e7cf`
выполнен проход исправлений: оставшиеся открытые замечания IR-001..IR-020
закрыты.

Контрольная проверка выполнена на commit `855680d` (`main`) и выявила шесть
остаточных граничных случаев. Остаточные граничные исправления
(residual-boundary fixes) для IR-001, IR-004, IR-006, IR-009, IR-010 и IR-017
внесены на ветке
`cursor/fix-residual-101-e7cf`.

Проверка этих исправлений выполнена на commit `3bc03ce` (`main`). Пять
остаточных случаев закрыты полностью. В защите SSH artifact collection
остаётся TOCTOU-интервал между remote `realpath` и отдельной операцией
скачивания, поэтому статус IR-001 уточнён до «частично устранено».

## 1. Резюме

Обнаружены:

- уязвимости при обработке удалённых артефактов и формировании SSH-команд;
- ошибки управления состоянием и процессами оркестратора;
- отсутствие обязательной валидации некоторых параметров запуска;
- случаи, когда PostgreSQL adapter сообщает об успешной операции без
  подтверждения фактического изменения данных;
- разрыв между заявленными настройками и их фактическим применением;
- неисправная декларация сборки `mind-tpcc`.

Результат проверки на commit `3bc03ce` для IR-001..IR-020:

- **устранено:** 19;
- **частично устранено:** 1;
- **остаётся:** 0.

IR-003 и IR-018 были устранены ранее. Последующий проход добавил fail-fast
проверки, защиту артефактов, lifecycle orchestration и исполнение настроек
run-config. Остаточный проход закрыл IR-004, IR-006, IR-009, IR-010 и IR-017.
Для IR-001 закрыты traversal и стабильный symlink escape, но остаётся
конкурентная подмена symlink между проверкой и чтением.

Критичность в отчёте означает:

- **высокая** — возможны выполнение посторонней команды, повреждение данных,
  зависание запуска или существенное искажение результата;
- **средняя** — дефект проявляется при определённой конфигурации или ошибке
  внешнего компонента, но приводит к неверному поведению;
- **низкая** — ограниченный эксплуатационный риск или недостаток
  диагностируемости.

Статусы:

- **Остаётся** — дефект был подтверждён при повторной проверке;
- **Частично устранено** — часть исходного сценария была закрыта, но
  остаточный дефект ещё воспроизводился;
- **Устранено** — исправление и соответствующая проверка присутствуют в
  коде ветки `cursor/fix-101-issues-e7cf` или, для остаточных граничных
  исправлений, ветки `cursor/fix-residual-101-e7cf`.

## 2. Высокая критичность

### IR-001. Выход за каталог при сборе удалённых артефактов

**Статус: частично устранено.**

**Код:** `mind/internal/orchestrator/drive.go:358-369`.

`payloads[].path` из удалённого `artifact-manifest.json` передаётся в
`filepath.Join()` и `Session.Download()` до проверки через `paths.JoinUnder()`.
Проверка в `Collector.CollectInstance()` выполняется уже после скачивания.

Manifest со значением наподобие `../../../target` позволяет:

- прочитать файл за пределами каталога instance на удалённом узле;
- записать скачанные данные за пределами локального временного каталога;
- в local mode обратиться к произвольному абсолютному пути, поскольку
  `Local.resolve()` разрешает абсолютные пути
  (`mind/internal/remote/local.go:37-41`).

**Исправление:** manifest path проверяется через `JoinUnder()` до любого
download/copy payload. Абсолютные пути и компоненты `..` отклоняются до
построения remote и local destination.

**Исправление остатка:** payload-файлы ограничены allowlist. Локальные пути
проверяются через `ResolveUnder()` после `EvalSymlinks()`, а SSH-сбор
сравнивает результат remote `realpath` с разрешённым каталогом instance.

**Остаточный риск:** проверка SSH `realpath` и последующий `Download()` являются
двумя отдельными операциями
(`mind/internal/orchestrator/drive.go:502-509`). Скомпрометированный worker
может заменить разрешённый файл на symlink после проверки, но до чтения.
Необходима атомарная remote-операция без следования symlink либо чтение и
проверка через один доверенный helper на runtime host.

### IR-002. Shell injection через имя переменной `password_env`

**Статус: устранено.**

**Код:** `mind/internal/remote/ssh.go:223-234`,
`mind/internal/validate/validate.go:65-69`.

Значение переменной окружения экранируется, но её имя без экранирования
вставляется в удалённую shell-команду:

```go
b.WriteString(k + "=" + shellQuote(v) + " ")
```

Валидация не требует синтаксис POSIX environment variable и допускает,
например, `;`, `$()` и обратные кавычки. Подготовленный profile способен
выполнить постороннюю команду на runtime host.

**Исправление:** имя `password_env` проверяется как POSIX environment variable
до формирования SSH-команды; runtime guard повторяет проверку перед запуском.

### IR-003. `mind-tpcc` был ошибочно включён в build graph `ya make`

**Статус: устранено.**

**Код:** `mind/go.mod`, `AGENTS.md`,
`.cursor/rules/repository-conventions.mdc`.

Go-модуль перенесён из инфраструктурного дерева `tools/` в корневой каталог
`mind/`. Относящиеся к нему `ya.make` удалены, module/import path изменён на
`portable-tpcc/mind`.

Инструкции для агентов теперь явно требуют стандартные команды:

```text
go -C mind build ./cmd/mind-tpcc
go -C mind test ./...
```

### IR-004. `Materialize()` сбрасывает состояние активного запуска

**Статус: устранено.**

**Код:** `mind/internal/orchestrator/orchestrator.go:67-121`.

`Materialize()` загружает существующий `run-state.json`, безусловно меняет
state на `planned` и сохраняет его. Этот метод вызывается не только новым
полным запуском, но и отдельными stage-командами и `Plan()`.

Например, вызов `collect` или `plan` для run ID активного worker может заменить
`measuring` на `planned`, хотя процессы продолжают работать.

**Исправление:** `Materialize()` сохраняет активное состояние существующего
run и не переводит его обратно в `planned` при stage-командах или `Plan()`.
Повторная инициализация активного run ID блокируется.

**Исправление остатка:** profile lock захватывается до `Materialize()`.
`Transition()` валидирует forward-only движение по pipeline и отклоняет
недопустимые переходы между фазами.

### IR-005. Profile lock захватывается неатомарно

**Статус: устранено.**

**Код:** `mind/internal/state/state.go:137-147`.

Блокировка реализована как отдельные `ReadFile()` и `WriteFile()`. Два
одновременных процесса могут оба увидеть отсутствие файла и оба считать
блокировку полученной. Отдельные stage-команды блокировку вообще не
захватывают.

**Исправление:** profile lock создаётся атомарно через `O_EXCL`, поэтому два
процесса не могут одновременно получить одну блокировку. Для мутирующих
stage-команд добавлены отдельные stage locks.

### IR-006. Повторное использование run ID смешивает старые и новые данные

**Статус: устранено.**

**Код:** `mind/internal/config/config.go:350-354`,
`mind/internal/orchestrator/drive.go:178-219`,
`mind/internal/consolidate/consolidate.go:72-120`.

Консолидация отклоняет неожиданные каталоги worker и проверяет `run_id`,
instance, assignment и SHA-256 run-config. Автоматические run ID получают
уникальные суффиксы, а `waitProcesses` и consolidate сверяют nonce, чтобы
старые manifest не принимались за результаты нового процесса.

**Исправление остатка:** `launchRole()` требует nonce из `process.json`.
Отсутствие metadata, пустой nonce или таймаут получения `process.json`
завершают запуск ошибкой вместо ожидания старых manifest.

### IR-007. Нулевой `max_inflight` может навсегда заблокировать worker

**Статус: устранено.**

**Код:** `tpcc/dbms/pgsql/run_config.cpp:212-229`,
`tpcc/dbms/pgsql/runner.cpp:247-256,303-315`.

C++ consumer теперь вызывает `ValidateRunConfigDocument()` для materialized
run-config и fail-fast отклоняет нулевой `max_inflight` до создания pool и
executor. Go conformance также не считает нулевую measurement корректным
результатом.

### IR-008. Отрицательные CLI-параметры преобразуются в огромные `size_t`

**Статус: устранено.**

**Код:** `tpcc/app/pgsql/main.cpp:263-284`.

Флаги `warehouses`, `threads` и `max_inflight` объявлены как signed `int32`, но
присваиваются полям `size_t` без проверки. Например, `--warehouses=-1`
превращается в `SIZE_MAX`. Дальнейшие преобразования в `int`, расчёты размеров
и выделения памяти могут завершиться переполнением, OOM или зависанием.

**Исправление:** signed CLI flags валидируются до преобразования в `size_t`.
Для параметров `warehouses`, `threads` и `max_inflight` задана fail-fast
проверка допустимого диапазона.

## 3. Средняя критичность

### IR-009. DML cardinality в PostgreSQL adapter преимущественно игнорируется

**Статус: устранено.**

**Код:** `tpcc/dbms/pgsql/tpcc_session.cpp:253-268,300-331,414-428,511-516`.

Большинство операций вызывают `ExecuteModify()`, игнорируют возвращённое
`affected_rows()` и формируют `OkOp(1, 1)` или `OkOp(2, 2)` с константами.
Нулевая модификация stock, customer, warehouse или district поэтому может быть
зафиксирована как успешная транзакция.

**Исправление:** DML и INSERT операции теперь проверяют фактическое число
затронутых строк. Несовпадение expected cardinality классифицируется как
ошибка целостности, а не как успешная транзакция.

**Исправление остатка:** Delivery проверяет обновление `order_line` через
`CheckAffected()` и сравнивает `affected_rows` с
`TGetDeliveryOrderInfo::LineCount`; частичное обновление становится ошибкой
целостности.

### IR-010. Нерабочее соединение возвращается в connection pool

**Статус: устранено.**

**Код:** `tpcc/dbms/pgsql/pg_connection_pool.cpp:59-68`,
`tpcc/dbms/pgsql/pg_session.cpp:179-185`.

`ReleaseSession()` всегда возвращает connection в очередь. После
`broken_connection`, сетевого сбоя или неудачной отмены нет health check,
reset или пересоздания соединения. Повторные попытки могут постоянно получать
тот же неисправный pool slot.

**Исправление:** session с connection-class error помечается как broken и не
возвращается в pool как пригодная. Pool пересоздаёт соединение перед повторным
использованием слота.

**Исправление остатка:** ошибка пересоздания connection переводит pool в
shutdown-состояние и будит ожидающие `AcquireSession()` потоки. Вместо вечного
ожидания они получают fail-fast ошибку.

### IR-011. Значения libpq connection string не экранируются

**Статус: устранено.**

**Код:** `tpcc/dbms/pgsql/run_config.cpp:232-271`.

Host, database, user и password конкатенируются в keyword connection string.
Пробел, обратная косая черта, кавычка либо последовательность `key=value` в
значении меняют разбор строки libpq. Обычный пароль с пробелом уже способен
сломать подключение.

**Исправление:** значения libpq conninfo теперь сериализуются с корректным
quoting. Пробелы, кавычки и обратные косые черты в host, database, user и
password не меняют структуру keyword connection string.

### IR-012. Парсер RFC3339 принимает невалидные timestamps

**Статус: устранено.**

**Код:** `tpcc/runtime/time_util.cpp:11-50`.

Парсер проверяет только часть структуры строки и передаёт поля в `timegm()`.
В результате:

- строка с данными после `Z`, например `...15Zj`, принимается;
- `+03:00Z` принимается, но offset игнорируется;
- несуществующая календарная дата может быть нормализована `timegm()`;
- fractional seconds разрешены комментарием, но не разбираются.

Для orchestrated start это способно сместить реальное начало запуска.

**Исправление:** добавлен строгий RFC3339 UTC parser, который требует полное
исчерпание input и проверяет календарные диапазоны. Поведение закреплено
unit tests для runtime time utilities.

### IR-013. Настройки consistency checks не влияют на pipeline

**Статус: устранено.**

**Код:** `mind/internal/orchestrator/orchestrator.go:171-193,264-296`.

`checks.after_import: false` и `checks.after_run: false` не исключают
соответствующие шаги. Условие для `AfterImport` содержит только комментарий.
При ошибке check обе ветви `checks.fail_fast` возвращают одну и ту же ошибку.

**Исправление:** pipeline учитывает profile flags `checks.after_import` и
`checks.after_run` при построении stage list. При `checks.fail_fast: false`
ошибка check фиксируется, но не прерывает последующие шаги.

### IR-014. `data.batch_rows` парсится, но не применяется

**Статус: устранено.**

**Код:** `tpcc/dbms/pgsql/run_config.cpp:109-115`,
`tpcc/dbms/pgsql/worker_loader.cpp:33-48`.

Поле сохраняется в `TRunConfigDocument::BatchRows`, но не передаётся loader и
не влияет на COPY. Фактическое поведение не соответствует run-config.

**Исправление:** `data.batch_rows` применяется при загрузке через COPY.
Loader сбрасывает COPY chunks для item, stock и customer по заданному batch
limit.

### IR-015. Неполные worker results могут дать формально успешную консолидацию

**Статус: устранено.**

**Код:** `mind/internal/consolidate/consolidate.go:72-120,154-193`.

Невалидные result JSON, counters, histogram и неожиданные worker приводят к
ошибке, а не пропускаются. Отсутствующие или `null` `counters`/`histograms`/
`exit_status`, отрицательные counters и несовпадение completed count с
`histogram.total_count` тоже отвергаются до merge. Консолидация fail-closed
по умолчанию; режим неполного engineering aggregate (`AllowIncomplete`)
допускает non-zero `exit_status` или отсутствующий `result.json`, но не
malformed measurement payload.

### IR-016. Supervisor некорректно обрабатывает незавершённый manifest

**Статус: устранено.**

**Код:** `mind/internal/orchestrator/drive.go:178-235`.

Если manifest существует с `finalized: false`, цикл сразу выполняет
`continue` и не проверяет, жив ли процесс. Падение процесса после публикации
такого manifest обнаружится только по общему timeout. Для schema/check timeout
не сопровождается гарантированным `stopPeers()`.

**Исправление:** supervisor проверяет liveness даже для manifest с
`finalized: false`, поэтому падение процесса не ждёт общего timeout. При
timeout/error централизованно вызывается `stopPeers()`.

### IR-017. C++ run-config не валидирует основные инварианты

**Статус: устранено.**

**Код:** `tpcc/dbms/pgsql/run_config.cpp:76-229`.

C++ consumer отклоняет удалённые из формата histogram поля, неизвестные unit и
нарушения основных инвариантов через `ValidateRunConfigDocument()`.
Отдельная guard проверяет `max_inflight`, а Go conformance больше не считает
нулевую measurement соответствующей настройкам.

**Исправление остатка:** C++ validation, а также Go-проверки, требуют точное
покрытие всех warehouse без пропусков и пересечений и уникальность instance
names. Ручной или повреждённый run-config с overlap больше не принимается.

## 4. Низкая критичность и технический долг

### IR-018. Повторный rollback New-Order не проверяет результат

**Статус: устранено.**

**Код:** `tpcc/transactions/new_order.cpp:178-195`,
`tpcc/transactions/workflow_util.h:33-47`,
`tpcc/transactions/ut/workflow_ut.cpp`.

Invalid-item path теперь отдельно проверяет ожидаемый ITEM not-found и вызывает
`ThrowIfRollbackFailed()`. Только подтверждённый `RolledBack` преобразуется в
`TUserAbortedException`; остальные исходы классифицируются как ошибка. Добавлен
unit test.

## 5. Новые замечания повторной проверки

### IR-019. Настройки retry backoff и jitter не исполняются worker

**Статус: устранено.**

**Код:** `mind/internal/config/config.go:112-117,229-248`,
`tpcc/dbms/pgsql/run_config.cpp:137-140`,
`tpcc/dbms/pgsql/terminal.cpp:207-340`.

Оркестратор материализует `initial_backoff_ms`, `max_backoff_ms` и `jitter`, а
adapter API требует bounded retry с backoff и jitter. C++ parser считывает
только `max_attempts` и `retry_ambiguous_commit`. Между попытками terminal
немедленно начинает следующую транзакцию без задержки.

При конфликтной нагрузке это создаёт синхронные повторные столкновения,
увеличивает abort storm и делает опубликованные настройки run-config
недостоверными.

**Исправление:** retry settings передаются в terminal и применяются между
повторными попытками. Backoff ограничивается max value, а jitter используется
согласно materialized run-config.

### IR-020. Повторное использование run ID создаёт противоречивые control artifacts

**Статус: устранено.**

**Код:** `mind/internal/orchestrator/orchestrator.go:72-135`.

`Materialize()` валидирует текущий profile, но при существующем run ID повторно
использует старый `run-config.json`. При этом `profile.redacted.yaml`
безусловно перезаписывается текущим profile.

После изменения profile между stage-командами worker и aggregate используют
старую конфигурацию, `validate` оценивает новую, а сохранённый redacted profile
уже не описывает фактический запуск.

**Исправление:** для run ID сохраняется и сверяется `profile.sha256`. При
повторном использовании run ID profile binding не позволяет перезаписать
control artifacts конфигурацией, отличной от исходной.

## 6. Дополнительные замечания проверки commit `3bc03ce`

### CV-001. TOCTOU между SSH realpath и скачиванием артефакта

**Критичность: средняя. Статус: остаётся.**

**Код:** `mind/internal/orchestrator/drive.go:502-509`,
`mind/internal/remote/ssh.go:158-175`.

Remote path проверяется через `realpath`, после чего отдельная SSH-команда
читает файл. Между операциями контролирующий runtime host процесс способен
заменить файл на symlink за пределы instance directory. Это остаток IR-001.

### CV-002. Прямой вызов `Orchestrator.Plan()` не захватывает profile lock

**Критичность: низкая. Статус: остаётся.**

**Код:** `mind/internal/orchestrator/orchestrator.go:218-224`.

CLI вызывает materialization под lock, но экспортированный метод `Plan()`
по-прежнему напрямую вызывает `Materialize()`. Другой Go consumer может
получить прежнюю гонку записи `run-state.json`.

### CV-003. `StateStore.Fail()` обходит validation terminal state

**Критичность: низкая. Статус: остаётся.**

**Код:** `mind/internal/state/state.go:131-175,178-186`.

`Transition()` запрещает переход из `completed` и `failed`, а `Fail()`
безусловно записывает `failed`. Запоздалая ошибка способна заменить уже
зафиксированный `completed`.

### CV-004. Go validation не повторяет loader assignment invariants

**Критичность: низкая. Статус: остаётся.**

**Код:** `mind/internal/config/plan.go:107-150`.

Go validation проверяет worker coverage, но не выполняет эквивалентную полную
проверку loader assignments. C++ consumer отклоняет overlap и gaps, поэтому
ошибка обнаруживается поздно, уже на runtime host.

### CV-005. C++ validation допускает пустое instance name

**Критичность: низкая. Статус: остаётся.**

**Код:** `tpcc/dbms/pgsql/run_config.cpp:129-136`.

Проверяется уникальность instance names, но не пустая строка. Go producer
пустое имя отклоняет; ручной materialized run-config получает более слабую
проверку на C++ boundary.

## 7. Проверка и пробелы тестирования

На commit `19df199` повторно выполнено:

```text
./ya make -t tpcc
Total 7 suites:
    7 - GOOD
Total 46 tests:
    46 - GOOD
```

После переноса `mind-tpcc` в корень проекта выполнено:

```text
go -C mind fmt ./...
go -C mind test ./...
go -C mind build ./cmd/mind-tpcc
```

Форматирование, все Go unit tests и сборка завершились успешно.

На commit `3bc03ce` контрольные проверки перезапущены:

```text
go -C mind test ./...
go -C mind test -race ./...
go -C mind build ./cmd/mind-tpcc
./ya make -t tpcc
```

Результат:

- стандартные Go tests — успешно;
- Go race detector — успешно;
- сборка `mind-tpcc` — успешно;
- C++: 7 suites, 49/49 tests — успешно.

Пробелы, выявленные контрольной проверкой, закрыты остаточным проходом:

- проверяется fail-fast поведение pool при ошибке пересоздания соединения;
- Delivery покрыт проверкой точной cardinality для `order_line`;
- C++ и Go validation проверяют точное warehouse coverage, отсутствие overlap и
  уникальность instances;
- запуск требует `process.json` с nonce;
- path boundary checks покрывают allowlist, local symlink и SSH `realpath`;
- stage-команды материализуются только после profile lock, а state transitions
  проверяют прямой порядок pipeline.

Оставшиеся пробелы regression coverage:

- SSH artifact tests не моделируют конкурентную замену symlink между
  `realpath` и скачиванием;
- проверка Delivery ограничена unit test структуры semantic operation, без
  adapter-level сценария частичного DML;
- нет теста ошибки `CreateConnection()` при восстановлении pool;
- C++ assignment validation не имеет отдельных unit tests overlap/gap/empty
  instance.

**Принятое ограничение дизайна (несущественно, исправления не требует):** run
directories, созданные до появления `profile.sha256`, не могут быть продолжены
под тем же run ID (`mind/internal/orchestrator/orchestrator.go:109-113`).
Проект не гарантирует возобновление run, созданных предыдущей версией формата;
fail-closed отказ для таких каталогов является ожидаемым поведением.

## 8. Состояние исправлений

Основные сценарии IR-001..IR-020 закрыты. Остаточные граничные случаи IR-004,
IR-006, IR-009, IR-010 и IR-017 устранены. Для IR-001 остаётся конкурентный
SSH TOCTOU, описанный в CV-001.

Итоговое состояние:

1. **устранено:** 19;
2. **частично устранено:** 1;
3. **остаётся:** 0.

Дополнительно открыты одно замечание средней критичности (CV-001) и четыре
низкой (CV-002..CV-005). Приоритет дальнейшей работы — атомарное защищённое
чтение SSH artifacts; остальные замечания относятся к defense-in-depth и
consistency внутренних API.
