# portable-tpcc

Проект горизонтально масштабируемой реализации TPC-C с общей логикой
нагрузки, адаптерами YDB/PostgreSQL/OceanBase и отдельным оркестратором.

Текущий репозиторий содержит проект архитектуры:

- [спецификация portable-tpcc](docs/specification.md);
- [пример профиля оркестратора](docs/examples/profile.v1.yaml);
- [пример нормализованной конфигурации запуска](docs/examples/run-config.v1.json);
- [пример токена синхронизированного старта](docs/examples/start-token.v1.json).

Спецификация основана на анализе веток `someshit` реализаций
`zinal/ydb`, `zinal/tpcc-postgres-cpp`, `zinal/tpcc-oceanbase-cpp` и локальных
материалов оркестратора TPC-E в [`temp-portable-tpce`](temp-portable-tpce/).

Реализация ещё не начата. Результаты, полученные будущей программой, не должны
называться официальными TPC-C без предусмотренной TPC проверки.
