# Анализ соответствия реализации требованиям TPC-C 5.11

Статус: обновлённый анализ реализации на commit `1aac6c4a`.

Основа сравнения: [TPC Benchmark C Standard Specification, Revision
5.11](https://www.tpc.org/TPC_Documents_Current_Versions/pdf/tpc-c_v5.11.0.pdf).

## Актуальный результат

Репозиторий реализует engineering workload по мотивам TPC-C для PostgreSQL,
YDB и OceanBase, но не полный официальный тест TPC-C 5.11. Результаты
намеренно маркируются `result_class: engineering` и не должны публиковаться
как официальный `tpmC` без обязательной независимой TPC verification.

Фактически исполняемый код всех трёх адаптеров проверен отдельно. Проектная
документация использовалась только как перечень ранее найденных проблем, а не
как доказательство реализованности. Формулы consistency suite сверялись с
Clause 3.3.2 TPC-C 5.11.

Общая бизнес-логика пяти транзакций, runtime, pacing, measurement boundaries
и worker artifacts теперь вынесены в shared modules. Поэтому большинство
workload-свойств одинаково для всех трёх СУБД; основные различия находятся в
SQL/YQL adapters, physical schema, affected-row checks и preflight.

### Сводный вердикт по СУБД

| Область | PostgreSQL | YDB | OceanBase |
| --- | --- | --- | --- |
| Пять shared transaction workflows | Реализованы | Реализованы | Реализованы |
| Consistency conditions 3.3.2.1-12 | Полный набор, формулы корректны | Полный набор; есть NULL fail-open и ослабленный 3.3.2.4 | Полный набор, dialect translation корректен |
| Post-import catalog | Полный | Полный | Полный |
| Exact money/rate domain path | Да | Да, `Decimal(22,9)` | Да |
| Delivery fail-closed affected rows | Да | Неполно | Да |
| Physical schema близка к TPC-C | Да, с отдельными type deviations | Существенно более permissive | Да, с отдельными type deviations |

Ни один вариант не является полным официальным TPC-C 5.11 benchmark из-за
общих пробелов initial population, variability verification, RTE/response-time
reporting, deferred Delivery и certification procedures.

## Область и проверка

Проверены:

- shared schema model, generator, population и load paths;
- shared workflows New-Order, Payment, Order-Status, Delivery и Stock-Level;
- PostgreSQL, YDB и OceanBase transaction/session adapters;
- terminals, mix, keying/think time, pacing, retries и phase controller;
- worker artifacts, histograms, collect и consolidate;
- consistency/post-import checks и их orchestration;
- git history прежних исправлений и изменения после commit `19df199`.

Выполнены:

```text
./ya make -t -DHAVE_CUDA=no -DCUDA_VERSION=11.4 \
  tpcc/domain/ut tpcc/generator/ut tpcc/transactions/ut \
  tpcc/runtime/ut tpcc/metrics/ut tpcc/harness/ut \
  tpcc/checks/ut tpcc/loader/ut \
  tpcc/dbms/pgsql/ut tpcc/dbms/ydb/ut tpcc/dbms/oceanbase/ut
go -C mind test ./...
```

Результат: 11 C++ suites / 145 tests и все Go tests прошли. Эти тесты не
исполняют весь SQL/YQL consistency catalog на намеренно повреждённых данных и
не заменяют integration runs на живых СУБД, длительный measurement interval
или failure/recovery tests.

Критичность ниже относится к достоверности engineering-результата. Некоторые
пункты отдельно помечены как высокие только для официального TPC-C.

## Активные замечания

### 1. Фактическая variability не верифицируется

**Критичность: высокая для официального TPC-C.**

TPC-C §5.5.1.5 требует проверять на measurement interval фактические:

- 0,9-1,1% rollback New-Order;
- среднее число order lines 9,5-10,5 и распределение 5-15;
- 0,95-1,05% remote order lines;
- 14-16% remote Payment;
- 57-63% customer-by-last-name для Payment и Order-Status.

Вероятности заданы в `tpcc/transactions/new_order.cpp` и
`tpcc/transactions/payment.cpp`, но `tpcc/harness/artifacts.cpp` публикует
только transaction outcome counters и latency histograms. Business-input
counters отсутствуют, поэтому фактическую выборку нельзя проверить post-hoc.

### 2. Response-time reporting остаётся неполным

**Критичность: высокая для официального TPC-C; средняя для engineering.**

Worker и consolidator корректно сохраняют full response-time histogram,
`min`/`max`/`avg` и p50/p90/p95/p99. Microsecond mode больше не проходит через
предварительное округление до milliseconds.

По-прежнему отсутствуют:

- Remote Terminal Emulator и menu response time;
- отдельные interactive/deferred Delivery metrics;
- проверка допустимых p90;
- обязательные frequency distributions и графики §5.6;
- truncation reported throughput до нуля знаков после запятой.

Синхронная Delivery и отсутствие RTE являются принятыми product deviations,
но не соответствуют полному официальному тесту.

### 3. Initial population не полностью соответствует TPC-C

**Критичность: высокая для официального TPC-C. Затрагивает все СУБД.**

`RandomAString` в `tpcc/generator/strings.h` генерирует только `a-z`, а не
alphanumeric character set. NURand C values в
`tpcc/domain/constants.h` (`C_LAST_LOAD_C`, `C_LAST_RUN_C`, `C_ID_C`,
`OL_I_ID_C`) заданы compile-time constants вместо выбора требуемых значений
на population/run.

Cardinalities, customer/order permutation, delivered/undelivered split,
`OL_DELIVERY_D = O_ENTRY_D`, delivered `OL_AMOUNT = 0.00`,
`D_NEXT_O_ID = 3001` и initial YTD при этом реализованы корректно.

### 4. Stale local collection может дать aggregate предыдущей попытки

**Критичность: высокая для достоверности engineering-результата.**

Remote process metadata очищается перед launch, nonce сверяется при
supervision, а consolidator проверяет согласованность nonce внутри collected
bundle. Однако standalone `consolidate` пропускает auto-collect, если уже
существует `collection-manifest.json`.

Сценарий: `test` и `collect` завершились, затем тот же run повторно запущен на
workers, после чего оператор вызывает только `consolidate`. Старый local
`results/<run_id>/raw/` остаётся внутренне согласованным и может быть принят
как результат новой попытки. Нужна инвалидация collection manifest/raw при
новом launch либо привязка collection к ожидаемым launch nonces.

### 5. Aggregate throughput использует configured duration

**Критичность: средняя.**

Worker записывает phase timestamps и `metrics.measurement_seconds`, но
`mind/internal/consolidate/consolidate.go` делит completed New-Order count на
`run-config.phases.measurement_ms`. Фактические worker timestamps и duration
не сверяются. При early stop или schedule drift aggregate throughput может не
совпасть с реально исполненным interval.

### 6. Custom binary gate не согласован с launch

**Критичность: высокая operational / средняя для результата.**

Материализованный `run-config.binary` может содержать custom worker binary.
Pre-run gate использует default `tpcc-<dbms>`, тогда как launch использует
`run-config.binary`; `WorkerBinary` также не участвует в проверке CLI
overrides. Staged invocation способна пройти gate и затем запустить другое
имя либо завершиться только на remote launch.

### 7. YDB consistency checks fail-open на NULL

**Критичность: высокая для integrity verdict YDB.**

YDB physical schema оставляет большинство non-key columns nullable.
Consistency checks 3.3.2.1, 3.3.2.8, 3.3.2.9, 3.3.2.10 и 3.3.2.12, а также
часть post-import checks используют `!=`. В YQL `NULL != value` даёт
`UNKNOWN`, строка не попадает в `bad`, и повреждение может дать false pass.

Проблемные места находятся в `tpcc/dbms/ydb/check.cpp` около строк 201, 325,
341, 370 и 412. PostgreSQL использует `IS DISTINCT FROM`, OceanBase —
null-safe `<=>`.

YDB 3.3.2.4 дополнительно использует
`COALESCE(o.sum_ol_cnt, 0) != COALESCE(ol.ol_count, 0)`. Это отождествляет
отсутствующую сторону с нулём и слабее PostgreSQL `IS DISTINCT FROM`.

### 8. YDB transaction adapter не везде fail-closed

**Критичность: средняя; для повреждённых данных Delivery — высокая.**

YDB adapter часто возвращает ожидаемое число affected rows константой, не
проверяя фактический результат. В частности:

- batch Delivery не проверяет число удалённых `new_order` и обновлённых
  `order_line`;
- `TCompleteOrderDelivery::LineCount` не используется в batch path;
- New-Order создаёт `oorder` и `new_order` через `UPSERT`, тогда как
  PostgreSQL/OceanBase используют `INSERT`;
- carrier обновляется частичным `UPSERT`, способным скрыть отсутствующий
  parent row.

Это ослабляет обнаружение corruption и reservation bugs. При этом вывод о
гарантированном concurrent double-delivery был бы неверен: все операции одной
бизнес-транзакции выполняются в `SerializableRW` transaction
(`tpcc/dbms/ydb/ydb_session.cpp`), а YDB `ABORTED` классифицируется как
retryable. Отсутствие `FOR UPDATE` само по себе не доказывает double commit;
активный дефект — именно отсутствие fail-closed affected-row/cardinality
checks.

### 9. Payment при одном warehouse нарушает local input rule

**Критичность: средняя. Затрагивает все СУБД через shared workflow.**

В 15% ветви Payment при `WarehouseCount == 1` customer warehouse остаётся
home warehouse, но customer district выбирается случайно. Для single-warehouse
configuration remote customer невозможен, поэтому должны использоваться home
warehouse и home district.

### 10. Physical schema расходятся между адаптерами

**Критичность: средняя/низкая.**

- OceanBase задаёт `S_YTD decimal(8,2)` вместо TPC-C `Numeric(8,0)`;
- PostgreSQL/OceanBase задают `OL_QUANTITY decimal(6,2)` вместо
  `Numeric(2,0)`;
- YDB использует широкие `Decimal(22,9)`, unbounded `Utf8` и оставляет
  большинство non-key fields nullable;
- YDB `OL_QUANTITY` представлен `Int32`.

Штатный generator записывает допустимые значения, поэтому normal workload
семантически работает, но DDL не обеспечивает все logical constraints.

### 11. Order-Status output lines не сортируются в PostgreSQL/OceanBase

**Критичность: средняя для официальной транзакции; низкая для текущего
engineering режима без RTE.**

TPC-C §2.6.2.2 требует ascending `OL_NUMBER`. YDB использует
`ORDER BY ol_number`; PostgreSQL и OceanBase читают lines без `ORDER BY`.
Текущий shared workflow не публикует screen output, поэтому это не меняет
database state, но остаётся отклонением от официального профиля транзакции.

### 12. Check query errors смешиваются с consistency failures

**Критичность: средняя для диагностики. PostgreSQL и OceanBase.**

Per-chunk runner ловит любое исключение — timeout, connection loss, syntax
error или найденное нарушение — и записывает `ECheckStatus::Failed`.
Structured report различает `failed` и `error`, но adapter это различие
теряет. `report.Ok()` остаётся false, поэтому fail-open не возникает, однако
причина результата классифицируется неверно.

### 13. Недостаточное regression coverage для SQL/YQL checks

**Критичность: низкая как самостоятельный дефект, высокая как риск регрессии.**

`tpcc/checks/ut/catalog_ut.cpp` проверяет catalog IDs, phase filtering и
progress format. SQL/YQL predicates трёх adapters не выполняются на fixtures
с NULL, orphan rows, mixed delivery dates и неверными aggregates. Нужны
DB-backed integration tests либо dialect-specific query fixtures.

## Подтверждённые исправления без регрессии

На текущем HEAD подтверждены:

- intentional rollback New-Order выполняет DB profile всех валидных lines,
  принимает только ожидаемый ITEM not-found и подтверждённый rollback;
- intentional rollback входит в New-Order throughput и response-time
  histogram;
- default think time экспоненциальный;
- Stock-Level использует постоянный terminal home district при стандартных
  10 terminals/warehouse;
- `O_ALL_LOCAL` корректен при одном warehouse;
- `OL_DELIVERY_D = O_ENTRY_D` для initial delivered orders;
- consistency checks используют NULL-only semantics carrier, не `0` sentinel;
- post-import suite проверяет delivered carrier range и `OL_AMOUNT = 0.00`;
- PostgreSQL/OceanBase Delivery используют lock и affected-row guard;
- measurement boundaries учитывают только транзакции, полностью лежащие
  внутри interval;
- malformed/missing worker counters, histograms и exit status отвергаются;
- histogram extrema обязательны, merge использует checked arithmetic;
- `unit: us` сохраняет microsecond precision;
- soft launch conformance правильно трактует mix minima, minimum keying/think
  times и non-positive/short measurement;
- warehouse check ranges параметризованы и одинаковы во всех adapters;
- YDB Optional<Bool>, absolute path prefix и Decimal aggregate casts сохранены;
- OceanBase exact money comparisons, query timeout и FULL JOIN emulation
  сохранены.

Таким образом, прежние defects 5.1-5.14 и 5.17-5.19 не вернулись, кроме
того, что Delivery concurrency fix был реализован как explicit row guard
только в PostgreSQL/OceanBase. Для YDB эквивалентная защита от concurrent
commit обеспечивается serializable conflict detection, но defensive
affected-row validation остаётся неполной.

## Принятые ограничения продукта

Эти пункты остаются отклонениями от официального TPC-C, но соответствуют
выбранному engineering scope:

- синхронная inline Delivery без deferred queue и 80-second metric;
- отсутствие полноценного RTE, menu/screens и end-user response time;
- отсутствие встроенных ACID certification, power-loss и durability tests;
- deterministic synthetic initial dates вместо wall clock loader host;
- external checkpoint/recovery control;
- отсутствие автоматического sustained-operation proof, FDR, price/tpmC и
  независимой TPC verification.

## Итоговая оценка

| Область | Оценка |
| --- | --- |
| Shared transaction core | Основные пять workflows реализованы; active gap — single-Warehouse Payment district |
| Initial population | Cardinalities и delivered split корректны; a-string и per-run C values не соответствуют |
| PostgreSQL adapter | Consistency suite корректен; Delivery guards есть; Order-Status ordering и check status classification остаются |
| YDB adapter | Serializable workload и полный catalog; NULL fail-open checks и неполные affected-row guards |
| OceanBase adapter | Consistency suite корректен; Delivery guards и timeout есть; отдельные DDL type deviations |
| Runtime | Think time, pacing, rollback accounting и measurement boundaries исправлены |
| Reporting | Histogram integrity исправлена; official variability и RT reporting неполны |
| Orchestration | Nonce/identity checks усилены; stale local collection и custom binary gate остаются |

Документ ниже сохраняет предыдущий аудит commit `19df199` как исторический
baseline. Его «активные» статусы и вывод о практически единственном
PostgreSQL adapter больше не описывают текущий HEAD.

<details>
<summary>Архив предыдущего аудита на commit 19df199</summary>

## 1. Резюме

Репозиторий реализует PostgreSQL workload generator в стиле TPC-C, но не полный
официальный тест TPC-C 5.11. Результаты намеренно маркируются как
`result_class: engineering` и не должны публиковаться как официальный `tpmC`.

После предыдущего контроля исправлены:

- экспоненциальное распределение think time по умолчанию;
- DB-профиль intentional rollback New-Order для валидных order lines;
- включение intentional rollback в throughput и response-time histogram;
- постоянный home district Stock-Level для стандартных 10 terminals/warehouse;
- `OL_DELIVERY_D = O_ENTRY_D` в initial population;
- NULL-only семантика carrier в consistency checks.

Эти исправления проходят unit tests и в основном корректны. После повторного
аудита устранены fail-open классификация intentional rollback, неполный учёт
measurement boundaries, игнорирование unknown YAML fields и пробел
post-import coverage для delivered `OL_AMOUNT` / `O_CARRIER_ID`.

Проверка на `19df199` уточнила, что stale-worker и malformed-artifact защита
закрывают исходные проблемы только частично. Кроме того, новые
soft-conformance и response-time statistics содержат отдельные ошибки,
описанные в активной секции.

Документ разделён на:

1. активные замечания;
2. принятые ограничения продукта;
3. исторические, полностью устранённые ошибки.

## 2. Область и методика

Проверены:

- schema, generator, initial population и loader;
- shared workflows пяти транзакций;
- PostgreSQL session/transaction adapter;
- terminals, pacing, retries и phase controller;
- worker artifacts, histograms и `mind-tpcc consolidate`;
- integrity checks и orchestration;
- изменения от `884230e` до `19df199`, включая PR #40-#47.

Выполнены проверки:

```text
./ya make tpcc/domain/ut tpcc/transactions/ut tpcc/runtime/ut \
  tpcc/metrics/ut tpcc/checks/ut
go test ./...
go run ./cmd/mind-tpcc validate --profile ../../docs/examples/profile.ydb.v1.yaml
```

Unit-test команды завершились успешно. Последняя команда ожидаемо завершилась
ошибкой strict YAML на `retry_ambiguous_commit`, подтвердив активное замечание
3.9. Статический и unit-test аудит не заменяет интеграционные тесты на
PostgreSQL, failure tests и длительный запуск SUT.

Критичность:

- **критическая** — результат может быть принят за корректный при фактически
  неверном выполнении или измерении;
- **высокая** — существенно искажает workload, throughput либо aggregate;
- **средняя** — ограниченный сценарий, несовместимость или недостаточная
  валидация;
- **низкая** — защитная проверка или пробел test coverage вне normal path.

## 3. Активные замечания

### 3.1. Не проверяются фактические пределы variability inputs

**Критичность: высокая для официального TPC-C.**

TPC-C §5.5.1.5 требует проверять на measurement interval:

- 0,9-1,1% rollback New-Order;
- среднее число order lines 9,5-10,5 и равномерность 5-15;
- 0,95-1,05% remote order lines;
- 14-16% remote Payment;
- 57-63% выбора customer по фамилии для Payment и Order-Status.

Generator использует требуемые вероятности, но worker artifacts не содержат
business-input counters для проверки фактической выборки.

### 3.2. Response-time reporting остаётся неполным

**Критичность: высокая для официального TPC-C.**

Частично улучшено: worker histogram теперь хранит exact `min_recorded`,
`max_recorded` и `sum_values`; consolidate публикует `min` / `max` / `avg`
вместе с p50/p90/p95/p99 в `aggregate.json` (`response_time_*`) и
`summary.txt` ([histogram.h](../tpcc/metrics/histogram.h),
[artifacts.cpp](../tpcc/dbms/pgsql/artifacts.cpp),
[merge.go](../mind/internal/histogram/merge.go),
[consolidate.go](../mind/internal/consolidate/consolidate.go)).

По-прежнему отсутствуют:

- menu response time;
- отдельные interactive/deferred Delivery metrics;
- проверка допустимых p90;
- required frequency distributions и графики §5.6.

Reported throughput также не truncates до нуля decimal places, как требует
TPC-C §5.4.4; для engineering metric сохранение дробной части допустимо.

### 3.3. Initial population остаётся частично несовместимым с §4.3

**Критичность: высокая для официального TPC-C.**

Остаются:

- `RandomAString` генерирует только `a-z`, а не alphanumeric
  ([strings.h](../tpcc/generator/strings.h));
- C-Load/C-Run заданы compile-time constants вместо случайного выбора для
  конкретных population/run
  ([constants.h](../tpcc/domain/constants.h),
  [rng.h](../tpcc/domain/rng.h)).

Post-import проверки delivered `OL_AMOUNT = 0.00` и carrier `[1..10]` добавлены
в 5.18; generator создаёт эти значения корректно.

### 3.4. Stale artifacts прежней попытки того же run принимаются

**Критичность: высокая. Частично исправленное прежнее замечание.**

Consolidator теперь отвергает unexpected worker names и проверяет `run_id`,
instance, run-config hash и warehouse assignment. Однако эти значения
совпадают у повторных попыток одного run:

- `GenerateRunID` выдаёт один и тот же ID для profile в течение дня
  ([config.go:345-349](../mind/internal/config/config.go#L345-L349));
- remote instance directory перед start не очищается;
- supervisor может принять уже существующий finalized manifest;
- instance nonce из новой попытки не сопоставляется между supervision,
  collection и consolidation.

Старый `result.json` от предыдущей попытки того же run может быть принят как
актуальный. Нужна сквозная проверка ожидаемого process/instance nonce либо
атомарная очистка/staging instance directory перед каждым launch.

### 3.5. Неполный worker result всё ещё обрабатывается fail-open

**Критичность: высокая. Частично исправленное прежнее замечание.**

Присутствующие counters/histograms теперь валидируются строго, но допускаются:

- отсутствующие или `null` counters;
- отсутствующие или `null` histograms;
- отсутствующий либо неверного типа `exit_status`;
- отрицательные counters;
- ненулевые completed counters без соответствующего response-time histogram;
- несовпадение `histogram.total_count` с completed count transaction type.

Связанный код:

- [consolidate.go:101-121](../mind/internal/consolidate/consolidate.go#L101-L121);
- [consolidate.go:207-250](../mind/internal/consolidate/consolidate.go#L207-L250).

Такой artifact способен дать `workers_complete=true` и throughput без
response-time data. Обязательные measurement fields и их перекрёстные
инварианты должны проверяться до merge.

### 3.6. Microsecond histogram содержит только millisecond precision

**Критичность: высокая для точности engineering reporting. Новая ошибка,
проявившаяся после добавления min/max/avg.**

Terminal сначала округляет `latency` и `latencyFull` до
`std::chrono::milliseconds`, после чего при `unit: us` преобразует уже
округлённое значение обратно в microseconds:

- [terminal.cpp:223-245](../tpcc/dbms/pgsql/terminal.cpp#L223-L245);
- [terminal.h:149-168](../tpcc/dbms/pgsql/terminal.h#L149-L168).

Default orchestrated histogram использует `us`, но фактическая гранулярность
остаётся 1000 us. Новые `min`, `max`, `avg` и percentiles выглядят точнее, чем
исходное измерение; sub-millisecond latency превращается в zero.

Нужно передавать исходные microseconds в stats либо честно публиковать unit
`ms`.

### 3.7. Soft conformance checks дают ложные verdicts

**Критичность: высокая для нового `tpcc_settings_conformant`.**

Проверка параметров запуска содержит несколько ошибок:

- `measurementMs <= 0` не добавляет deviation из-за условия
  `measurementMs > 0 && measurementMs < 120m`;
- New-Order ошибочно задан minimum 45%, хотя §5.2.3 не устанавливает для него
  minimum;
- keying time и mean think time сравниваются на точное равенство defaults,
  хотя TPC-C задаёт минимальные значения и допускает большие;
- structural validation принимает zero/negative phase durations.

Ссылки:

- [conformance.go:7-17](../mind/internal/config/conformance.go#L7-L17);
- [conformance.go:34-82](../mind/internal/config/conformance.go#L34-L82);
- [validate.go:128-150](../mind/internal/validate/validate.go#L128-L150).

В результате `tpcc_settings_conformant` может быть как ложноположительным, так
и ложноотрицательным. Это informational feature, но его status должен
вычисляться по фактическим требованиям TPC-C.

### 3.8. Новые histogram extrema и average валидируются недостаточно

**Критичность: средняя/высокая. Новая ошибка reporting feature.**

При JSON decode отсутствующие `min_recorded`, `max_recorded` и `sum_values`
становятся нулями. Для непустого histogram такой payload может пройти текущие
проверки, если buckets/`total_count` согласованы, и aggregate опубликует
`min=0`, `max=0`, `avg=0` для ненулевого распределения.

Кроме того:

- empty histogram не обязан иметь нулевые extrema;
- merge переносит `max_recorded` из empty histogram;
- нет checked arithmetic для bucket/count/sum merges.

Ссылки:

- [merge.go:43-81](../mind/internal/histogram/merge.go#L43-L81);
- [merge.go:84-125](../mind/internal/histogram/merge.go#L84-L125);
- [merge.go:189-211](../mind/internal/histogram/merge.go#L189-L211).

### 3.9. Histogram profile и официальный пример конфигурации некорректны

**Критичность: средняя.**

`mind-tpcc validate` не проверяет оставшиеся histogram knobs:

- неизвестный `unit` проходит control-host validation и отклоняется только
  worker;
- `highest <= 0` молча заменяется default вместо structural error.

Дополнительно [profile.ydb.v1.yaml](examples/profile.ydb.v1.yaml) содержит
`retry_ambiguous_commit`, которого нет в `profile.RetryPolicy`. После включения
`KnownFields(true)` официальный пример больше не проходит `mind-tpcc validate`.

Ссылки:

- [profile.go:126-148](../mind/internal/profile/profile.go#L126-L148);
- [validate.go:98-120](../mind/internal/validate/validate.go#L98-L120);
- [profile.ydb.v1.yaml:86-91](examples/profile.ydb.v1.yaml#L86-L91).

## 4. Принятые ограничения и отклонения

Эти пункты не являются планируемыми исправлениями portable-tpcc. Они остаются
отклонениями от официальной TPC-C, но соответствуют внутренней
[specification.md](specification.md).

### 4.1. Синхронная Delivery

Delivery выполняется inline в terminal. Deferred queue, Delivery result log и
80-second deferred completion metric не реализуются. Решение принято для
фокусировки workload на DBMS transaction execution.

### 4.2. Отсутствие полноценного Remote Terminal Emulator

Проект не моделирует TPC-C menus/screens и end-user response time. Latency
измеряется на workload-client boundary. Поэтому официальные RTE requirements и
menu metrics неприменимы к engineering result.

### 4.3. Отсутствие встроенных ACID certification tests

Atomicity/isolation/durability, power-loss и failure procedures не встроены.
Каждый DBMS adapter при этом обязан обеспечивать:

- atomic commit/rollback;
- достаточную isolation для concurrent home/remote transactions;
- корректную классификацию commit outcome;
- сохранение logical consistency conditions.

Подтверждение этих свойств выполняется внешними средствами.

### 4.4. Детерминированные synthetic initial dates

`C_SINCE`, `H_DATE` и `O_ENTRY_D` привязаны к fixed reference epoch и
детерминированному семидневному window вместо wall clock loader host. Это
принято ради стабильной population между hosts, retries и adapters.

### 4.5. Внешний контроль checkpoints

Управление transaction log, checkpoint/recovery mechanisms и проверка
checkpoint interval остаются ответственностью DBMS/operator из-за различий
между СУБД.

### 4.6. Отсутствие версионирования worker artifacts

Worker artifact schema и совместимость смешанных версий намеренно не
версионируются как отдельный протокол. Один run предполагает согласованный
набор бинарных артефактов из одной текущей версии portable-tpcc.

Ответственность разделена следующим образом:

- оператор обязан своевременно вызвать `deploy`, в частности после обновления
  или смены версии и до `run` / запуска workload; `run` не заливает бинарники
  сам, а только проверяет, что явный `deploy` уже выполнен;
- `deploy` обязан применить на всех назначенных hosts актуальный набор бинарных
  артефактов из текущей версии (profile-scoped shared path под `remote_root`);
- смешивание worker binaries разных версий в одном run не поддерживается и
  считается ошибкой эксплуатации, а не форматом, который должен согласовывать
  consolidator.

### 4.7. Sustained operation и full disclosure

Доказательство восьмичасовой устойчивости, подготовка Full Disclosure Report,
price/tpmC, availability date и обязательные disclosure-графики не являются
гарантиями, которые tooling должен формировать самостоятельно.

Оператор и организация, проводящая тест, отвечают за:

- внешний sustained-load protocol и доказательство §5.5.1.2;
- сбор инфраструктурных и коммерческих данных;
- построение и публикацию FDR/графиков §5.6/§8;
- независимую TPC verification.

Portable-tpcc предоставляет engineering workload и исходные artifacts, но не
сертификационный процесс и не официальный TPC-C result.

## 5. Исторические полностью устранённые ошибки

Ниже сохранены первоначальные замечания, которые больше не воспроизводятся на
commit `19df199`.

Нумерация сохраняет исторические IDs. Пункты 5.15, 5.16 и 5.20 исключены из
этой секции после повторной проверки: соответствующие исправления оказались
частичными и описаны как активные замечания 3.4, 3.5 и 3.7.

### 5.1. Intentional rollback не учитывался в tpmC и RT

**Устранено:** intentional rollback теперь записывает latency,
`*_user_aborted`, входит в worker throughput и aggregate
([terminal.h](../tpcc/dbms/pgsql/terminal.h),
[artifacts.cpp](../tpcc/dbms/pgsql/artifacts.cpp),
[consolidate.go](../mind/internal/consolidate/consolidate.go)).

Fail-open classification устранён в 5.13; measurement boundaries устранены в 5.14.

### 5.2. Валидные строки rollback New-Order не выполняли DB-профиль

**Устранено в `0467d46`:** для валидных позиций выполняются ITEM, STOCK update
и ORDER-LINE insert до lookup unused item
([new_order.cpp:122-225](../tpcc/transactions/new_order.cpp#L122-L225)).

### 5.3. Think time был постоянным

**Устранено по умолчанию:** `SampleThinkTimeMs` реализует
`-log(r) * mean` с допустимым cap `10 * mean`
([think_time.h](../tpcc/domain/think_time.h)). Constant behavior оставлено
только как явно выбранный compatibility mode.

### 5.4. Stock-Level выбирал новый district для каждой транзакции

**Устранено для стандартной конфигурации:** terminal получает постоянный home
district, который использует Stock-Level
([runner.cpp](../tpcc/dbms/pgsql/runner.cpp),
[stock_level.cpp](../tpcc/transactions/stock_level.cpp)).

### 5.5. OL_DELIVERY_D initial order не совпадал с O_ENTRY_D

**Устранено:** delivered lines получают `order.EntryUnix`; добавлен post-import
check `post_import.ol_delivery_eq_entry`
([populate.cpp](../tpcc/generator/populate.cpp),
[load_batch.cpp](../tpcc/dbms/pgsql/load_batch.cpp),
[check.cpp](../tpcc/dbms/pgsql/check.cpp)).

### 5.6. O_ALL_LOCAL был неверен при одном warehouse

**Устранено в `4d44f21`:** remote branch недоступна при одном warehouse
([new_order.cpp:58-69](../tpcc/transactions/new_order.cpp#L58-L69)).

### 5.7. Clock calibration содержала нулевую заглушку

**Устранено в `45ce1f6`:** worker измеряет PostgreSQL clock offset/RTT, проверяет
skew budget, а consolidator проверяет calibration всех expected workers
([clock_calibration.cpp](../tpcc/dbms/pgsql/clock_calibration.cpp)).

При `max_clock_skew_ms <= 0` проверка явно отключена; standalone run не является
проверенным multi-host режимом.

### 5.8. Aggregate считал отсутствие checks успешным

**Устранено в `56018f2`/`04b87dd`:** обязательные reports проверяются
fail-closed, причины публикуются в `integrity_errors`
([consolidate.go](../mind/internal/consolidate/consolidate.go)).

### 5.9. Money/rate проходили через double

**Устранено в `3e6cede`:** PostgreSQL adapter использует exact text parsing
`GetMoney`/`GetRate`
([query_result.h](../tpcc/dbms/pgsql/query_result.h),
[tpcc_session.cpp](../tpcc/dbms/pgsql/tpcc_session.cpp)).

### 5.10. Конкурентная Delivery выбирала oldest order без блокировки

**Устранено в `2cab3ab`:** выбор использует `FOR UPDATE`, а delete проверяет
affected rows и возвращает retryable abort при конфликте
([tpcc_session.cpp](../tpcc/dbms/pgsql/tpcc_session.cpp)).

### 5.11. Legacy import не создавал customer-name index

**Устранено в `7b759a2` (позже вынесено в роль `indexes`):** после load
`EnsureIndexes` / `CreateIndexes` создаёт индекс, затем
`EnsureStatistics` / `ANALYZE`
([init.cpp](../tpcc/dbms/pgsql/init.cpp),
[pg_admin_adapter.cpp](../tpcc/dbms/pgsql/pg_admin_adapter.cpp)).

### 5.12. Consistency checks использовали epsilon и carrier 0 как NULL

**Устранено:** money сравнивается точно через `IS DISTINCT FROM`; условия
3.3.2.5/7 используют только `O_CARRIER_ID IS NULL`
([check.cpp:192-429](../tpcc/dbms/pgsql/check.cpp#L192-L429)).

### 5.13. Ошибка могла быть засчитана как intentional rollback New-Order

**Устранено:** unused-item path принимает только ITEM `not-found`
(`EErrorClass::Integrity`), проверяет `TCommitResult` rollback через
`ThrowIfRollbackFailed` и лишь затем бросает `TUserAbortedException`.
Неуспешный или повторный `Rollback()` в PostgreSQL adapter возвращает
`OutcomeUnknown`, а не ложный `RolledBack`
([new_order.cpp](../tpcc/transactions/new_order.cpp),
[workflow_util.h](../tpcc/transactions/workflow_util.h),
[tpcc_session.cpp](../tpcc/dbms/pgsql/tpcc_session.cpp)).

### 5.14. Response time мог пересекать measurement boundary

**Устранено:** учёт §5.4.2 опирается на абсолютные timestamps phase schedule:

- `startWall` фиксируется в тот же момент, что и начало `latencyFull`
  (до inflight-slot wait);
- метрики записываются только если
  `CompletelyWithinMeasurement(startWall, endWall)`;
- `MayAdmit`/`MayRecord` используют schedule, а не только Tick()-published
  phase enum, поэтому lag main loop до 50 ms не допускает late admission и не
  пропускает начало measurement
  ([phase_controller.h](../tpcc/runtime/phase_controller.h),
  [terminal.cpp](../tpcc/dbms/pgsql/terminal.cpp)).

### 5.17. Unknown YAML fields не отклонялись

**Устранено в `e344bf0`/`a997ab8`:** `profile.Parse` использует
`yaml.NewDecoder` с `KnownFields(true)`, поэтому опечатки вроде
`think_time_distribtion` отклоняются на parse time
([profile.go](../mind/internal/profile/profile.go)).

### 5.18. Post-import suite не проверял delivered OL_AMOUNT и carrier range

**Устранено в `e344bf0`/`a997ab8`:** добавлены checks
`post_import.ol_amount_delivered` (`OL_AMOUNT = 0.00` для initial delivered
lines) и `post_import.o_carrier_id_range` (`O_CARRIER_ID` в `[1..10]` для
delivered orders)
([check.cpp](../tpcc/dbms/pgsql/check.cpp),
[catalog.cpp](../tpcc/checks/catalog.cpp)).

Generator уже создавал корректные значения; это был defensive coverage gap.
Оставшиеся замечания initial population (a-string alphabet и C-Load/C-Run)
описаны в активном пункте 3.3.

### 5.19. Histogram settings частично игнорировались

**Устранено:** `runtime.histogram.lowest` и `significant_figures` удалены из
profile/run-config schema. Они читались и публиковались как effective
settings, но `THistogram` layout `linear_exp` использует только `unit` и
`highest` (`max_value`); `hdr_till` — implementation default (4096, capped by
`highest`), публикуемый в worker artifacts.

- profile YAML с этими полями отклоняется через `KnownFields`;
- worker run-config с этими полями отклоняется при parse;
- `settings.histogram` в `result.json` содержит только
  `unit` / `highest` / `layout` / `hdr_till` / `max_value`
  ([workload_config.h](../tpcc/domain/workload_config.h),
  [run_config.cpp](../tpcc/dbms/pgsql/run_config.cpp),
  [artifacts.cpp](../tpcc/dbms/pgsql/artifacts.cpp),
  [profile.go](../mind/internal/profile/profile.go),
  [config.go](../mind/internal/config/config.go)).

## 6. Итоговая оценка

| Область | Оценка |
| --- | --- |
| DB workload core | Основные пять транзакций реализованы; intentional rollback fail-closed по ITEM not-found и подтверждённому rollback |
| Initial population | Cardinalities и delivery timestamps корректны; synthetic dates приняты; a-string и C constants остаются; post-import coverage для delivered amount/carrier добавлена |
| Runtime | Think time, Stock-Level и measurement boundaries исправлены для default |
| Consolidation | Unexpected worker names и identity/config mismatch отвергаются; same-run stale attempts и отсутствующие measurement fields остаются |
| Delivery | Синхронная модель — принятое отклонение; конкурентная обработка исправлена |
| RTE / ACID / checkpoints / disclosure | Внешние или вне области по принятому решению |
| Launch-parameter checks | Soft TPC-C 5.11 status присутствует, но имеет false-positive/false-negative cases из 3.7 |
| Reporting | Min/max/avg и percentiles добавлены, но precision и metadata validation требуют исправления |
| DBMS adapters | Практически реализован только PostgreSQL |

Повторный аудит не обнаружил возврата historical defects 5.1-5.14 и 5.17-5.19
или deadlock/data-corruption регрессий в ITEM/STOCK locking, Stock-Level
binding, think-time sampling, timestamp population и carrier checks.

При этом stale-attempt и malformed-measurement fixes оказались частичными
(3.4-3.5), а новые soft-conformance и response-time statistics добавили
ошибки 3.6-3.9. Сравнение результатов до и после
`884230e`/`a997ab8`/`19df199` требует учитывать ожидаемые изменения workload:
tpmC включает intentional rollback, rollback New-Order создаёт полную
DB-нагрузку, default think-time distribution экспоненциальное, а response-time
artifacts теперь содержат extrema и average.

</details>
