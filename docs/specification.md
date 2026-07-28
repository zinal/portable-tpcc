# Спецификация portable-tpcc

Статус: проект архитектуры, версия 1.

Ключевые слова **MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT** и **MAY**
трактуются в смысле RFC 2119.

## 1. Назначение

`portable-tpcc` — горизонтально масштабируемый генератор нагрузки TPC-C для
нескольких СУБД. Проект состоит из:

1. общих C++-библиотек с моделью TPC-C, генератором данных, терминальным
   runtime, метриками и проверками;
2. адаптеров СУБД;
3. исполняемых файлов, скомпонованных с конкретным адаптером;
4. отдельного оркестратора `tpccctl`, который подготавливает базу, распределяет
   нагрузку между хостами, синхронизирует фазы, собирает артефакты и формирует
   единый результат.

Первый набор адаптеров:

- YDB;
- PostgreSQL;
- OceanBase.

Архитектура не должна требовать fork общей логики для добавления СУБД.

Этот документ не заменяет и не пересказывает стандарт TPC-C. Состав схемы,
правила генерации данных, транзакционные профили, терминальная модель,
распределения входов, response-time requirements и формулы стандартных метрик
определяются выбранной редакцией TPC-C. `portable-tpcc` хранит ссылку и
идентификатор этой редакции в manifest запуска. Здесь определены только
архитектурные способы совместно реализовать эти правила для нескольких СУБД и
нескольких хостов генератора.

## 2. Область применимости

### 2.1. Цели

- Одна логическая база TPC-C и произвольное число хостов генератора.
- Однозначное владение складами worker-процессами.
- Горизонтальная загрузка начальных данных.
- Одинаковые данные и логические входы транзакций для всех адаптеров.
- DBMS-специфичные DDL, bulk load, SQL, retry mapping и физическая раскладка.
- Синхронные границы ramp-up, measurement и drain.
- Слияние счётчиков и гистограмм без усреднения перцентилей.
- Полный воспроизводимый manifest запуска.
- Автоматические проверки до и после теста.

### 2.2. Не-цели первой версии

- Создание или администрирование кластеров СУБД.
- Kubernetes, Ansible или systemd как обязательная среда.
- Один универсальный SQL-диалект.
- Поддержка серверных stored procedures как переносимого интерфейса.
- Автоматическая сертификация результата TPC.
- Автоматическое продолжение measurement после потери worker.
- Динамическая перебалансировка терминалов во время measurement.

`portable-tpcc` MUST называть результат официальным TPC-C только после
независимой проверки всех требований TPC. По умолчанию отчёт содержит
`result_class: engineering`.

### 2.3. Режимы

`engineering` разрешает profile overrides стандартных параметров для
диагностики. Каждое такое изменение MUST быть явно перечислено в
`deviations`.

`conformance` запрещает overrides стандартных параметров и требует полного
набора проверок и артефактов, определённого выбранной редакцией стандарта.
Конкретные значения не дублируются в этой спецификации: их предоставляет
versioned модуль `tpcc/spec/<edition>`. Само имя режима не является заявлением
о сертификации результата.

## 3. Общая архитектура

```text
                         CONTROL HOST
                ┌─────────────────────────┐
                │ tpccctl                 │
                │ profile / state / merge │
                └──────┬─────────────┬────┘
                  SSH  │             │ artifacts
          ┌────────────┴──────┐  ┌───┴────────────┐
          │ loader processes  │  │ worker processes│
          │ global + W ranges │  │ W ranges        │
          └───────────┬───────┘  └───────┬────────┘
                      │                  │
                      └────────┬─────────┘
                               │ adapter API
                    ┌──────────▼──────────┐
                    │ one logical database│
                    │ YDB / PG / OB       │
                    └─────────────────────┘
```

Роли:

| Роль | Количество | Ответственность |
| --- | ---: | --- |
| `control` | 1 | `tpccctl`, единственное mutable run-state |
| `db` | 1 логическая БД | заранее созданная и доступная СУБД |
| `loader` | 1..N | детерминированные непересекающиеся части загрузки |
| `worker` | 1..N | workload закреплённых складов |
| `results` | 1 | каталог консолидированных артефактов |

## 4. Компоненты и границы библиотек

### 4.1. `tpcc/spec`

Выбранная редакция стандарта представляется одним versioned C++ package. Он
содержит только машиноисполняемые правила и test vectors, необходимые
реализации; нормативным источником остаётся внешний документ TPC-C.

Package собирается как:

- библиотека, с которой линкуются `domain`, workload binaries и contract
  tests;
- DBMS-neutral CLI `tpcc-spec`, используемый Go-оркестратором;
- JSON Schema для входов и выходов CLI.

Минимальный CLI:

```text
tpcc-spec describe --edition <id>
tpcc-spec materialize --edition <id> --scale <json> --seed-source <json>
tpcc-spec derive-terminals --spec-state <json> --assignment <json>
tpcc-spec expected-data --spec-state <json> --load-plan <json>
tpcc-spec qualify --spec-state <json> --aggregate-input <json>
```

