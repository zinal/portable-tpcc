# Анализ соответствия реализации требованиям TPC-C 5.11

Статус: повторный анализ реализации на commit `19df199`.

Основа сравнения: [TPC Benchmark C Standard Specification, Revision
5.11](https://www.tpc.org/TPC_Documents_Current_Versions/pdf/tpc-c_v5.11.0.pdf).

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
post-import coverage для delivered `OL_AMOUNT` / `O_CARRIER_ID`. Позднее
закрыта fail-open обработка неполного worker result: отсутствующие
measurement fields и несогласованные counters/histograms больше не
интерпретируются как нули.

Проверка на `19df199` уточнила, что stale-worker защита закрывает исходную
проблему только частично. Кроме того, новые soft-conformance и
response-time statistics содержат отдельные ошибки, описанные в активной
секции.

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

Ниже сохранены первоначальные замечания, которые больше не воспроизводятся.
Аудит на commit `19df199` закрыл 5.1-5.14 и 5.17-5.19; 5.16 закрыт позднее
fail-closed проверкой measurement fields.

Нумерация сохраняет исторические IDs. Пункты 5.15 и 5.20 исключены из
этой секции после повторной проверки: соответствующие исправления оказались
частичными и описаны как активные замечания 3.4 и 3.7. Пункт 5.16 возвращён
сюда после fail-closed проверки measurement fields.

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

### 5.16. Неполный worker result обрабатывался fail-open

**Устранено:** consolidator требует integer `exit_status`, object `counters`
и object `histograms` до merge. Отсутствующие или `null` поля, отрицательные
counters, ненулевой completed count (`*_ok + *_user_aborted`) без
response-time histogram и `histogram.total_count`, не равный completed count,
дают ошибку, а не нули. `AllowIncomplete` по-прежнему разрешает non-zero
`exit_status` / отсутствующий `result.json`, но не malformed measurement
payload
([consolidate.go](../mind/internal/consolidate/consolidate.go)).

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
| Consolidation | Unexpected worker names и identity/config mismatch отвергаются; incomplete measurement fields и counter/histogram mismatches отвергаются; same-run stale attempts остаются |
| Delivery | Синхронная модель — принятое отклонение; конкурентная обработка исправлена |
| RTE / ACID / checkpoints / disclosure | Внешние или вне области по принятому решению |
| Launch-parameter checks | Soft TPC-C 5.11 status присутствует, но имеет false-positive/false-negative cases из 3.7 |
| Reporting | Min/max/avg и percentiles добавлены, но precision и metadata validation требуют исправления |
| DBMS adapters | Практически реализован только PostgreSQL |

Повторный аудит не обнаружил возврата historical defects 5.1-5.14, 5.16 и 5.17-5.19
или deadlock/data-corruption регрессий в ITEM/STOCK locking, Stock-Level
binding, think-time sampling, timestamp population и carrier checks.

При этом stale-attempt fix оказался частичным (3.4), а новые
soft-conformance и response-time statistics добавили
ошибки 3.6-3.9. Сравнение результатов до и после
`884230e`/`a997ab8`/`19df199` требует учитывать ожидаемые изменения workload:
tpmC включает intentional rollback, rollback New-Order создаёт полную
DB-нагрузку, default think-time distribution экспоненциальное, а response-time
artifacts теперь содержат extrema и average.
