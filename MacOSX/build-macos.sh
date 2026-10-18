#!/bin/bash

./setup-vcpkg.sh

xcodebuild -project QuakeSpasm.xcodeproj -target QSS-M

cat <<EOF > build/Release/Quakespasm-Spiked-Revision.txt
Git URL:      $(git config --get remote.origin.url)
Git Revision: $(git rev-parse HEAD)
Git Date:     $(git show --no-patch --no-notes --pretty='%ai' HEAD)
Compile Date: $(date)
EOF

# Keep the loose game assets beside the app so the engine finds them in kuzbass/.
mkdir -p build/Release/kuzbass
cp -R ../Quake/kuzbass/. build/Release/kuzbass/

# zip the files in `build/Release` to create the final archive for distribution
cd build/Release
rm QSS-M_MacOS.zip
zip --symlinks --recurse-paths QSS-M_MacOS.zip *