Все команды являются чистыми функциями и выводят canonical JSON. `describe`
возвращает immutable edition ID, URL нормативного документа, его известный
SHA-256, module ABI version и module SHA. `materialize` создаёт opaque
`spec-state.json`; оркестратор не интерпретирует его внутренние параметры.

`tpccctl` MUST NOT реализовывать правила TPC-C на Go. Он вызывает `tpcc-spec`,
валидирует JSON Schema и сохраняет binary/module hash. Workload binary при
старте проверяет, что linked module SHA совпадает с `spec-state.json`.

### 4.2. `tpcc/domain`

Не зависит от SDK СУБД, сети и планировщика:

- versioned представление требований выбранной редакции TPC-C;
- типы идентификаторов;
- точные числовые типы;
- immutable input и output transaction types, экспортированных spec module;
- NURand и генерация строк;
- derivation детерминированных RNG stream;
- правила начального наполнения;
- общие бизнес-расчёты;
- инварианты и ожидаемые cardinality.

Scale и ограничения типов задаёт versioned spec module. В C++ точные
десятичные значения представляются checked fixed-point типами; преобразование
в `double` внутри domain и adapter API MUST NOT использоваться.

### 4.3. `tpcc/generator`

- создаёт общие параметры генератора на test run;
- создаёт начальные данные;
- создаёт вход логической транзакции;
- предоставляет независимые stream по ключу:
  `(run_seed, purpose, warehouse, district, terminal, sequence)`;
- генерирует одинаковые значения независимо от числа loader/worker процессов.

Параллелизм MUST NOT менять содержимое базы. Для одного `run_seed` хэш
канонической строки каждой записи одинаков при любом sharding.

### 4.4. `tpcc/transactions`

Содержит общую последовательность бизнес-операций. Библиотека работает через
типизированный `ITpccSession`, а не через SQL, `pqxx`, YDB SDK или `MYSQL*`.

Нормативный adapter API:

```text
Begin(TIsolation) -> TTransaction
Execute(TTransaction&, TSemanticOperation) -> TOperationResult
ExecuteBatch(TTransaction&, TSemanticBatch) -> TBatchResult
ExecuteFinalAndCommit(TTransaction&, TSemanticOperation)
    -> {TOperationResult, TCommitOutcome}
Commit(TTransaction&) -> TCommitOutcome
Rollback(TTransaction&) -> TRollbackOutcome
Cancel(TTransaction&) -> TCancelOutcome
```

`TTransaction` имеет явные состояния `active`, `committing`, `committed`,
`rolled_back`, `outcome_unknown`. `TCommitOutcome` содержит certainty и native
diagnostics. Operation result задаёт ожидаемую cardinality; нарушение
ожидания является `integrity`, а не пустым успешным результатом.

Batch-граница нужна, чтобы YDB мог выполнять set-oriented YQL, PostgreSQL —
prepared SQL/COPY, а OceanBase — cached prepared statements без изменения
общего алгоритма. `ExecuteFinalAndCommit` позволяет YDB объединить последний
запрос и commit без скрытых deferred side effects. Дополнительное fusion
разрешено только внутри одной semantic operation/batch.

### 4.5. `tpcc/runtime`

- terminal state machines из assignment spec module;
- coroutine scheduler;
- keying и think time;
- admission control;
- immutable logical transaction envelope;
- retry loop;
- executor асинхронных частей workload;
- фазовые барьеры;
- mergeable metrics;
- graceful drain.

Runtime зависит только от `domain`, `transactions` и абстрактного adapter API.

### 4.6. `tpcc/loader`

- строит план строк по shard;
- выделяет единственного владельца DB-wide набора данных;
- создаёт детерминированный batch с устойчивым идентификатором и hash;
- передаёт его в единый идемпотентный `PutBatch`;
- MAY вести локальный cache успешно завершённых batch только как оптимизацию;
- сверяет cardinality и канонические выборочные hash после загрузки.

### 4.7. `tpcc/checks`

Общие описания:

- каталог проверок выбранной редакции стандарта;
- versioned идентификатор и ожидаемый тип результата каждой проверки;
- проверки полноты загрузки и согласованности shard;
- статистические проверки исполненного workload;
- инфраструктурные проверки фаз, ownership и артефактов.

SQL/запрос для каждого условия реализуется адаптером, но идентификатор,
ссылка на пункт стандарта, ожидаемая семантика и формат результата общие.
Текст стандартного условия в библиотеке и этой спецификации не копируется.

### 4.8. `tpcc/metrics`

- счётчики;
- гистограммы с общими границами;
- события ошибок/retry;
- сериализация worker result;
- детерминированное слияние;
- qualification flags.

### 4.9. Адаптеры

Каждый `tpcc/dbms/<name>` реализует:

