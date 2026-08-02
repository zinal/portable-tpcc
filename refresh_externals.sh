#! /bin/bash

set -e

#YDB_ROOT=/Users/mzinal/Projects/ydb/core-zinal
YDB_ROOT=/home/zinal/Projects/YDB/core/zinal

echo "YDB_ROOT: $YDB_ROOT"

cp $YDB_ROOT/ya .
cp $YDB_ROOT/ya.bat .
cp $YDB_ROOT/ya.conf .

rm -rf build
cp -r $YDB_ROOT/build build

rm -rf util
cp -r $YDB_ROOT/util util

ls contrib/libs | while read v; do
  if [ -d "$YDB_ROOT/contrib/libs/$v" ]; then
    rm -rf contrib/libs/$v
    cp -r $YDB_ROOT/contrib/libs/$v contrib/libs/$v
  fi
done

ls contrib/restricted | while read v; do
  rm -rf contrib/restricted/$v
  cp -r $YDB_ROOT/contrib/restricted/$v contrib/restricted/$v
done

ls contrib/tools | while read v; do
  rm -rf contrib/tools/$v
  cp -r $YDB_ROOT/contrib/tools/$v contrib/tools/$v
done

ls library/cpp | while read v; do
  rm -rf library/cpp/$v
  cp -r $YDB_ROOT/library/cpp/$v library/cpp/$v
done
