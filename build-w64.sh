#!/bin/sh

echo "Git URL:      https://github.com/timbergeron/QSS-M.git" > QSS-M-Revision.txt
echo "Git Revision: `git rev-parse HEAD`" >> QSS-M-Revision.txt
echo "Git Date:     `git log -1 --date=short --format=%cd`" >> QSS-M-Revision.txt
echo "Compile Date: `date`" >> QSS-M-Revision.txt
export SOURCE_DATE_EPOCH=$(git log -1 --date=short --format=%ct)

cd Quake/
MAKEARGS="-j8"

# Make win64
export QSS_CFLAGS="-DQSS_REVISION=`git rev-parse HEAD`"
export QSS_LDFLAGS="-Wl,--allow-multiple-definition"
make -f Makefile.w64 clean
./build_cross_win64-sdl2.sh $MAKEARGS
mv quakespasm.exe QSS-M-w64.exe
zip -9j QSS-M-w64.zip ../Windows/codecs/x64/*.dll ../Windows/curl/lib/x64/libcurl.dll ../Windows/zlib/x64/zlib1.dll ../LICENSE.txt ../Quakespasm.html quakespasm.pak qssm.pak ../Quakespasm.txt ../Quakespasm-Spiked.txt ../Quakespasm-Music.txt ../Windows/SDL2/lib64/SDL2.dll ../QSS-M-Revision.txt QSS-M-w64.exe
zip -9 -r QSS-M-w64.zip kuzbass/
make -f Makefile.w64 clean