1. `IAdminAdapter` — schema, index, analyze/compact, clean и metadata;
2. `ILoadAdapter` — идемпотентный `PutBatch` и `Ensure*`-операции;
3. `ISessionFactory` / `ITpccSession` — транзакции;
4. `ICheckAdapter` — запросы общих инвариантов;
5. `IErrorClassifier` — нормализованные ошибки;
6. `ICapabilities` — isolation, batch, commit, cancellation и topology;
7. DBMS-специфичную конфигурацию и её строгую валидацию.

SDK-типы MUST NOT выходить за границу адаптера.

### 4.10. Исполняемые файлы

Первая версия собирает отдельные программы:

```text
tpcc-ydb
tpcc-pgsql
tpcc-oceanbase
tpcc-spec
tpccctl
```

C++-программы линкуют одни и те же общие библиотеки и один адаптер. Такой
подход не требует runtime plugin ABI и не тащит клиентские библиотеки всех
СУБД в один binary.

`tpccctl` — отдельный self-contained Go binary.

## 5. Горизонтальное исполнение workload

### 5.1. Assignment

Versioned spec module строит полный набор стандартных terminal identities для
заданного scale. Пользовательский profile перечисляет только loader/worker
instances и hosts; ручных warehouse ranges и признака владельца DB-wide
данных в profile нет.

Assignment определяет владение **домашними терминалами** склада; он не
ограничивает доступ транзакций к строкам других складов. Один набор домашних
терминалов склада MUST NOT делиться между worker-процессами.

`tpccctl` применяет `balanced-contiguous-v1` отдельно к loaders и workers:

1. сортирует instances по ASCII-имени в bytewise ascending order;
2. делит число складов на число instances;
3. первые `warehouse_count % instance_count` instances получают на один склад
   больше;
4. формирует непрерывные полуоткрытые диапазоны без пересечений и пропусков;
5. назначает DB-wide данные первому loader в том же порядке.

Количество instances MUST быть положительным и не превышать число складов.
Алгоритм, входной instance set и вычисленный assignment записываются в
`run-config.json`/`load-plan.json` и показываются командой `plan` до любых
side effects. Ручной override assignment в v1 отсутствует.

Добавление worker изменяет только assignment. Оно MUST NOT менять logical
scale, число терминалов, их идентичность или параметры генератора. Worker
получает assignment в неизменяемом `run-config.json` и проверяет его SHA-256
из start token.

Статическое владение выбрано намеренно: динамический reassignment во время
measurement меняет pacing, RNG streams и набор соединений, поэтому запрещён.

### 5.2. Runtime worker

Один worker содержит:

- terminal state machines закреплённых складов;
- coroutine scheduler и monotonic timers;
- ограничитель одновременных обращений к adapter;
- отдельные исполнители асинхронных частей workload, требуемых стандартом;
- локальные счётчики и mergeable histograms;
- phase controller и graceful drain.

Каждый terminal state сериален: следующая логическая операция не начинается,
пока предыдущая не перешла в окончательное состояние. Число OS threads,
соединений и inflight операций — параметры производительности worker, а не
параметры logical scale.

### 5.3. Логическая транзакция и retry

До первой попытки runtime создаёт immutable envelope:

```json
{
  "run_id": "...",
  "worker_id": "...",
  "terminal_id": "...",
  "sequence": 42,
  "transaction": "...",
  "input": {},
  "input_timestamp": "...",
  "logical_id": "..."
}
```

Все входы, timestamps и logical ID MUST оставаться неизменными до
окончательного результата. Общий runtime не позволяет адаптерам повторно
вызывать генератор при retry.

Нормализованные классы ошибок:

| Класс | Действие |
| --- | --- |
| `retryable_abort` | rollback подтверждён; bounded retry с backoff+jitter |
| `not_committed` | безопасно повторить согласно контракту адаптера |
| `ambiguous_commit` | MUST NOT повторять без backend-specific resolution |
| `permanent` | завершить операцию ошибкой; policy решает судьбу run |
| `integrity` | fail run |
| `cancelled` | завершение фазы, не retry |

Внутренний retry SDK и общий retry runtime MUST образовывать один наблюдаемый
budget. Число попыток, задержки и native error code попадают в метрики.

### 5.4. Асинхронные части workload

Требуемые стандартом отложенные операции реализуются общим runtime через
типизированную bounded queue и отдельный executor. Adapter выполняет только
атомарную DB-часть элемента очереди.

Queue сохраняет logical ID и времена enqueue/start/completion. На границе
measurement admission прекращается, а уже принятые элементы получают
отдельное окно drain. Конкретные сроки и критерии берутся из versioned spec
module, а не задаются этим документом.

## 6. Разделение логической и физической схемы

### 6.1. Общий schema model

Versioned spec module описывает таблицы, столбцы, ограничения и логически
необходимые access paths в DBMS-neutral AST. Он является единственным
источником схемы для generator, checks и adapter contract tests.

Adapter преобразует AST в DDL и physical layout. Он MAY добавлять служебные
ключи, indexes, partitions и storage options, если:

