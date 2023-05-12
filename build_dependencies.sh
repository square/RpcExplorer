#!/bin/bash

set -e

CDK_VERSION="${CDK_VERSION:-cdk-5.0-20230201}"
mkdir -p lib
pushd lib

curl -L -o protobuf-cpp-3.21.2.tar.gz https://github.com/protocolbuffers/protobuf/releases/download/v21.2/protobuf-cpp-3.21.2.tar.gz
curl -L -o ncurses-6.3.tar.gz         https://ftp.gnu.org/pub/gnu/ncurses/ncurses-6.3.tar.gz
curl -L -o cdk.tar.gz "https://invisible-island.net/archives/cdk/${CDK_VERSION}.tgz"
for x in *.tar.gz; do tar xf $x; done

pushd ncurses-6.3
./configure --prefix=`pwd`/dist --enable-widec && make && make install
popd

pushd "${CDK_VERSION}"
./configure --prefix=`pwd`/dist --with-curses-dir=`pwd`/../ncurses-6.3/dist --with-ncursesw
make && make install
popd

pushd protobuf-3.21.2/
./configure --prefix=`pwd`/dist && make && make install
popd

popd
