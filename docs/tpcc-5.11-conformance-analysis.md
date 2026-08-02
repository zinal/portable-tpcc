# Анализ соответствия реализации требованиям TPC-C 5.11

Статус: повторный анализ реализации на commit `4d44f21`.

Основа сравнения: [TPC Benchmark C Standard Specification, Revision
5.11](https://www.tpc.org/TPC_Documents_Current_Versions/pdf/tpc-c_v5.11.0.pdf).

## 1. Резюме

Текущий репозиторий реализует полезный PostgreSQL workload generator в стиле
TPC-C, но не полный официальный тест TPC-C 5.11. Получаемый throughput нельзя
публиковать как валидный `tpmC`.

Это соответствует заявленному позиционированию проекта:

- [README.md](../README.md) запрещает называть результаты официальными без
  требуемой TPC-верификации;
- [specification.md](specification.md) определяет результат как
  `result_class: engineering`, разрешает менять параметры workload и не
  выполняет проверку соответствия конкретной редакции TPC-C.

После первичного анализа устранены замечания по `O_ALL_LOCAL`, exact-decimal
чтениям PostgreSQL, legacy index, конкурентной Delivery, clock calibration и
fail-closed integrity. Денежная часть consistency checks также исправлена.
Новых регрессий, вызванных этими изменениями, не обнаружено.

Основные остающиеся блокирующие причины:

1. неверный учёт штатных rollback New-Order в throughput и response time;
2. сокращённый профиль rollback New-Order;
3. несоответствующая спецификации модель Stock-Level terminals;
4. нарушения initial population, кроме принятого решения о синтетических
   начальных датах;
5. неполная проверка валидности measurement interval и отчётность.

Синхронная Delivery, отсутствие полноценного RTE и встроенных ACID/checkpoint
tests являются теперь явно зафиксированными границами продукта. Они остаются
отклонениями от официальной TPC-C 5.11, но не считаются дефектами относительно
внутренней [specification.md](specification.md).

## 2. Область и методика

Проверены:

- схема и начальная загрузка PostgreSQL;
- генерация данных и transaction inputs;
- workflows пяти транзакций;
- terminal runtime, pacing, retry и фазы запуска;
- сбор и консолидация метрик;
- consistency checks;
- orchestration и итоговые артефакты.

Анализ является статическим. Он не заменяет обязательные испытания TPC-C на
полностью сконфигурированном SUT, в частности ACID, failure и sustained-load
tests.

Критичность в документе означает:

- **критическая** — делает результат функционально неверным или самостоятельно
  блокирует соответствие TPC-C;
- **высокая** — нарушает обязательный профиль, population или правила
  измерения;
- **средняя** — локальный дефект либо риск, который не всегда проявляется в
  итоговом результате.

Статус каждого исходного замечания:

- **Остаётся** — код по-прежнему содержит описанную проблему;
- **Устранено** — замечание сохранено для истории, но исправлено в текущем коде;
- **Принятое отклонение** — несоответствие официальной TPC-C осознанно принято
  как граница engineering workload;
- **Частично устранено** — исправлена только часть исходного замечания.

## 3. Критические замечания исходного аудита

### 3.1. Неверно считается tpmC

**Статус: присутствует на момент анализа.**

TPC-C §5.1.2 и §5.4.2 требуют включать в MQTh штатные 1% New-Order,
завершившиеся rollback из-за неверного item. Их response time также входит в
статистику New-Order.

Реализация учитывает такие транзакции отдельно как `UserAborted`:

- [new_order.cpp:143-145](../tpcc/transactions/new_order.cpp#L143-L145);
- [terminal.cpp:186-189](../tpcc/dbms/pgsql/terminal.cpp#L186-L189);
- [terminal.cpp:242-244](../tpcc/dbms/pgsql/terminal.cpp#L242-L244).

Но throughput вычисляется только из `new_order_ok`:

- [artifacts.cpp:166-188](../tpcc/dbms/pgsql/artifacts.cpp#L166-L188);
- [consolidate.go:141-146](../tools/tpccctl/internal/consolidate/consolidate.go#L141-L146).

Следствия:

- throughput систематически занижен приблизительно на 1%;
- штатные rollback отсутствуют в response-time histogram;
- счётчик `UserAborted` не экспортируется в worker `result.json`, где
  публикуются только `OK`, `Failed` и `Retried`
  ([artifacts.cpp:169-184](../tpcc/dbms/pgsql/artifacts.cpp#L169-L184));
- невозможно проверить ограничения response time на полном наборе New-Order.

Дополнительно §5.4.2 разрешает учитывать только транзакции, response time которых
полностью измерен внутри measurement interval. Флаг `recordMetrics` фиксируется
до keying time
([terminal.cpp:141-158](../tpcc/dbms/pgsql/terminal.cpp#L141-L158)), а результат
записывается после завершения транзакции
([terminal.cpp:228-235](../tpcc/dbms/pgsql/terminal.cpp#L228-L235)).
Проверки, что начало и окончание response time находятся внутри одного
measurement interval, нет.

### 3.2. Rollback-профиль New-Order выполняется не полностью

**Статус: присутствует на момент анализа.**

По TPC-C §2.4.2.3 все валидные позиции должны быть обработаны до последней
неверной позиции. Эффекты этой работы затем откатываются общей транзакцией.
Знание о неверном item разрешает пропустить действия только для самого
неверного item.

Реализация:

1. исключает invalid item из запроса ITEM
   ([new_order.cpp:123-129](../tpcc/transactions/new_order.cpp#L123-L129));
2. сразу выполняет rollback
   ([new_order.cpp:143-145](../tpcc/transactions/new_order.cpp#L143-L145));
3. не доходит до обработки stock и order lines
   ([new_order.cpp:148-214](../tpcc/transactions/new_order.cpp#L148-L214)).

Таким образом, около 1% New-Order не создают обязательную read/write-нагрузку
для валидных позиций. Invalid item также не вызывает предусмотренный профилем
ITEM `not-found`: rollback выполняется по заранее известному флагу
`HasInvalidItem`.

### 3.3. Delivery не соответствует обязательной deferred-модели

**Статус: принятое отклонение.**

TPC-C §2.7.2 требует:

- поставить Delivery в очередь;
- вернуть управление терминалу независимо от завершения фактической доставки;
- выполнить доставку отдельно;
- записать durable result file с временем постановки, завершения и
  обработанными заказами.

В PostgreSQL-адаптере Delivery явно объявлена синхронной:

- [pg_capabilities.cpp:12](../tpcc/dbms/pgsql/pg_capabilities.cpp#L12);
- [terminal.cpp:223-230](../tpcc/dbms/pgsql/terminal.cpp#L223-L230);
- [delivery.cpp:24-117](../tpcc/transactions/delivery.cpp#L24-L117).

Отдельной очереди и Delivery result file нет. Из-за этого невозможно проверить
80-секундное ограничение deferred Delivery, долю skipped deliveries и
интерактивный response time Delivery. Согласно принятому решению, тест
производительности СУБД намеренно выполняет Delivery синхронно, inline в
терминале. Это зафиксировано во внутренней
[specification.md](specification.md) как сознательное отступление от TPC-C 5.11
и не планируется к устранению.

### 3.4. Отсутствует полноценный Remote Terminal Emulator

**Статус: принятое ограничение области.**

TPC-C требует menu cycle, input/output screens, menu response time и отображение
всех результатов транзакций. Реализация вызывает business workflow напрямую и
измеряет внутренний вызов.

Например, New-Order не формирует обязательные выходные данные:

- `total_amount`;
- `brand_generic`;
- полный набор customer, tax, item и stock output fields.

Часть нужных данных читается, но результат не используется
([new_order.cpp:85-214](../tpcc/transactions/new_order.cpp#L85-L214)).
Menu response time отсутствует. Поэтому собранная задержка не является полным
response time эмулированного пользователя по TPC-C §5.3.

Полноценный RTE не входит и не будет входить в область проекта: его реализация
выходит за рамки теста производительности СУБД. Метрики следует трактовать как
latency на границе workload client, а не как end-user response time TPC-C.

### 3.5. Отсутствуют обязательные ACID tests

**Статус: принятое ограничение области с обязательными требованиями к
адаптерам.**

Реализовано хорошее покрытие consistency conditions 3.3.2.1-12:

- [catalog.cpp:18-34](../tpcc/checks/catalog.cpp#L18-L34);
- [check.cpp:192-625](../tpcc/dbms/pgsql/check.cpp#L192-L625).

Однако отсутствуют:

- Atomicity tests из §3.2.2;
- consistency demonstration под нагрузкой из §3.3.3;
- Isolation tests из §3.4.2;
- crash, memory, power и durable-media tests из §3.5.4.

Проверка SQL-инвариантов после workload не заменяет эти испытания.

Встроенные процедуры ACID certification реализовывать не планируется. При этом
внутренняя спецификация требует от каждого DBMS-адаптера атомарного
commit/rollback, достаточной изоляции concurrent home/remote transactions и
корректной обработки неопределённого результата commit. Эти свойства должны
обеспечиваться механизмами выбранной СУБД и проверяться отдельно от
portable-tpcc.

### 3.6. Think time не имеет требуемого распределения

**Статус: устранено (default); опциональный режим совместимости сохранён.**

TPC-C §5.2.5.4 требует независимо выбирать think time из
отрицательно-экспоненциального распределения:

```text
Tt = -log(r) * mean
```

По умолчанию runtime сэмплирует think time по этой формуле с truncation на
`10 * mean` ([think_time.h](../tpcc/domain/think_time.h),
[terminal.cpp](../tpcc/dbms/pgsql/terminal.cpp)).

Опциональный режим `runtime.think_time_distribution: benchbase` (alias
`constant`) сохраняет прежнее поведение portable-tpcc: фиксированное ожидание,
равное настроенному среднему. Для соответствия TPC-C 5.11 этот режим включать
не следует.

### 3.7. Stock-Level не закреплён за district терминала

**Статус: присутствует на момент анализа.**

По TPC-C §2.8.1.1 каждому из десяти терминалов warehouse должна соответствовать
постоянная уникальная пара `(W_ID, D_ID)`.

В runtime терминалу передаётся только warehouse
([runner.cpp:329-349](../tpcc/dbms/pgsql/runner.cpp#L329-L349)), а Stock-Level
выбирает district заново для каждой транзакции:
[stock_level.cpp:26-31](../tpcc/transactions/stock_level.cpp#L26-L31).

Это нарушает профиль Stock-Level и распределение доступа по districts.

## 4. Замечания высокой критичности исходного аудита

### 4.1. Начальные даты не соответствуют спецификации

**Статус: принятое отклонение.**

TPC-C §4.3.3.1 требует, чтобы `C_SINCE`, `H_DATE` и `O_ENTRY_D` содержали текущие
дату и время ОС при population.

Генератор использует случайное время в пределах недели после фиксированной
эпохи 2020-01-01:

- [populate.h:9-11](../tpcc/generator/populate.h#L9-L11);
- [populate.cpp:26-29](../tpcc/generator/populate.cpp#L26-L29);
- [populate.cpp:111-113](../tpcc/generator/populate.cpp#L111-L113);
- [populate.cpp:134-136](../tpcc/generator/populate.cpp#L134-L136);
- [populate.cpp:169-171](../tpcc/generator/populate.cpp#L169-L171).

Это прямое отклонение от требований initial population TPC-C, принятое ради
воспроизводимости. Синтетические даты привязаны к одной конкретной reference
date, а зависимость от seed обеспечивает стабильное наполнение при повторной
загрузке, на разных hosts и в разных adapters. Решение зафиксировано во
внутренней [specification.md](specification.md).

Текущая реализация распределяет отдельные timestamps в интервале до семи суток
после reference epoch
([populate.cpp:26-29](../tpcc/generator/populate.cpp#L26-L29)). Поэтому её
следует описывать как фиксированное synthetic reference window, а не как одно
одинаковое значение времени для всех rows. Это уточнение документации, не
регрессия последних исправлений.

### 4.2. OL_DELIVERY_D не равен O_ENTRY_D

**Статус: присутствует на момент анализа.**

Для начальных доставленных заказов TPC-C §4.3.3.1 требует:

```text
OL_DELIVERY_D = O_ENTRY_D
```

Order и order lines получают даты из разных RNG streams:

- [populate.cpp:169-171](../tpcc/generator/populate.cpp#L169-L171);
- [populate.cpp:202-205](../tpcc/generator/populate.cpp#L202-L205).

Кроме того, salt даты order line не включает warehouse и district. Начальная
база формально некорректна, а post-import checks это условие не проверяют.

### 4.3. Генерация a-string не является alphanumeric

**Статус: присутствует на момент анализа.**

TPC-C §4.3.2.2 определяет a-string как случайную последовательность
alphanumeric-символов. `RandomAString` генерирует только `a-z`:
[strings.h:9-23](../tpcc/generator/strings.h#L9-L23).

Это затрагивает ITEM, warehouse, district, customer, history и другие поля.
Поля, для которых спецификация отдельно требует только буквы или n-string,
данным замечанием не затрагиваются.

### 4.4. C-Load не выбирается случайно при population

**Статус: присутствует на момент анализа.**

TPC-C §4.3.3.1 требует независимо случайно выбирать NURand-константу C для
population и test run с соблюдением ограничений на разность.

Обе константы зафиксированы:

- [constants.h:32-33](../tpcc/domain/constants.h#L32-L33);
- [rng.h:164-173](../tpcc/domain/rng.h#L164-L173).

Разность между ними допустима, но требование выбора C для конкретной population
не выполнено.

### 4.5. Нет conformance validation параметров запуска

**Статус: присутствует на момент анализа.**

Конфигурация допускает:

- не 10 терминалов на warehouse;
- mix, не обеспечивающий минимальные проценты TPC-C;
- отключённый pacing;
- произвольные keying и think times;
- measurement interval менее 120 минут.

Связанный код:

- [validate.go:101-107](../tools/tpccctl/internal/validate/validate.go#L101-L107);
- [validate.go:115-137](../tools/tpccctl/internal/validate/validate.go#L115-L137);
- [validate.go:179-191](../tools/tpccctl/internal/validate/validate.go#L179-L191);
- [worker_loader.cpp:96-113](../tpcc/dbms/pgsql/worker_loader.cpp#L96-L113).

TPC-C §4.2.2 требует 10 терминалов на warehouse, а §5.5.2 — непрерывный
measurement interval не менее 120 минут. Aggregate не сообщает, что конкретная
конфигурация этим требованиям не соответствует.

### 4.6. O_ALL_LOCAL некорректен при одном warehouse

**Статус: устранено в commit `4d44f21`.**

TPC-C §2.4.1.5 требует при конфигурации с одним warehouse снабжать все позиции
из home warehouse. Соответственно, создаваемый заказ должен иметь
`O_ALL_LOCAL = 1`.

Теперь remote-ветка недоступна при `WarehouseCount == 1`, а `AllLocal` меняется
на `0` только после выбора другого warehouse:
[new_order.cpp:59-70](../tpcc/transactions/new_order.cpp#L59-L70).
Все заказы односкладской конфигурации получают корректное
`O_ALL_LOCAL = 1`.

### 4.7. Не проверяются фактические пределы вариативности inputs

**Статус: присутствует на момент анализа.**

TPC-C §5.5.1.5 требует проверять на конкретном measurement interval:

- 0,9-1,1% rollback New-Order;
- среднее число order lines в диапазоне 9,5-10,5 и равномерность 5-15;
- 0,95-1,05% remote order lines;
- 14-16% remote Payment;
- 57-63% выбора customer по фамилии для Payment и Order-Status.

Генератор задаёт требуемые вероятности, например в
[new_order.cpp:53-78](../tpcc/transactions/new_order.cpp#L53-L78) и
[payment.cpp:40-57](../tpcc/transactions/payment.cpp#L40-L57), но runtime не
собирает business-input counters, необходимые для проверки реально полученной
выборки. Worker JSON содержит только результаты и retries транзакций
([artifacts.cpp:169-184](../tpcc/dbms/pgsql/artifacts.cpp#L169-L184)).

Даже корректная вероятность генерации не гарантирует попадание конкретного
measurement interval в нормативные пределы.

### 4.8. Response-time требования не проверяются и неполно отчётны

**Статус: присутствует на момент анализа для официального TPC-C; полноценные RTE-метрики вне
принятой области проекта.**

TPC-C требует average, maximum и p90 для каждого типа транзакций, menu response
time, отдельные interactive/deferred Delivery times и response-time
distributions.

Consolidator публикует только p50, p90, p95 и p99:
[merge.go:95-113](../tools/tpccctl/internal/histogram/merge.go#L95-L113).

Average нельзя точно восстановить, поскольку raw histogram не хранит сумму
значений. `MaxRecorded` хранится, но в итоговые response times не выводится.
Также не проверяются допустимые p90:

- 5 секунд для New-Order, Payment, Order-Status и interactive Delivery;
- 20 секунд для Stock-Level;
- 80 секунд для deferred Delivery.

Нарушение этих ограничений не инвалидирует опубликованный throughput.

### 4.9. Проверка синхронизации часов является заглушкой

**Статус: устранено в commit `45ce1f6`.**

Worker выполняет пять запросов `clock_timestamp()` к PostgreSQL, выбирает sample
с минимальным RTT и записывает измеренные offset/uncertainty:

- [clock_calibration.cpp:25-61](../tpcc/dbms/pgsql/clock_calibration.cpp#L25-L61);
- [artifacts.cpp:110-130](../tpcc/dbms/pgsql/artifacts.cpp#L110-L130).

Worker завершается с ошибкой при превышении положительного skew budget
([worker_loader.cpp:74-94](../tpcc/dbms/pgsql/worker_loader.cpp#L74-L94)), а
consolidator проверяет наличие calibration каждого worker и разброс offsets
([consolidate.go:197-260](../tools/tpccctl/internal/consolidate/consolidate.go#L197-L260)).

При `max_clock_skew_ms <= 0` enforcement осознанно отключён; standalone `run`
также не проходит orchestrated calibration. Эти режимы нельзя считать
проверенными multi-host измерениями.

### 4.10. Aggregate считает отсутствие checks успешным результатом

**Статус: устранено в commits `56018f2` и `04b87dd`.**

Consolidator определяет обязательные check reports с учётом явно skipped steps,
возвращает `integrity_ok = false` при отсутствии, ошибке чтения, некорректном
JSON или `ok=false`, а причины публикует в `integrity_errors`:
[consolidate.go:263-357](../tools/tpccctl/internal/consolidate/consolidate.go#L263-L357).

### 4.11. Не контролируются sustained operation и checkpoints

**Статус: частично принятое ограничение области.**

TPC-C §5.5.1.2 требует конфигурацию, способную непрерывно поддерживать
заявленный throughput не менее восьми часов без вмешательства оператора.
§5.5.2.2 требует для систем с deferred writes checkpoint interval не более
30 минут и не менее четырёх checkpoints внутри measurement interval.

Контроль checkpoint осознанно находится вне области portable-tpcc, поскольку
разные СУБД используют разные transaction log, flush и recovery mechanisms.
Их конфигурация и проверка остаются ответственностью оператора.

При этом отсутствие доказательства восьмичасовой устойчивости остаётся
ограничением результата: двухчасовой workload сам по себе не подтверждает
sustained performance тестируемой конфигурации по TPC-C §5.5.1.2.

### 4.12. Нет полного TPC-C disclosure/reporting

**Статус: присутствует на момент анализа для официального TPC-C; FDR не является целью engineering
workload.**

Отсутствуют price/tpmC, availability date, Full Disclosure Report, сведения о
checkpoint, доказательство steady state и значительная часть отчётности §8. В
частности, не строятся обязательные:

- response-time distributions для каждого transaction type (§5.6.1);
- New-Order response time versus throughput (§5.6.2);
- New-Order think-time distribution (§5.6.3);
- New-Order throughput versus elapsed time (§5.6.4).

Итог намеренно помечается как `engineering`:
[consolidate.go:157-174](../tools/tpccctl/internal/consolidate/consolidate.go#L157-L174).

Это правильная маркировка для текущего инструмента, но она подтверждает его
неполноту как официального теста.

## 5. Замечания средней критичности исходного аудита

### 5.1. Денежные значения проходят через double

**Статус: устранено в commit `3e6cede`.**

Таблицы PostgreSQL используют `DECIMAL`, shared domain — `TMoney`/`TRate`, а
адаптер теперь читает точное текстовое представление PostgreSQL:

- [query_result.h:111-127](../tpcc/dbms/pgsql/query_result.h#L111-L127);
- [tpcc_session.cpp:193-239](../tpcc/dbms/pgsql/tpcc_session.cpp#L193-L239);
- [tpcc_session.cpp:342-374](../tpcc/dbms/pgsql/tpcc_session.cpp#L342-L374);
- [tpcc_session.cpp:450-490](../tpcc/dbms/pgsql/tpcc_session.cpp#L450-L490).

Money/rate на domain↔adapter пути больше не преобразуются через `double`.

### 5.2. Конкурентная Delivery выбирает заказ без блокировки

**Статус: устранено в commit `2cab3ab`.**

Oldest new order теперь выбирается с `FOR UPDATE`:
[tpcc_session.cpp:290-298](../tpcc/dbms/pgsql/tpcc_session.cpp#L290-L298).
Удаление дополнительно проверяет affected-row count и классифицирует уже
захваченный заказ как retryable abort:
[tpcc_session.cpp:493-501](../tpcc/dbms/pgsql/tpcc_session.cpp#L493-L501).

TPC-C не предписывает именно этот SQL-механизм, но текущая реализация устраняет
выявленный риск повторной конкурентной обработки.

### 5.3. Legacy import не создаёт customer-name index

**Статус: устранено в commit `7b759a2`.**

Общий `ImportSync` теперь вызывает `CreateIndexes` до `ANALYZE`, поэтому индекс
создаётся и в standalone, и в orchestrated paths:
[import.cpp:243-257](../tpcc/dbms/pgsql/import.cpp#L243-L257).

### 5.4. Consistency checks ослабляют точные сравнения

**Статус: частично устранено в commit `3e6cede`.**

Денежные условия больше не используют допуск `1e-3`; они сравниваются точно
через `IS DISTINCT FROM`, например:

- [check.cpp:192-200](../tpcc/dbms/pgsql/check.cpp#L192-L200);
- [check.cpp:331-350](../tpcc/dbms/pgsql/check.cpp#L331-L350);
- [check.cpp:355-382](../tpcc/dbms/pgsql/check.cpp#L355-L382);
- [check.cpp:407-429](../tpcc/dbms/pgsql/check.cpp#L407-L429).

Остаётся вторая часть исходного замечания: checks 3.3.2.5 и 3.3.2.7 трактуют
carrier `0` как эквивалент `NULL`:

- [check.cpp:250-274](../tpcc/dbms/pgsql/check.cpp#L250-L274);
- [check.cpp:306-327](../tpcc/dbms/pgsql/check.cpp#L306-L327).

Спецификация определяет недоставленный заказ через `NULL`.

## 6. Реализованные корректные части

К сильным сторонам текущей реализации относятся:

- девять основных таблиц и правильные базовые cardinalities;
- все пять типов транзакций и большая часть их DB-изменений;
- default mix `45/43/4/4/4`, корректные минимальные keying times и
  экспоненциальный think time по умолчанию;
- NURand с допустимой разностью текущих C-Load/C-Run;
- 5-15 order lines, 1% remote lines и 1% rollback inputs;
- 85/15 Payment и 60/40 выбор customer;
- `DECIMAL` в PostgreSQL и `TMoney` в shared domain;
- exact-decimal PostgreSQL reads без промежуточного `double`;
- детерминированная и повторяемая загрузка;
- создание secondary indexes во всех import paths;
- все двенадцать consistency conditions;
- exact money comparisons и fail-closed проверка check artifacts;
- блокировка конкурентно выбранного oldest new order в Delivery;
- raw histogram buckets и их merge между workers;
- wall-clock фазы с общим `--start-at` и реальная clock calibration;
- явная маркировка результатов как `engineering`.

## 7. Общая оценка

| Область | Оценка |
| --- | --- |
| DB workload core | В основном реализован; rollback New-Order и Stock-Level имеют нормативные ошибки |
| Initial population | Cardinalities корректны; synthetic dates — принятое отклонение; delivery timestamps, a-string и C-Load не соответствуют §4.3 |
| Driver / RTE | Полный RTE осознанно вне области; latency измеряет workload-client boundary |
| Delivery | Синхронная модель — принятое отклонение; конкурентная обработка oldest order исправлена |
| tpmC и response time | Текущие значения не являются валидными метриками TPC-C |
| Consistency | Хорошее покрытие 3.3.2.1-12; carrier `0` всё ещё считается эквивалентом `NULL` |
| Atomicity / isolation / durability | Встроенные certification tests вне области; гарантии обязательны для каждого DBMS adapter |
| Checkpoints | Управление и контроль вне области; ответственность DBMS/operator |
| Reporting / disclosure | Engineering artifacts, не FDR |
| DBMS adapters | Практически реализован только PostgreSQL |

Для нагрузочного, сравнительного и регрессионного тестирования PostgreSQL
проект пригоден при явном указании фактической конфигурации. Для получения
результата, называемого TPC-C или tpmC, необходима существенная доработка
transaction profiles, population, measurement validation и disclosure
workflow, а также внешнее подтверждение RTE/ACID/checkpoint требований, которые
осознанно не входят в portable-tpcc.

## 8. Итог повторного контроля

| Статус | Замечания |
| --- | --- |
| Устранено | 3.6 think time exponential (default); 4.6 `O_ALL_LOCAL`; 4.9 clock calibration; 4.10 integrity fail-open; 5.1 exact-decimal reads; 5.2 Delivery race; 5.3 legacy index |
| Частично устранено | 5.4 exact money checks исправлены, carrier `0` как `NULL` остался |
| Принятое отклонение | 3.3 synchronous Delivery; 3.4 отсутствие полного RTE; 3.5 отсутствие встроенных ACID tests; 4.1 synthetic initial dates; checkpoint-часть 4.11 |
| Остаётся | 3.1, 3.2, 3.7, 4.2-4.5, 4.7, 4.8, sustained-часть 4.11, 4.12 |

Повторный аудит изменений до commit `4d44f21` не выявил новых регрессий,
внесённых исправлениями. Дополнительно уточнено, что clock-skew enforcement
работает только при положительном `max_clock_skew_ms` и в orchestrated worker
path. Это ограничение конфигурации, а не регрессия реализации calibration.