- логическая видимость и транзакционная семантика не меняются;
- добавление отражено в run manifest;
- эквивалентные indexes не дублируются;
- общие проверки могут отличить логические данные от служебных.

Точные десятичные типы domain MUST отображаться в точные типы СУБД.

### 6.2. YDB

Адаптер SHOULD:

- ставить warehouse key первым для warehouse-local таблиц;
- использовать range partitioning и документировать split policy;
- использовать `GlobalSync` indexes только там, где они нужны запросам;
- использовать typed `BulkUpsert` для загрузки;
- использовать set-oriented операции и commit в последнем запросе;
- получать topology hints как рекомендацию, а не менять logical scale;
- не отображать точные domain values в `Double`;
- не скрывать retry внутри `RetryQuery`.

`.sys/nodes`, compaction, index implementation tables и YDB status codes
остаются внутри адаптера.

### 6.3. PostgreSQL

Адаптер SHOULD:

- использовать prepared statements и `COPY`;
- задавать fully-qualified identifiers вместо зависимости от `search_path`;
- отображать exact domain values в DECIMAL;
- классифицировать ошибки по SQLSTATE;
- применять достаточную isolation и row locking;
- не создавать по OS thread на каждый coroutine при наличии неблокирующего
  транспорта; если используется blocking libpqxx, IO pool MUST быть ограничен;
- создавать customer index после bulk load и затем выполнять `ANALYZE`.

### 6.4. OceanBase

Адаптер SHOULD:

- использовать tablegroup/hash partitioning по warehouse key;
- отдельно настраивать DB-wide и warehouse-scoped данные;
- использовать cached prepared statements и parameter binding;
- различать deadlock, lock timeout, serialization failure, killed transaction,
  connection loss и ambiguous commit;
- валидировать `max_inflight > 0`;
- поддерживать явный выбор foreign keys как физическую настройку, отражаемую в
  результате;
- выполнять `ANALYZE` после создания indexes;
- не считать MariaDB integration test заменой теста на OceanBase.

Connector/C, local index syntax, timeout variables и catalog queries остаются
в адаптере.

## 7. Горизонтальная загрузка

### 7.1. План

Оркестратор строит `load-plan.json`:

- сначала canonical `plan_payload` только из assignments и batches;
- `plan_payload_sha256` — hash всего plan payload;
- `load_id` — SHA-256 canonical tuple
  `(run_id, plan_payload_sha256, spec-state SHA, loader binary SHA)`;
- итоговый document содержит payload, `plan_payload_sha256` и `load_id`, после
  чего отдельно вычисляется hash всего `load-plan.json`;
- ровно один shard владеет DB-wide данными, определёнными spec module;
- warehouse-scoped данные делятся непересекающимися диапазонами складов;
- batch имеет `batch_id`, диапазон ключей, число строк и собственный
  `batch_payload_sha256` канонических строк;
- assignment не зависит от порядка запуска.

### 7.2. Возобновление

У loader есть один DBMS-neutral контракт:

```text
PutBatch(load_id, batch_id, table, key_range, rows, batch_payload_sha256)
    -> completed | outcome_unknown | failed
```

`PutBatch` MUST быть идемпотентным: любое число повторов одного batch, в том
числе после `outcome_unknown` или crash, приводит к тому же конечному набору
строк, что и одно успешное выполнение. Поэтому неподтверждённый batch всегда
повторяется без отдельного recovery mode.

Перед первым batch адаптер атомарно связывает workload path с `load_id`.
Пустой path принимает новый идентификатор, частично загруженный path — только
тот же идентификатор. Наличие другого `load_id` является `integrity` и требует
явного `clean`/нового path; это предотвращает смешивание двух datasets и
остаточные строки от другого scale.

Для выполнения контракта:

- строки и все их значения полностью детерминированы `spec-state`;
- batch содержит полные значения, а не относительные increments;
- logical/technical keys стабильны;
- server-generated timestamps, sequences и другие меняющиеся defaults при
  загрузке запрещены;
- `batch_id` связан с `load_id`, key range и `batch_payload_sha256`;
- другой payload для того же batch identity является `integrity`;
- таблица без подходящего logical key получает детерминированный служебный
  ключ либо реализуется адаптером через staging + replace-range.

`PutBatch` — семантическая операция, а не требование использовать SQL
`INSERT`. Адаптер MAY применять upsert, staging/merge, replace-range или
внутренний ledger, но эти варианты не видны loader и profile.

Локальный checkpoint успешных batch MAY ускорять повторный `load`. Он связан
с hashes run/profile/load-plan/spec-state/binary и не является условием
корректности: при отсутствии или неопределённости checkpoint batch просто
повторяется.

Создание схемы, indexes и statistics выполняется отдельными идемпотентными
`EnsureSchema`, `EnsureIndexes` и `EnsureStatistics`. После всех `PutBatch`
оркестратор запускает post-import checks; их результат является
authoritative подтверждением полноты загрузки.

### 7.3. Проверки после импорта

MUST выполняться на quiescent database:

