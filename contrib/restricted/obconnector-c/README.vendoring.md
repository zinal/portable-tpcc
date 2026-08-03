# OceanBase Connector/C (obconnector-c)

Vendored from [oceanbase/obconnector-c](https://github.com/oceanbase/obconnector-c.git)
(package version 2.2.13 from `rpm/libobclient-VER.txt`).

## Layout

| Path | Role |
| --- | --- |
| `include/` | Public API (`mysql.h`, …) plus frozen CMake-generated headers |
| `include/ma_config.h` | Platform dispatcher → `ma_config-linux.h` |
| `include/ma_config-linux.h` | Output of CMake `ma_config.h.in` on Linux |
| `include/mariadb_version.h` | Output of CMake `mariadb_version.h.in` |
| `libmariadb/` | Connector sources (CMake `LIBMARIADB_SOURCES`) |
| `libmariadb/ma_client_plugin.c` | Generated static plugin registry |
| `plugins/auth/`, `plugins/pvio/` | Default **static** plugins from CMake |

## CMake → ya.make mapping

Configure options mirrored by `ya.make`:

- `WITH_SSL=OPENSSL` → `libmariadb/secure/openssl.c`, `-DHAVE_OPENSSL -DHAVE_TLS`
- `WITH_EXTERNAL_ZLIB=ON` → `PEERDIR(contrib/libs/zlib)` (no bundled `zlib/`)
- `WITH_DYNCOL=ON` → `mariadb_dyncol.c`
- Default static plugins: `mysql_native_password`, `mysql_old_password`, `pvio_socket`
- Defines from `libmariadb/CMakeLists.txt`: `HAVE_COMPRESS`, `LIBMARIADB`, `THREAD`

Dynamic auth plugins (caching_sha2, ed25519, …) are omitted; OceanBase TPC-C
uses native password auth.

## Regenerating config headers

```bash
cmake -S <upstream> -B /tmp/obconnector-build \
  -DWITH_SSL=OPENSSL -DWITH_EXTERNAL_ZLIB=ON \
  -DWITH_UNIT_TESTS=OFF -DWITH_CURL=OFF
cp /tmp/obconnector-build/include/ma_config.h include/ma_config-linux.h
cp /tmp/obconnector-build/include/mariadb_version.h include/mariadb_version.h
cp /tmp/obconnector-build/libmariadb/ma_client_plugin.c libmariadb/ma_client_plugin.c
```
