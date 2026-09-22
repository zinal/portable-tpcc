# План исправлений transaction/query layer

Статус: рекомендации для `main` на commit `e0c689b6`.

Основа:

- [TPC-C 5.11](https://www.tpc.org/TPC_Documents_Current_Versions/pdf/tpc-c_v5.11.0.pdf),
  прежде всего §§2.3–2.8 и §3.4;
- [Ant Financial OceanBase 2.2 FDR](https://tpc.org/results/fdr/tpcc/ant_financial~tpcc~alibaba_cloud_compute_service_cluster~fdr~2020-05-17~v01.pdf),
  Appendix B, стр. 45–52;
- текущие shared workflows и адаптеры PostgreSQL, YDB и OceanBase.

Документ описывает исправления transaction/query layer, выявленные при
сравнении с FDR, и непосредственно связанные с ними проверки. Полный перечень
проблем initial population, orchestration, artifacts и официальной TPC-C
отчётности остаётся в
[tpcc-5.11-conformance-analysis.md](tpcc-5.11-conformance-analysis.md).
Разделы 1–4 ниже являются четырьмя списками доработок: общим, OceanBase,
PostgreSQL и YDB. Остальные разделы задают инварианты и порядок применения,
но не добавляют отдельный backlog.

Приоритеты:

- **P0** — атомарность, целостность или возможность безопасно выполнить run;
- **P1** — обязательная семантика transaction profile, adapter contract или
  диагностика integrity verdict;
- **P2** — ожидаемый существенный выигрыш по latency/throughput;
- **P3** — дополнительная оптимизация либо работа только для официального
  TPC-C scope.

## Инварианты решения

- Оптимизация числа SQL/YQL statements допустима, но не должна уменьшать
  число обязательных логических операций из TPC-C §2.3.3.
- Повторные ITEM/STOCK keys внутри одного New-Order должны сохранять
  line-level семантику. Set-oriented statement допустим, если вход и результат
  по-прежнему содержат отдельную позицию для каждой order line.
- `customer.c_data` должен читаться и изменяться только для Payment BC.
  Отдельная прикладная таблица `customer_data` не требуется. Физическое
  размещение может отличаться только через прозрачный механизм СУБД.
- Ошибки cardinality и rollback должны обнаруживаться до подтверждения
  успешного исхода транзакции.
- Stored procedures не являются общей целью. Их можно добавлять только как
  измеренный DBMS-specific режим, сохраняя тот же semantic contract и
  обычный SQL/YQL fallback.

## 1. Общие доработки

### G1. Сохранить операции для повторных ITEM/STOCK keys

**Приоритет: P1.**

Shared New-Order удаляет повторные STOCK keys до чтения, а YDB/OceanBase
дополнительно объединяют повторные updates. Это сохраняет обычное конечное
состояние, но уменьшает обязательные операции «для каждой order line» из
TPC-C §2.4.2.2 и не проходит буквальное требование §2.3.3.

Нужно передавать в адаптер line ordinal вместе с ITEM/STOCK key, возвращать
результат для каждой позиции и применять STOCK transitions в порядке order
lines. Пакетная передача остаётся разрешённой; нельзя заменять несколько
логических обращений одним client-side cached value.

Критерий готовности: тест с двумя одинаковыми `OL_I_ID` подтверждает два
ITEM lookup results, две последовательные STOCK mutations, два ORDER_LINE и
правильные промежуточные `S_QUANTITY`.

### G2. Разделить Customer projections по транзакциям

**Приоритет: P2.**

`TGetCustomerById` и `TGetCustomersByLastName` сейчас возвращают универсальную
строку из 18 полей. New-Order нужны только `C_DISCOUNT`, `C_CREDIT`, `C_LAST`;
Order-Status — `C_ID`, `C_FIRST`, `C_MIDDLE`, `C_LAST`, `C_BALANCE`; Payment —
собственный набор полей и counters.

Следует ввести transaction-specific semantic operations/result types, не
дублируя SQL в shared workflow. `TGetCustomerData` остаётся отдельной
операцией только для Payment BC.

Критерий готовности: каждая транзакция проецирует только поля TPC-C profile и
технические поля, необходимость которых объяснена в adapter documentation.

### G3. Проецировать только нужный `S_DIST_xx`

**Приоритет: P2.**

PostgreSQL и OceanBase передают все десять 24-символьных `S_DIST_xx`, хотя
New-Order нужен только district-specific столбец. Все адаптеры должны
возвращать единое поле `s_dist_info`, используя `CASE` либо ограниченное
семейство кэшируемых statement shapes.

Критерий готовности: result содержит один `s_dist_info`, statement/query cache
остаётся ограниченным, а line-level ITEM/STOCK cardinality сохраняется.

### G4. Перенести агрегирование Delivery в контракт адаптера

**Приоритет: P2.**

Сейчас PostgreSQL, OceanBase и YDB получают каждое `OL_AMOUNT` и суммируют
значения на клиенте. Semantic operation `TGetDeliveryOrderInfo` должна
позволять СУБД вернуть `O_C_ID`, `O_OL_CNT`, `SUM(OL_AMOUNT)` и фактический
`COUNT(*)` на один order. Это уменьшает result traffic, но сохраняет
требуемое чтение ORDER_LINE и точную проверку line count.

Критерий готовности: сумма вычисляется exact numeric арифметикой СУБД,
`COUNT(*)` равен `O_OL_CNT`, пустой/неполный набор lines является integrity
error, а подтверждённый `LineCount` используется при последующем Delivery
update.

### G5. Выполнять обязательные вычисления New-Order

**Приоритет: P1.**

Текущий workflow получает `I_DATA`, `S_DATA`, taxes и discount, но не вычисляет
brand/generic и итог:

```text
SUM(OL_AMOUNT) * (1 - C_DISCOUNT) * (1 + W_TAX + D_TAX)
```

Fixed workload должен всегда выполнять эти вычисления exact decimal
арифметикой. Значения можно передавать в typed transaction trace/result,
который не участвует в основном measurement output; отдельный runtime mode
для сокращённого профиля не вводится.

Критерий готовности: unit tests покрывают `ORIGINAL` в обоих полях, mixed
brand/generic и округление итоговой суммы.

### G6. Исправить Payment при одном warehouse

**Приоритет: P1.**

В remote-ветви генератора при `WarehouseCount == 1` warehouse остаётся home,
но customer district может стать случайным. По TPC-C §2.5.1.2 при одном
warehouse Payment обязан быть полностью local, включая district.

Критерий готовности: для one-warehouse profile всегда выполняются
`C_W_ID = W_ID` и `C_D_ID = D_ID`; для нескольких warehouses сохраняется
заданная доля remote inputs.

### G7. Добавить общие semantic trace tests

**Приоритет: P1.**

Нужны adapter-independent сценарии, проверяющие последовательность и
cardinality операций:

- повторные ITEM/STOCK keys;
- последний несуществующий ITEM и подтверждённый rollback;
- конкурентные Payment/Delivery одного customer;
- конкурентные Delivery одного district;
- missing ITEM, STOCK, ORDER и ORDER_LINE, включая несовпадение
  `COUNT(ORDER_LINE)` с `O_OL_CNT`;
- Payment BC/GC и customer-by-last-name с чётным/нечётным числом строк.

Критерий готовности: один набор сценариев запускается для fake session и для
integration adapters; расхождение в количестве логических операций ломает
тест.

### G8. Соблюдать контракт `cancelled`

**Приоритет: P1.**

`cancelled` сейчас может пройти через shared workflow как обычный transaction
failure, хотя adapter API требует остановить phase без retry. Shared helpers
должны преобразовывать этот класс в управляемую отмену, не записывать её как
permanent failure и не запускать следующую попытку.

Критерий готовности: semantic test инжектирует `cancelled` в каждый workflow,
после чего phase прекращает admission, commit не вызывается, retry отсутствует.

### G9. Измерять стоимость query plan, а не только transaction latency

**Приоритет: P2.**

Portable counters могут безопасно включать количество DB requests,
возвращённых rows, retryable aborts и объём result data по типу транзакции.
Физические rows read, buffer/cache statistics и планы собираются только
опциональными DBMS tools вне authoritative measurement, чтобы instrumentation
не менял optimizer plan и latency.

DBMS-specific benchmark сравнивает одинаковые inputs до и после изменения и
проверяет отсутствие регрессии p90/p99. Диагностические counters не меняют
основной response-time histogram.

### G10. Не смешивать query fixes с официальным RTE/Delivery scope

**Приоритет: P3.**

Deferred Delivery, result log, terminal screens и официальные response-time
границы остаются отдельным проектом. Если целью станет официальный TPC-C,
нужно реализовать весь scope §§2.7.2, 5 и 6, а не только заменить inline
Delivery очередью внутри одного адаптера.

## 2. OceanBase

### OB1. Не подтверждать неуспешный ROLLBACK

**Приоритет: P0.**

`TObConnection::Rollback()` игнорирует ошибку `ROLLBACK`, после чего верхний
слой сообщает `RolledBack`. Нужно вернуть ошибку, классифицировать потерю
соединения как unknown outcome и не считать intentional New-Order успешно
откаченным без подтверждения.

Критерий готовности: unit/integration fault injection отличает подтверждённый
rollback, server error и connection loss; session с неизвестным состоянием не
возвращается в pool.

### OB2. Блокировать только выбранного Payment customer

**Приоритет: P2.**

`SELECT ... WHERE c_last = ? ... FOR UPDATE` блокирует всех клиентов с одной
фамилией. Следует получить упорядоченные IDs, выбрать медиану и заблокировать
только выбранную строку, либо использовать эквивалентный server-side запрос.
Order-Status должен остаться неблокирующим.

Критерий готовности: конкурентный Payment/Delivery для другого клиента с той
же фамилией не ждёт завершения текущего Payment; медиана соответствует
TPC-C §2.5.2.2.

### OB3. Агрегировать Delivery order lines на сервере

**Приоритет: P2.**

Сохранить существующий prefetch десяти districts, но заменить передачу всех
`OL_AMOUNT` на `GROUP BY ol_d_id, ol_o_id` с `SUM` и `COUNT`; одновременно
получить `O_OL_CNT` и потребовать его равенство фактическому count. Missing
order и неполный набор lines должны оставаться integrity errors.

### OB4. Объединить Stock-Level в один statement

**Приоритет: P2.**

FDR получает `D_NEXT_O_ID` и считает distinct low-stock items одним запросом
через DISTRICT. Аналогичная форма убирает один request и остаётся
эквивалентной TPC-C §2.8.2.2.

Критерий готовности: один statement использует interval
`[D_NEXT_O_ID - 20, D_NEXT_O_ID - 1]` и возвращает ровно одну count row.

### OB5. Сократить reservation district до атомарного DML

**Приоритет: P2.**

Если используемая версия MySQL-compatible OceanBase поддерживает надёжный
`UPDATE ... RETURNING` или эквивалент, заменить `SELECT ... FOR UPDATE` +
`UPDATE` одним atomic statement. Текущая форма остаётся fallback; перенос
Oracle-mode syntax из FDR без проверки версии запрещён.

### OB6. Снизить parse/round-trip overhead без обязательных procedures

**Приоритет: P3.**

Payment/Delivery multi-statements содержат динамические literals и хуже
используют statement cache. Следует отдельно измерить:

1. параметризованные statements;
2. текущие bounded multi-statements;
3. опциональные MySQL-mode stored routines.

Режим принимается только при одинаковой семантике affected-row checks,
commit outcome и retries. Oracle-mode `FORALL` из FDR напрямую не переносится.

### OB7. Различать check failure и execution error

**Приоритет: P1.**

Найденное consistency violation должно иметь status `failed`, а timeout,
connection loss и SQL error — `error`. Итог остаётся fail-closed в обоих
случаях, но оператор получает корректную причину.

## 3. PostgreSQL

### PG1. Пакетно читать ITEM и STOCK

**Приоритет: P2.**

PostgreSQL выполняет отдельный request на каждый ITEM и STOCK. Нужно
использовать array/`VALUES` input с line ordinal, один set-oriented ITEM query
и один STOCK `FOR UPDATE`. Вход должен сохранять повторы согласно G1.

Критерий готовности: число requests не зависит от `O_OL_CNT`, missing row
определяется по line-level cardinality, STOCK locks берутся в стабильном
порядке.

### PG2. Реализовать optimized `ExecuteBatch`

**Приоритет: P2.**

`TUpdateStock`, `TInsertOrderLine`, `TCompleteOrderDelivery` и
`TApplyDeliveryToCustomer` сейчас выполняются последовательно. Нужны
`VALUES`/CTE/multi-row INSERT формы с fail-closed `RETURNING` или
cardinality result.

После реализации и тестов `ExecuteBatchOptimized` можно переключить в `true`.
До этого capability должна честно оставаться `false`.

### PG3. Стабилизировать порядок STOCK locks

**Приоритет: P2.**

До пакетного `FOR UPDATE` keys следует сортировать по `(s_w_id, s_i_id,
line_ordinal)`. Это уменьшает взаимные deadlocks New-Order без изменения
логического порядка STOCK transitions для повторных keys.

### PG4. Сжать Payment location и выбранный customer path

**Приоритет: P2.**

Два updates и два selects location можно объединить CTE/`RETURNING` запросом.
Для customer-by-last-name следует передавать только IDs/ordering columns до
выбора медианы, затем получать Payment projection выбранной строки.

Обычный SELECT при PostgreSQL Repeatable Read не следует объявлять
доказанной lost-update ошибкой: concurrent update приводит к serialization
failure. `FOR UPDATE` выбранной строки — опциональный способ заменить часть
aborts ожиданием; решение принимается по измерению contention.

### PG5. Prefetch и batching Delivery

**Приоритет: P2.**

Получить oldest order для десяти districts и агрегированные order info одним
или несколькими bounded statements, затем пакетно выполнить delete/order/
order_line/customer updates. `DELETE ... RETURNING` должен отличать
конкурентный claim (`retryable_abort`) от повреждения cardinality.
Order info обязан вернуть `O_OL_CNT` и проверить его против фактического
`COUNT(ORDER_LINE)`.

### PG6. Объединить Stock-Level в один query

**Приоритет: P2.**

JOIN к DISTRICT устраняет отдельное чтение `D_NEXT_O_ID`. План должен
использовать warehouse/district/order range и существующие indexes; изменение
принимается после `EXPLAIN (ANALYZE, BUFFERS)` на репрезентативном масштабе.

### PG7. Различать check failure и execution error

**Приоритет: P1.**

Как и для OceanBase, SQLSTATE timeout/connection/syntax errors должны
формировать `ECheckStatus::Error`, а найденные bad rows —
`ECheckStatus::Failed`.

## 4. YDB

При реализации пунктов этого раздела необходимо следовать актуальным
`ydb-core`/`ydb-table` skills и документации Query Service; синхронные
`GetValueSync()` в scheduler path запрещены.

### YDB1. Сделать worker DML fail-closed

**Приоритет: P0.**

Многие YQL updates/deletes возвращают константный `OkOp`/`OkBatch`, не
доказывая число найденных строк. Если Query Service не предоставляет
affected-row count, запрос должен вернуть matched keys или postcondition
result в той же транзакции.

В первую очередь проверяются:

- district reservation;
- customer Payment update и HISTORY insert;
- удаление `new_order`;
- carrier и ORDER_LINE Delivery updates;
- STOCK и ORDER_LINE New-Order batches.

### YDB2. Заменить скрывающие ошибки `UPSERT`

**Приоритет: P0/P1.**

Создание `oorder`, `new_order`, `order_line` должно использовать insert
semantics, а изменение существующих STOCK/OORDER — update semantics.
Частичный `UPSERT` carrier способен создать stub order, а STOCK UPSERT —
скрыть отсутствующую строку.

Технический HISTORY key сохраняется, но collision должен быть ошибкой, а не
перезаписью.

### YDB3. Обнаруживать конкурентный Delivery claim

**Приоритет: P0.**

Удаление `new_order` должно подтвердить ровно один key на district. Нулевой
результат после ранее выбранного order классифицируется как
`retryable_abort`; остальные mismatches — `integrity`. До этого нельзя
считать batch Delivery fail-closed.

### YDB4. Исправить NULL semantics integrity checks

**Приоритет: P0.**

YQL `NULL != value` даёт `UNKNOWN` и может пропустить повреждение. Все
predicates consistency conditions должны явно проверять NULL. В 3.3.2.4
нельзя приравнивать отсутствующую aggregate side к нулю через симметричный
`COALESCE`.

Критерий готовности: DB-backed fixtures с NULL, отсутствующей стороной JOIN,
orphan rows и неверными aggregates завершаются failed verdict.

### YDB5. Prefetch и агрегирование Delivery

**Приоритет: P2.**

Сейчас shared loop вызывает oldest-order и order-info по каждому district.
Нужно одним bounded YQL flow получить oldest IDs, `O_C_ID`,
`O_OL_CNT`, `SUM(OL_AMOUNT)` и `COUNT(*)`, потребовать
`COUNT(*) = O_OL_CNT` и сохранить transaction snapshot.

### YDB6. Объединить Payment finish с commit

**Приоритет: P2 после YDB1.**

Customer update и HISTORY insert следует выполнять в одном
`ExecuteFinalAndCommit` flow с проверкой обеих целей до commit. BC-ветка
дополнительно обновляет `c_data`; GC не должна затрагивать family `cdata`.

### YDB7. Пересмотреть Stock-Level single-query plan

**Приоритет: P3.**

Текущие два запроса корректны и соответствуют известным ограничениям YQL
JOIN. Переход к одному statement допустим только если актуальный Query Service
строит стабильный план с range predicates в `WHERE`; иначе оставить два
запроса.

### YDB8. Сохранить и проверить `cdata` column group

**Приоритет: P1.**

`c_data` уже размещён в `FAMILY cdata`; generic customer projections не
должны его читать. Нужны schema/query regression tests, подтверждающие, что
только Payment BC обращается к полю. Дополнительная таблица не создаётся.

## Последовательность применения списков

| Этап | Пункты |
| --- | --- |
| Correctness | OB1, YDB1–YDB4, G1, G6, G8 |
| Общий API и тесты | G2–G5, G7 |
| Основные performance changes | PG1–PG6, OB2–OB4, YDB5–YDB6 |
| Diagnostics | OB7, PG7, G9 |
| Дополнительные улучшения | G10, OB5–OB6, YDB7 |

Каждый performance пункт принимается только при одинаковых deterministic
inputs до и после изменения, проверенной transaction state и affected
cardinality, отсутствии новых permanent/integrity errors, сравнении DB
requests/retries/throughput/p90/p99 и integration run на соответствующей
СУБД.
