#!/bin/bash
set -euo pipefail
export PATH=/usr/bin:/bin:/mingw64/bin:$PATH
cd /c/Users/USUARIO/OneDrive/Desktop/OJBS-OPENCODE/dgbminer-src
sed -i 's/PTHREAD_LIBS="-lpthreadGC2"/PTHREAD_LIBS="-lpthread"/g' configure.ac configure
sed -i 's#PTHREAD_LDFLAGS="-L/mingw/x86_64-w64-mingw32/lib"#PTHREAD_LDFLAGS=""#g' configure.ac configure
./autogen.sh
make distclean >/dev/null 2>&1 || true
rm -f config.status
CFLAGS='-O3 -fcommon -march=x86-64 -Wall -Wno-unused-result -D_WIN32_WINNT=0x0601 -DCURL_STATICLIB' \
LDFLAGS='-static -static-libgcc -static-libstdc++' \
LIBS='-lcurl -lz -lwldap32 -lbcrypt -ladvapi32 -lcrypt32 -lsecur32 -lws2_32 -liphlpapi -lssh2 -lws2_32 -lcrypto -lcrypt32 -lbcrypt -lz -lbrotlidec -lbrotlicommon -lzstd -lnghttp2 -lngtcp2_crypto_ossl -lngtcp2 -lssl -lcrypto -lws2_32 -lgdi32 -lcrypt32 -lbcrypt -lnghttp3 -lpsl -lunistring -lws2_32 -liconv -lidn2 -liconv -lunistring -lnormaliz' \
./configure --host=x86_64-w64-mingw32 --with-curl=/mingw64 --with-crypto=/mingw64 --disable-assembly
make -j4
cp -f cpuminer.exe ../cpuminer-windows.exe