- все проверки, помеченные spec module как `after-import`;
- полнота batch manifest;
- отсутствие пересечений и пропусков load assignment;
- соответствие фактических cardinality ожидаемым значениям, вычисленным spec
  module для scale;
- canonical sample hashes, одинаковые для всех адаптеров;
- готовность DBMS-specific indexes/statistics.

Отчёт хранит идентификаторы проверок, ссылки на пункты стандарта и
машиночитаемые результаты. Он не копирует нормативный текст TPC-C.

## 8. Оркестратор `tpccctl`

### 8.1. Принципы

Control plane следует следующим принципам:

1. один self-contained Go binary;
2. декларативный YAML profile `portable-tpcc/v1`;
3. SSH + SFTP/tar без обязательного агента;
4. `plan` без side effects;
5. immutable `run-config.json`, byte-identical на runtime hosts;
6. argv содержит только путь к config, instance selector и process-local пути;
7. mutable `run-state.json` только на control host;
8. host-local deploy manifest;
9. process identity, stdout/stderr и readiness files в каталоге instance;
10. SHA-256 после распространения конфигурации;
11. local profile lock плюс DB-scoped fence и execution gate;
12. fail-fast для параллельных стадий;
13. сбор сырых артефактов даже при неуспехе;
14. secrets только через environment и временные файлы mode 0600.

### 8.2. Команды

```text
tpccctl validate
tpccctl plan
tpccctl deploy
tpccctl schema
tpccctl load
tpccctl check [--after-import|--after-run]
tpccctl start
tpccctl status
tpccctl stop
tpccctl collect
tpccctl consolidate
tpccctl run
tpccctl cleanup --yes
```

`run` выполняет:

```text
validate → deploy → schema → load → check(after-import)
→ arm workers → ramp-up → measurement → drain
→ check(after-run) → collect → consolidate
```

Отдельные skip flags разрешены только в `engineering`; все пропуски
записываются как deviations.

### 8.3. Profile и run-config

Человек редактирует profile. Оркестратор валидирует его и создаёт
нормализованный `run-config.json`, включающий:

- schema version и run ID;
- SHA profile и binaries;
- edition metadata, `tpcc-spec` binary SHA и `spec-state.json` SHA;
- dbms kind и не-секретную конфигурацию;
- scale и warehouse assignment;
- opaque generator/spec state reference;
- относительные длительности и phase policy;
- histogram schema;
- expected workers;
- policy retry/failure;
- пути артефактов.

Пароль в run-config отсутствует; сохраняется только имя environment variable.
Profile содержит instance inventory, но не assignment; все ranges в
run-config являются вычисленным output `balanced-contiguous-v1`.
Функциональные параметры worker MUST NOT дублироваться в argv. Worker
запускается как `tpcc-<dbms> worker --run-config <path> --instance <name>` и
выбирает собственный assignment по `instance`.

Примеры:

- [profile.v1.yaml](examples/profile.v1.yaml);
- [control-config.v1.json](examples/control-config.v1.json);
- [run-config.v1.json](examples/run-config.v1.json);
- [start-token.v1.json](examples/start-token.v1.json).

Примеры иллюстративны: значения вида `*_SHA256` заменяются генератором.
Production hash — 64 lowercase hex символа от canonical JSON по RFC 8785 либо
от исходных binary bytes. Test fixtures MUST содержать реальные проверяемые
hashes.

Реализация MUST хранить JSON Schema для profile, control-config, run-config,
spec-state, start-token, readiness, process state и результатов. YAML profile
валидируется как JSON data model с `additionalProperties:false`. Defaults
материализуются в локальный immutable `control-config.json` и runtime
`run-config.json`; после этого исходный profile больше не читается.
Control-config содержит SSH inventory, local/state/result paths и deploy
policy. Run-config содержит только параметры runtime hosts.

### 8.4. Каталоги

Runtime host:

```text
/opt/portable-tpcc/
├── .tpccctl/deploy-manifest.json
├── bin/
├── schema/
└── runs/<run_id>/
    ├── run-config.json
    ├── spec-state.json
    ├── start-token.json
    ├── load-plan.json
    ├── loader/<name>/
    └── worker/<name>/
        ├── process.json
        ├── ready.json
        ├── armed.json
        ├── stdout.log
        ├── stderr.log
        ├── events.jsonl
        └── result.json
```

Control host:

```text
<state-dir>/
├── profiles/<profile-id>/current-run.json
├── profiles/<profile-id>/run.lock
└── runs/<run_id>/
    ├── run-state.json
    ├── control-config.json
    ├── profile.redacted.yaml
    ├── run-config.json
    ├── spec-state.json
    └── load-plan.json
```

### 8.5. Deploy и cleanup

Deploy:

- проверяет source file hash;
- обновляет host-local manifest инкрементально;
- начинает с `complete:false`, завершает `complete:true`;
- повторный deploy идемпотентен.

Cleanup удаляет только пути из полного manifest и никогда не выполняет
безусловный `rm -rf remote_root`. В non-interactive режиме требуется `--yes`.

