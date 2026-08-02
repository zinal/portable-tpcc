# Анализ соответствия реализации требованиям TPC-C 5.11

Статус: повторный анализ реализации на commit `a997ab8`.

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
measurement boundaries, слияние лишних/stale worker artifacts при consolidate,
fail-open обработка повреждённых counters/histograms, игнорирование unknown
YAML fields и пробел post-import coverage для delivered `OL_AMOUNT` /
`O_CARRIER_ID`.

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
- worker artifacts, histograms и `tpccctl consolidate`;
- integrity checks и orchestration;
- изменения от `4d44f21` до `a997ab8`.

Выполнены проверки:

```text
./ya make tpcc/domain/ut tpcc/transactions/ut tpcc/checks/ut
go test ./...
```

Обе команды завершились успешно. Статический и unit-test аудит не заменяет
интеграционные тесты на PostgreSQL, failure tests и длительный запуск SUT.

Критичность:

- **критическая** — результат может быть принят за корректный при фактически
  неверном выполнении или измерении;
- **высокая** — существенно искажает workload, throughput либо aggregate;
- **средняя** — ограниченный сценарий, несовместимость или недостаточная
  валидация;
- **низкая** — защитная проверка или пробел test coverage вне normal path.

## 3. Активные замечания

### 3.1. Нет TPC-C conformance validation параметров запуска

**Критичность: высокая для официального TPC-C; допустимо для engineering
profiles.**

Конфигурация допускает:

- не 10 terminals/warehouse;
- mix, не обеспечивающий минимальные доли TPC-C;
- отключённый pacing;
- compatibility/constant think time;
- произвольные keying/think means;
- measurement interval менее 120 минут.

Ссылки:

- [validate.go](../tools/tpccctl/internal/validate/validate.go);
- [defaults.go](../tools/tpccctl/internal/config/defaults.go);
- [specification.md](specification.md).

В частности, при `terminals_per_warehouse > 10` `HomeDistrictId` циклически
повторяет districts, и уникальность `(W_ID, D_ID)` Stock-Level из §2.8.1.1
нарушается.

### 3.2. Не проверяются фактические пределы variability inputs

**Критичность: высокая для официального TPC-C.**

TPC-C §5.5.1.5 требует проверять на measurement interval:

- 0,9-1,1% rollback New-Order;
- среднее число order lines 9,5-10,5 и равномерность 5-15;
- 0,95-1,05% remote order lines;
- 14-16% remote Payment;
- 57-63% выбора customer по фамилии для Payment и Order-Status.

Generator использует требуемые вероятности, но worker artifacts не содержат
business-input counters для проверки фактической выборки.

### 3.3. Response-time reporting остаётся неполным

**Критичность: высокая для официального TPC-C.**

Aggregate публикует p50/p90/p95/p99, но не:

- average и maximum по каждому transaction type;
- menu response time;
- отдельные interactive/deferred Delivery metrics;
- проверку допустимых p90;
- required frequency distributions и графики §5.6.

Raw histogram не хранит сумму значений, поэтому exact average восстановить
невозможно. Reported throughput также не truncates до нуля decimal places, как
требует TPC-C §5.4.4; для engineering metric сохранение дробной части допустимо.

### 3.4. Histogram settings частично игнорируются

**Критичность: средняя. Ранее не зафиксированный internal defect.**

`runtime.histogram.lowest` и `significant_figures` читаются и публикуются как
effective settings, но фактический `THistogram` layout их не использует:

- [workload_config.h](../tpcc/domain/workload_config.h);
- [artifacts.cpp:237-245](../tpcc/dbms/pgsql/artifacts.cpp#L237-L245).

Два профиля с разными значениями могут создавать одинаковые buckets, но
aggregate будет утверждать, что применялись разные настройки.

### 3.5. Initial population остаётся частично несовместимым с §4.3

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

### 3.6. Нет доказательства sustained operation и полного disclosure

**Критичность: высокая для официального TPC-C.**

Остаются:

- отсутствие доказательства восьмичасовой устойчивости §5.5.1.2;
- отсутствие FDR, price/tpmC, availability date;
- отсутствие обязательных графиков §5.6/§8.

Checkpoint control относится к принятым ограничениям и описан отдельно ниже.

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
  или смены версии и до запуска workload;
- `deploy` обязан применить на всех назначенных hosts актуальный набор бинарных
  артефактов из текущей версии;
- смешивание worker binaries разных версий в одном run не поддерживается и
  считается ошибкой эксплуатации, а не форматом, который должен согласовывать
  consolidator.

## 5. Исторические полностью устранённые ошибки

Ниже сохранены первоначальные замечания, которые больше не воспроизводятся на
commit `a997ab8`.

### 5.1. Intentional rollback не учитывался в tpmC и RT

**Устранено:** intentional rollback теперь записывает latency,
`*_user_aborted`, входит в worker throughput и aggregate
([terminal.h](../tpcc/dbms/pgsql/terminal.h),
[artifacts.cpp](../tpcc/dbms/pgsql/artifacts.cpp),
[consolidate.go](../tools/tpccctl/internal/consolidate/consolidate.go)).

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
([consolidate.go](../tools/tpccctl/internal/consolidate/consolidate.go)).

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

**Устранено в `7b759a2`:** общий `ImportSync` вызывает `CreateIndexes` перед
`ANALYZE` ([import.cpp](../tpcc/dbms/pgsql/import.cpp)).

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

### 5.15. Consolidator объединял лишние или stale worker artifacts

**Устранено:** до слияния counters/histograms consolidator отвергает каталоги
вне expected worker set и проверяет identity каждого `result.json`:

- `run_id` совпадает с consolidate run;
- `instance` совпадает с именем каталога;
- `run_config_sha256` совпадает с hash распределённого `run-config.json`;
- `assignment.warehouse_ranges` совпадает с assignment из run-config.

`Materialize` больше не перезаписывает существующий `run-config.json`, поэтому
hash остаётся стабильным между deploy и consolidate
([consolidate.go](../tools/tpccctl/internal/consolidate/consolidate.go),
[orchestrator.go](../tools/tpccctl/internal/orchestrator/orchestrator.go)).

### 5.16. Повреждённые counters и histograms обрабатывались fail-open

**Устранено в `e344bf0`/`a997ab8`:** consolidate отклоняет некорректный тип
counter и повреждённый histogram payload вместо частичного aggregate.
`histogram.Validate` требует `layout`, `unit`, ожидаемую длину buckets для
`linear_exp` и равенство `total_count` сумме buckets; `Merge` требует полной
совместимости layout/unit/параметров/длины buckets
([consolidate.go](../tools/tpccctl/internal/consolidate/consolidate.go),
[merge.go](../tools/tpccctl/internal/histogram/merge.go)).

### 5.17. Unknown YAML fields не отклонялись

**Устранено в `e344bf0`/`a997ab8`:** `profile.Parse` использует
`yaml.NewDecoder` с `KnownFields(true)`, поэтому опечатки вроде
`think_time_distribtion` отклоняются на parse time
([profile.go](../tools/tpccctl/internal/profile/profile.go)).

### 5.18. Post-import suite не проверял delivered OL_AMOUNT и carrier range

**Устранено в `e344bf0`/`a997ab8`:** добавлены checks
`post_import.ol_amount_delivered` (`OL_AMOUNT = 0.00` для initial delivered
lines) и `post_import.o_carrier_id_range` (`O_CARRIER_ID` в `[1..10]` для
delivered orders)
([check.cpp](../tpcc/dbms/pgsql/check.cpp),
[catalog.cpp](../tpcc/checks/catalog.cpp)).

Generator уже создавал корректные значения; это был defensive coverage gap.
Оставшиеся замечания initial population (a-string alphabet и C-Load/C-Run)
описаны в активном пункте 3.5.

## 6. Итоговая оценка

| Область | Оценка |
| --- | --- |
| DB workload core | Основные пять транзакций реализованы; intentional rollback fail-closed по ITEM not-found и подтверждённому rollback |
| Initial population | Cardinalities и delivery timestamps корректны; synthetic dates приняты; a-string и C constants остаются; post-import coverage для delivered amount/carrier добавлена |
| Runtime | Think time, Stock-Level и measurement boundaries исправлены для default |
| Consolidation | Unexpected/stale workers отвергаются; identity/config валидируются до merge; повреждённые histogram/counter payloads отклоняются fail-closed |
| Delivery | Синхронная модель — принятое отклонение; конкурентная обработка исправлена |
| RTE / ACID / checkpoints | Внешние или вне области по принятому решению |
| Reporting | Engineering artifacts, не официальный TPC-C FDR |
| DBMS adapters | Практически реализован только PostgreSQL |

Повторный аудит не обнаружил deadlock или data-corruption регрессий в новых
ITEM/STOCK locking, Stock-Level binding, think-time sampling, timestamp
population и carrier checks. Fail-open classification intentional rollback
устранён (5.13); measurement-boundary учёт §5.4.2 исправлен (5.14);
слияние лишних/stale worker artifacts устранено (5.15); fail-open counters/
histograms устранены (5.16); unknown YAML fields отклоняются (5.17);
post-import amount/carrier checks добавлены (5.18). Сравнение результатов до
и после `884230e`/`a997ab8` требует учитывать ожидаемые изменения workload:
tpmC увеличивается примерно на долю intentional rollback, rollback New-Order
создаёт полную DB-нагрузку, а default think-time distribution теперь
экспоненциальное.
