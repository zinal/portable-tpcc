# Анализ соответствия реализации требованиям TPC-C 5.11

Статус: анализ текущей реализации PostgreSQL и общего TPC-C-кода.

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

Основные блокирующие причины:

1. неверный учёт штатных rollback New-Order в throughput и response time;
2. сокращённый профиль rollback New-Order;
3. синхронная Delivery вместо обязательной deferred-модели;
4. отсутствие полноценного Remote Terminal Emulator;
5. отсутствие обязательных atomicity, isolation и durability tests;
6. несоответствующая спецификации модель think time и Stock-Level terminals;
7. нарушения initial population;
8. неполная проверка валидности measurement interval и отчётность.

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

## 3. Критические недостатки

### 3.1. Неверно считается tpmC

TPC-C §5.1.2 и §5.4.2 требуют включать в MQTh штатные 1% New-Order,
завершившиеся rollback из-за неверного item. Их response time также входит в
статистику New-Order.

Реализация учитывает такие транзакции отдельно как `UserAborted`:

- [new_order.cpp:143-145](../tpcc/transactions/new_order.cpp#L143-L145);
- [terminal.cpp:186-189](../tpcc/dbms/pgsql/terminal.cpp#L186-L189);
- [terminal.cpp:242-244](../tpcc/dbms/pgsql/terminal.cpp#L242-L244).

Но throughput вычисляется только из `new_order_ok`:

- [artifacts.cpp:165-187](../tpcc/dbms/pgsql/artifacts.cpp#L165-L187);
- [consolidate.go:148-153](../tools/tpccctl/internal/consolidate/consolidate.go#L148-L153).

Следствия:

- throughput систематически занижен приблизительно на 1%;
- штатные rollback отсутствуют в response-time histogram;
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
интерактивный response time Delivery. Разрешение synchronous Delivery во
внутренней [specification.md](specification.md) является сознательным
отступлением от TPC-C 5.11.

### 3.4. Отсутствует полноценный Remote Terminal Emulator

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

### 3.5. Отсутствуют обязательные ACID tests

Реализовано хорошее покрытие consistency conditions 3.3.2.1-12:

- [catalog.cpp:18-34](../tpcc/checks/catalog.cpp#L18-L34);
- [check.cpp:192-625](../tpcc/dbms/pgsql/check.cpp#L192-L625).

Однако отсутствуют:

- Atomicity tests из §3.2.2;
- consistency demonstration под нагрузкой из §3.3.3;
- Isolation tests из §3.4.2;
- crash, memory, power и durable-media tests из §3.5.4.

Проверка SQL-инвариантов после workload не заменяет эти испытания.

### 3.6. Think time не имеет требуемого распределения

TPC-C §5.2.5.4 требует независимо выбирать think time из
отрицательно-экспоненциального распределения:

```text
Tt = -log(r) * mean
```

Код всегда ждёт ровно настроенное среднее:
[terminal.cpp:331-334](../tpcc/dbms/pgsql/terminal.cpp#L331-L334).

Это меняет распределение нагрузки, конкуренцию и достигаемый throughput.
Постоянный keying time допустим, но постоянный think time — нет.

### 3.7. Stock-Level не закреплён за district терминала

По TPC-C §2.8.1.1 каждому из десяти терминалов warehouse должна соответствовать
постоянная уникальная пара `(W_ID, D_ID)`.

В runtime терминалу передаётся только warehouse
([runner.cpp:329-349](../tpcc/dbms/pgsql/runner.cpp#L329-L349)), а Stock-Level
выбирает district заново для каждой транзакции:
[stock_level.cpp:26-31](../tpcc/transactions/stock_level.cpp#L26-L31).

Это нарушает профиль Stock-Level и распределение доступа по districts.

## 4. Недостатки высокой критичности

### 4.1. Начальные даты не соответствуют спецификации

TPC-C §4.3.3.1 требует, чтобы `C_SINCE`, `H_DATE` и `O_ENTRY_D` содержали текущие
дату и время ОС при population.

Генератор использует случайное время в пределах недели после фиксированной
эпохи 2020-01-01:

- [populate.h:9-11](../tpcc/generator/populate.h#L9-L11);
- [populate.cpp:26-29](../tpcc/generator/populate.cpp#L26-L29);
- [populate.cpp:111-113](../tpcc/generator/populate.cpp#L111-L113);
- [populate.cpp:134-136](../tpcc/generator/populate.cpp#L134-L136);
- [populate.cpp:169-171](../tpcc/generator/populate.cpp#L169-L171).

Это прямое отклонение от требований initial population.

### 4.2. OL_DELIVERY_D не равен O_ENTRY_D

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

TPC-C §4.3.2.2 определяет a-string как случайную последовательность
alphanumeric-символов. `RandomAString` генерирует только `a-z`:
[strings.h:9-23](../tpcc/generator/strings.h#L9-L23).

Это затрагивает ITEM, warehouse, district, customer, history и другие поля.
Поля, для которых спецификация отдельно требует только буквы или n-string,
данным замечанием не затрагиваются.

### 4.4. C-Load не выбирается случайно при population

TPC-C §4.3.3.1 требует независимо случайно выбирать NURand-константу C для
population и test run с соблюдением ограничений на разность.

Обе константы зафиксированы:

- [constants.h:32-33](../tpcc/domain/constants.h#L32-L33);
- [rng.h:164-173](../tpcc/domain/rng.h#L164-L173).

Разность между ними допустима, но требование выбора C для конкретной population
не выполнено.

### 4.5. Нет conformance validation параметров запуска

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
- [worker_loader.cpp:78-95](../tpcc/dbms/pgsql/worker_loader.cpp#L78-L95).

TPC-C §4.2.2 требует 10 терминалов на warehouse, а §5.5.2 — непрерывный
measurement interval не менее 120 минут. Aggregate не сообщает, что конкретная
конфигурация этим требованиям не соответствует.

### 4.6. Response-time требования не проверяются и неполно отчётны

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

### 4.7. Проверка синхронизации часов является заглушкой

Каждый worker всегда записывает нулевые `offset_ms`, `uncertainty_ms` и
`rtt_ms`:
[artifacts.cpp:122-127](../tpcc/dbms/pgsql/artifacts.cpp#L122-L127).

Consolidator доверяет этим значениям:
[consolidate.go:114-126](../tools/tpccctl/internal/consolidate/consolidate.go#L114-L126).

При реальном clock skew workers могут измерять разные интервалы, но
`clock_skew_ok` останется равным `true`.

### 4.8. Aggregate считает отсутствие checks успешным результатом

Если каталог checks отсутствует, `evaluateIntegrity` возвращает `true`:
[consolidate.go:203-209](../tools/tpccctl/internal/consolidate/consolidate.go#L203-L209).

Это fail-open поведение: непроведённая проверка представляется успешно
пройденной.

### 4.9. Нет полного TPC-C disclosure/reporting

Отсутствуют price/tpmC, availability date, Full Disclosure Report, сведения о
checkpoint, доказательство steady state, обязательные response/think-time
графики и значительная часть отчётности §8.

Итог намеренно помечается как `engineering`:
[consolidate.go:157-174](../tools/tpccctl/internal/consolidate/consolidate.go#L157-L174).

Это правильная маркировка для текущего инструмента, но она подтверждает его
неполноту как официального теста.

## 5. Недостатки средней критичности

### 5.1. Денежные значения проходят через double

Таблицы PostgreSQL используют `DECIMAL`, а shared domain — `TMoney`. Однако
чтение денежных значений из PostgreSQL выполняется через `double`:

- [tpcc_session.cpp:35-43](../tpcc/dbms/pgsql/tpcc_session.cpp#L35-L43);
- [tpcc_session.cpp:226-229](../tpcc/dbms/pgsql/tpcc_session.cpp#L226-L229);
- [tpcc_session.cpp:375-378](../tpcc/dbms/pgsql/tpcc_session.cpp#L375-L378);
- [tpcc_session.cpp:473-498](../tpcc/dbms/pgsql/tpcc_session.cpp#L473-L498).

Это нарушает end-to-end exact-decimal подход §1.4.12 и внутреннее требование
проекта. Для текущих диапазонов округление обратно до cents обычно
восстанавливает значение, поэтому практический риск ограничен.

### 5.2. Конкурентная Delivery выбирает заказ без блокировки

Oldest new order выбирается без `FOR UPDATE`:
[tpcc_session.cpp:301-305](../tpcc/dbms/pgsql/tpcc_session.cpp#L301-L305).

Repeatable Read обычно преобразует конфликт в retry, но явной атомарной операции
«выбрать и удалить» нет, а количество реально изменённых строк не проверяется.
Это повышает риск повторной обработки и искажения skipped-delivery статистики.

### 5.3. Legacy import не создаёт customer-name index

Индекс определён в [init.cpp:168-170](../tpcc/dbms/pgsql/init.cpp#L168-L170), но
standalone `import` его не создаёт. Индекс явно создаётся только orchestrated
loader:
[worker_loader.cpp:44-49](../tpcc/dbms/pgsql/worker_loader.cpp#L44-L49).

Путь `init -> import -> run` поэтому либо не проходит preflight, либо не
получает необходимый индекс для Payment и Order-Status по фамилии.

### 5.4. Consistency checks ослабляют точные сравнения

Денежные условия проверяются с допуском `1e-3`, например:

- [check.cpp:192-200](../tpcc/dbms/pgsql/check.cpp#L192-L200);
- [check.cpp:331-350](../tpcc/dbms/pgsql/check.cpp#L331-L350);
- [check.cpp:355-382](../tpcc/dbms/pgsql/check.cpp#L355-L382);
- [check.cpp:407-429](../tpcc/dbms/pgsql/check.cpp#L407-L429).

Кроме того, checks 3.3.2.5 и 3.3.2.7 трактуют carrier `0` как эквивалент
`NULL`:

- [check.cpp:250-274](../tpcc/dbms/pgsql/check.cpp#L250-L274);
- [check.cpp:306-327](../tpcc/dbms/pgsql/check.cpp#L306-L327).

Спецификация определяет недоставленный заказ через `NULL`.

## 6. Реализованные корректные части

К сильным сторонам текущей реализации относятся:

- девять основных таблиц и правильные базовые cardinalities;
- все пять типов транзакций и большая часть их DB-изменений;
- default mix `45/43/4/4/4` и корректные минимальные keying times;
- NURand с допустимой разностью текущих C-Load/C-Run;
- 5-15 order lines, 1% remote lines и 1% rollback inputs;
- 85/15 Payment и 60/40 выбор customer;
- `DECIMAL` в PostgreSQL и `TMoney` в shared domain;
- детерминированная и повторяемая загрузка;
- все двенадцать consistency conditions;
- raw histogram buckets и их merge между workers;
- wall-clock фазы с общим `--start-at`;
- явная маркировка результатов как `engineering`.

## 7. Общая оценка

| Область | Оценка |
| --- | --- |
| DB workload core | В основном реализован; rollback New-Order и Stock-Level имеют нормативные ошибки |
| Initial population | Cardinalities корректны; даты, delivery timestamps, a-string и C-Load частично не соответствуют §4.3 |
| Driver / RTE | Существенно не соответствует TPC-C |
| Delivery | Не соответствует обязательной deferred-модели |
| tpmC и response time | Текущие значения не являются валидными метриками TPC-C |
| Consistency | Хорошее покрытие 3.3.2.1-12 |
| Atomicity / isolation / durability | Обязательные тесты отсутствуют |
| Reporting / disclosure | Engineering artifacts, не FDR |
| DBMS adapters | Практически реализован только PostgreSQL |

Для нагрузочного, сравнительного и регрессионного тестирования PostgreSQL
проект пригоден при явном указании фактической конфигурации. Для получения
результата, называемого TPC-C или tpmC, необходима существенная доработка
transaction profiles, RTE, Delivery, population, measurement validation, ACID
tests и disclosure workflow.