### 8.6. DB-scoped fence

До `schema`, `load`, `check` или `start` control получает через `IAdminAdapter`
fence на adapter-discovered canonical database identity. Identity MUST
содержать устойчивые cluster/tenant/database IDs и не может состоять только из
пользовательского endpoint alias.

Адаптер реализует fence атомарной служебной metadata record вне benchmark
tables. Record содержит `run_id`, случайный fencing token, generation и
`not_after`. Другой profile/control не может получить следующую generation до
истечения текущей. Mutating admin operation и каждый load batch передают
generation; БД отклоняет stale generation.

До старта workload `not_after` MUST быть позже максимального drain deadline с
запасом. Worker проверяет fence и execution gate перед ramp. После commit gate
другой control не может получить fence до завершения старого run, даже если
первый control упал. Потеря/преждевременное истечение fence делает run failed.
Metadata не входит в измеряемую схему и удаляется только владельцем token.

### 8.7. Синхронизация часов и двухфазный старт

Clock calibration использует несколько samples на host, выбирает sample с
минимальным RTT и сохраняет offset вместе с uncertainty. Проверка повторяется
перед measurement и после него; worker обнаруживает wall-clock step, а
deadlines исполняет по monotonic clock. Profile задаёт максимальные skew,
uncertainty и drift.

Старт разделён на prepare и commit:

1. Control распространяет `run-config.json` и `spec-state.json`.
2. Worker проверяет hashes, DB fence и adapter, создаёт runtime и пишет
   `ready.json`, но не запускает workload.
3. После полного ready set control создаёт `start-token.json`, связанный с
   config SHA, fence generation и hash ready set. Token содержит будущие phase
   epochs и ожидаемую generation DB-side execution gate.
4. Worker атомарно принимает token и пишет `armed.json`.
5. Только после полного armed set с актуальными process heartbeats control
   одной DB operation переводит общий execution gate из `prepared` в
   `committed`. Gate содержит config/token/ready-set hashes, fence generation
   и `not_before`.
6. Worker допускает workload только после чтения `committed` gate с точным
   совпадением generation и hashes.

Если ready/armed set неполон, control не может commit gate и ни один
корректный worker не начинает workload. DB-side gate устраняет частичное
обновление host-local файлов. После commit потеря worker не останавливает
мгновенно остальные процессы, но фиксируется heartbeat/status и делает
итоговый run failed.

`ready.json` содержит:

```json
{
  "schema_version": 1,
  "run_id": "20260728-lab-ydb",
  "instance": "worker-a",
  "instance_nonce": "...",
  "run_config_sha256": "...",
  "spec_state_sha256": "...",
  "binary_sha256": "...",
  "adapter": "ydb",
  "warehouse_ranges": [[1, 101]],
  "ready_at": "2026-07-28T11:59:20Z",
  "clock_calibration": {
    "measured_at": "2026-07-28T11:59:18Z",
    "offset_ms": 3,
    "uncertainty_ms": 8,
    "rtt_ms": 11
  }
}
```

### 8.8. Учёт на границах фаз

Worker хранит отдельные сырые populations по временам submit/start/complete и
не очищает shared counters на границе warmup. Для каждой логической операции
сохраняются monotonic timestamps, phase epochs и outcome. Drain operations
учитываются отдельно.

Какие populations входят в стандартные throughput и response-time metrics,
решает только `tpcc-spec qualify`. Оркестратор не кодирует эти правила и не
перекладывает поздние completions между populations.

### 8.9. Process supervision

Первая версия использует `nohup`, но PID не считается достаточной
идентичностью. `nohup` запускает маленький wrapper с заранее созданным
instance nonce. Wrapper получает exclusive instance lock, записывает и
`fsync`-ит `process.json` со своим PID, `/proc/self` start time, nonce,
run/config hashes и generation, после чего делает `exec` workload binary с
тем же PID. Ошибка регистрации запрещает `exec`.

Повторный start сначала reconciles remote record; сигнал можно послать только
процессу с совпавшими PID, start time и nonce.

Если control упал между launch и записью local state, следующий запуск
восстанавливает процессы по remote records и config hash. Stale record
перемещается в artifacts, а не молча перезаписывается.

Stop:

1. посылает worker SIGTERM;
2. worker прекращает admission и выполняет drain;
3. ждёт `stop_grace`;
4. при необходимости SIGKILL;
5. проверяет отсутствие процесса;
6. сохраняет частичные артефакты.

Повторный stop идемпотентен. Потеря worker во время measurement завершает
общий run как failed; reassignment запрещён, потому что изменил бы terminal
population и timing.

### 8.10. Состояния

```text
planned → deploying → schema → loading → checking_import
→ preparing → arming → ramping → measuring → draining
→ checking_result → collecting → consolidating → completed
```

Любое состояние может перейти в `stopping` и затем `failed`. Запись
run-state выполняется атомарно через temporary file + rename и содержит
последнюю ошибку и все известные процессы.

