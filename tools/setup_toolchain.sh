#!/bin/sh
# Fetch / build everything the matching build needs into tools/ and .venv/.
#   - Python venv with splat + spimdisasm
#   - GNU binutils for mips-linux-gnu (built from source: Homebrew has none)
#   - PSY-Q's GCC 2.8.1 (cc1/cpp) prebuilt by decompals/old-gcc
#   - maspsx (git submodule)
set -e
cd "$(dirname "$0")/.."

[ -d .venv ] || python3 -m venv .venv
.venv/bin/pip install -q -r requirements.txt

git submodule update --init tools/maspsx

if [ ! -x tools/binutils/bin/mips-linux-gnu-as ]; then
  V=2.45
  mkdir -p tools/build && cd tools/build
  [ -f binutils-$V.tar.xz ] || curl -fsSL https://ftp.gnu.org/gnu/binutils/binutils-$V.tar.xz -o binutils-$V.tar.xz
  rm -rf binutils-$V bld && tar xf binutils-$V.tar.xz && mkdir bld && cd bld
  # --with-system-zlib: the bundled zlib does not build against the macOS 26+ SDK
  ../binutils-$V/configure --target=mips-linux-gnu --prefix="$(cd ../.. && pwd)/binutils" \
    --disable-nls --disable-werror --disable-gdb --disable-gprof --disable-gprofng \
    --disable-sim --with-system-zlib --without-zstd > configure.log
  make -j"$(getconf _NPROCESSORS_ONLN)" > make.log && make install > install.log
  cd ../../..
fi

case "$(uname -s)" in
  Darwin) SUFFIX=-macos ;;
  *) SUFFIX= ;;
esac
for v in 2.8.1; do
  if [ ! -x tools/gcc-$v-psx/cc1 ]; then
    mkdir -p tools/gcc-$v-psx
    curl -fsSL "https://github.com/decompals/old-gcc/releases/download/0.17/gcc-$v-psx$SUFFIX.tar.gz" \
      | tar xz -C tools/gcc-$v-psx
  fi
done
echo "toolchain ready"
