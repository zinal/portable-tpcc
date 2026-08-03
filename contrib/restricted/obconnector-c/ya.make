# Vendored from https://github.com/oceanbase/obconnector-c.git
# Sources and compile flags follow the CMake static-library layout
# (libmariadb/CMakeLists.txt + default static plugins).

LIBRARY()

LICENSE(LGPL-2.1-or-later)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

VERSION(2.2.13)

ORIGINAL_SOURCE(https://github.com/oceanbase/obconnector-c/archive/cdaa70e5174592c8331c7ad0d6a5e81ac6d95529.tar.gz)

PEERDIR(
    contrib/libs/openssl
    contrib/libs/zlib
)

ADDINCL(
    GLOBAL contrib/restricted/obconnector-c/include
    contrib/restricted/obconnector-c/include
    contrib/restricted/obconnector-c/include_stubs
    contrib/restricted/obconnector-c/libmariadb
    contrib/restricted/obconnector-c/plugins/auth
    contrib/restricted/obconnector-c/plugins/pvio
)

NO_COMPILER_WARNINGS()

NO_RUNTIME()

CFLAGS(
    -DHAVE_COMPRESS
    -DHAVE_OPENSSL
    -DHAVE_TLS
    -DLIBICONV_PLUG
    -DLIBMARIADB
    -DTHREAD
    -DDBUG_OFF
)

SRCS(
    libmariadb/ma_array.c
    libmariadb/ma_charset.c
    libmariadb/ma_hash.c
    libmariadb/ma_net.c
    libmariadb/mariadb_charset.c
    libmariadb/ma_time.c
    libmariadb/ma_default.c
    libmariadb/ma_errmsg.c
    libmariadb/mariadb_lib.c
    libmariadb/ma_list.c
    libmariadb/ma_pvio.c
    libmariadb/ma_tls.c
    libmariadb/ma_alloc.c
    libmariadb/ma_compress.c
    libmariadb/ma_init.c
    libmariadb/ma_password.c
    libmariadb/ma_ll2str.c
    libmariadb/ma_sha1.c
    libmariadb/mariadb_stmt.c
    libmariadb/ma_loaddata.c
    libmariadb/ma_stmt_codec.c
    libmariadb/ma_string.c
    libmariadb/ma_dtoa.c
    libmariadb/mariadb_rpl.c
    libmariadb/ob_protocol20.c
    libmariadb/ob_bitmap.c
    libmariadb/ob_complex.c
    libmariadb/ob_strtoll10.c
    libmariadb/ob_oracle_format_models.c
    libmariadb/ob_thread.c
    libmariadb/ob_serialize.c
    libmariadb/ob_object.c
    libmariadb/ob_full_link_trace.c
    libmariadb/ob_rwlock.c
    libmariadb/ob_cond.c
    libmariadb/ob_thread_key.c
    libmariadb/ob_load_balance.c
    libmariadb/ob_utils.c
    libmariadb/ob_tnsname.c
    libmariadb/ma_client_plugin.c
    libmariadb/ma_io.c
    libmariadb/secure/openssl.c
    libmariadb/mariadb_dyncol.c
    libmariadb/mariadb_async.c
    libmariadb/ma_context.c
    plugins/auth/my_auth.c
    plugins/auth/old_password.c
    plugins/pvio/pvio_socket.c
)

END()