## 9. Метрики и консолидация

### 9.1. Worker result

Каждый worker пишет JSON с:

- run/config/binary/profile SHA;
- идентификатором редакции TPC-C и spec module SHA;
- adapter и server version;
- assignment;
- фактическими phase timestamps;
- стандартными workload counters по schema spec module;
- retry по normalized class и native code;
- telemetry асинхронных очередей;
- response, DB-attempt, admission-wait и end-to-end histograms;
- входными статистиками, необходимыми spec module для проверки workload;
- clock diagnostics;
- fatal errors и deviations.

Histogram хранит counts общих buckets в микросекундах, encoding/version,
underflow/overflow, а также mergeable `count`, точную сумму duration и точный
maximum. Worker не является источником итоговых percentile или average.

### 9.2. Консолидация

`consolidate` MUST:

1. проверить одинаковые run ID и config SHA;
2. проверить полный expected worker set;
3. проверить непересекающееся полное assignment;
4. проверить одинаковую histogram schema;
5. сложить bucket counts и counters;
6. вычислить percentile только после merge;
7. передать объединённые данные spec module для расчёта стандартных метрик без
   искусственного clamp;
8. вычислить производные engineering metrics отдельно;
9. применить qualification rules;
10. сохранить ссылки на исходные worker artifacts.

Нельзя:

- складывать или усреднять p99;
- масштабировать частичный результат на отсутствующие workers;
- заменять потерянные samples нулями;
- скрывать outcomes;
- ограничивать вычисленную стандартную метрику искусственным максимумом.

### 9.3. Итоговые артефакты

```text
results/<run_id>/
├── raw/loader/<instance>/
├── raw/worker/<instance>/
├── orchestrator/
│   ├── profile.redacted.yaml
│   ├── run-config.json
│   ├── spec-state.json
│   ├── run-state.json
│   ├── start-token.json
│   └── load-plan.json
├── checks/
│   ├── after-import.json
│   └── after-run.json
├── collection-manifest.json
├── aggregate.json
└── summary.txt
```

`aggregate.json` — канонический результат. `summary.txt` только представляет
его и не содержит уникальных данных.

Перед collect каждый процесс атомарно публикует `artifact-manifest.json` с
размером и SHA-256 каждого **payload**-файла, exit status, instance nonce и
`finalized:true`; сам manifest не входит в собственный payload. Collector
сначала копирует во временный каталог, затем проверяет manifest и только после
этого публикует raw instance directory.

После сбора control атомарно создаёт `collection-manifest.json`, покрывающий
все process manifests, control-config, run/spec/start state, load plan и check
results. Aggregate строится только из файлов этого manifest и сохраняет его
SHA-256. Незапечатанные данные остаются как `partial`, но не участвуют в
qualified aggregate.

### 9.4. Qualification flags

Оркестратор формирует инфраструктурные flags:

```text
workers_complete
assignment_valid
clock_skew_valid
phase_boundaries_valid
post_import_checks_valid
post_run_checks_valid
no_ambiguous_commit
no_integrity_errors
no_drain_cancellations
artifacts_sealed
```

Стандартные qualification flags поставляет versioned spec module. Итог
`qualified` равен conjunction инфраструктурных и стандартных обязательных
flags выбранного режима. Итоговый JSON хранит source (`orchestrator` или
`spec:<edition>`) каждого flag.

## 10. Ошибки, восстановление и идемпотентность

| Операция | Контракт |
| --- | --- |
| `validate`, `plan` | без side effects |
| `deploy` | повторяемый по manifest/hash |
| `schema` | проверяет существующее состояние; destructive recreate только с явным флагом |
| `load` | повторяемый: неизвестные batch безопасно проходят через `PutBatch` повторно |
| `start` | разрешён только владельцу profile lock, DB fence и prepared execution gate; другой активный run запрещён |
| `stop` | повторяемый, already stopped = success |
| `collect` | повторяет download во временный каталог и атомарно публикует |
| `consolidate` | чистая детерминированная функция артефактов |
| `cleanup` | только manifest-owned paths и явное подтверждение |

Частично успешная параллельная стадия считается failed. Оркестратор пытается
остановить уже запущенные процессы и собрать их логи. Ошибка collect не
переписывает исходную причину failure, а добавляется отдельной причиной.

## 11. Безопасность

- DB и SSH passwords MUST NOT находиться в profile artifacts, argv, logs,
  run-config или run-state.
- Profile с секретами редактируется перед сохранением.
- Предпочтителен ssh-agent; private key password auth первой версии не нужен.
- Секрет БД передаётся как имя environment variable.
- При необходимости remote secret создаётся mode 0600, source-ится wrapper
  shell и удаляется до `exec`.
- Host keys MUST проверяться; `insecure_ignore_host_key` разрешён только в
  `engineering` и отражается как deviation.
- Все пути нормализуются относительно разрешённых roots; `..` и symlink escape
  отклоняются.
- Логи native drivers проходят redaction известных connection-string форм.

## 12. Валидация

`tpccctl validate` MUST отвергать:

- неизвестный `apiVersion`, DBMS или поле;
- instance name вне `[a-z][a-z0-9-]*` или повторное имя;
- warehouse count вне поддерживаемого диапазона;
- пустой список loaders/workers или число instances больше числа складов;
- любые ручные `warehouse_ranges`, `assignment` или `owns_global_data` в
  profile;
- zero/negative threads, pool, duration, timeout или batch size;
- нарушение полноты вычисленного assignment или неединственный владелец
  DB-wide data shard;
- reuse remote `(host, run_dir, instance)`;
- отсутствующие artifacts;
- secret literal вместо `password_env`;
- credentials в endpoint URL, connection string или DBMS options;
- несовместимые adapter capabilities/isolation;
- отключение требуемого spec module runtime subsystem в conformance;
- слишком короткий lead time;
- несовместимые histogram schemas;
- retry policy, допускающую replay `ambiguous_commit`.

Adapter preflight проверяет server version, permissions, connectivity,
isolation, schema state и физическую конфигурацию.

## 13. Проверки и тесты реализации

### 13.1. Общие unit tests

- test vectors versioned spec module;
- domain types и canonical encoding;
- immutable input across injected retries;
- transaction workflows через fake adapter;
- `tpcc-spec` CLI/library equivalence;
- terminal identity и warehouse assignment;
- phase classification на граничных timestamps;
- histogram merge;
- load sharding независимо от числа shards.

### 13.2. Adapter contract suite

Один набор тестов запускается для каждой СУБД:

- DDL/create/clean;
- initial population hash/cardinality;
- прерывание `PutBatch` на разных стадиях и безопасный повтор;
- все операции, экспортированные выбранным spec module;
- transaction rollback atomicity;
- deadlock/serialization retry;
- ambiguous commit injection;
- асинхронные runtime operations;
- полный каталог общих checks;
- cancellation и reconnect policy.

### 13.3. Orchestrator tests

- strict profile validation;
- plan snapshots и argv;
- manifest-safe cleanup;
- redaction;
- config distribution/hash mismatch;
- deterministic assignment при нечётном делении и разном порядке profile;
- отказ при ручном или повреждённом assignment;
- collision DB-scoped fence и stale generation из другого profile;
- missing worker/early exit;
- incomplete ready/armed set и uncommitted execution gate;
- startup deadline;
- normative stop/drain;
- PID reuse и recovery после control crash;
- partial artifact collection;
- artifact manifest tampering;
- aggregate golden files;
- integration test через локальный SSH target.

### 13.4. Cross-DB equivalence

Для небольшого общего seed:

1. загрузить все три СУБД;
2. сравнить canonical row hashes;
3. выполнить фиксированный trace логических inputs;
4. сравнить normalized outputs и checks;
5. отдельно разрешить только документированные различия physical metadata.

## 14. Предлагаемая структура репозитория

```text
tpcc/
├── spec/
├── domain/
├── generator/
├── transactions/
├── runtime/
├── loader/
├── checks/
├── metrics/
├── dbms/
│   ├── ydb/
│   ├── pgsql/
│   └── oceanbase/
└── app/
    ├── ydb/
    ├── pgsql/
    └── oceanbase/
tools/
└── tpccctl/
docs/
├── specification.md
└── examples/
```

Все C++ targets описываются `ya.make`. Go orchestrator собирается существующей
поддержкой Go в `ya make`; альтернативная корневая build system не вводится.

## 15. Критерии готовности первой версии

1. Три binaries проходят одну adapter contract suite.
2. Одинаковый seed создаёт эквивалентные logical datasets.
3. Два и более worker hosts покрывают склады без дублирования терминалов.
4. Потеря worker делает run failed, но сохраняет частичные артефакты.
5. Фазы синхронизированы и warmup samples не попадают в measurement.
6. Retry fault injection подтверждает неизменяемость input.
7. Все требуемые spec module асинхронные операции имеют completion metrics.
8. Domain values не теряют точность на границе адаптера.
9. Post-import и post-run checks успешны.
10. Aggregate воспроизводится из raw artifacts без доступа к СУБД.
11. Profile, run-config и results не содержат секретов.
12. `plan`, повторяемый load, idempotent stop/collect и manifest cleanup покрыты
    тестами.

## 16. Открытые решения перед реализацией

До начала кодирования требуется зафиксировать:

1. конкретный C++ future/coroutine ABI общих библиотек;
2. bucket layout гистограмм и максимальную измеряемую latency;
3. набор поддерживаемых edition packages и правила их обновления;
4. стратегию разрешения ambiguous commit для каждой СУБД;
5. формат canonical row encoding для cross-DB hash;
6. минимальные поддерживаемые версии YDB, PostgreSQL и OceanBase;
7. политику хранения асинхронных очередей при аварии worker.

Эти решения MUST быть приняты как versioned ADR до стабилизации
`portable-tpcc/v1`; они не должны незаметно кодироваться в первом адаптере.
