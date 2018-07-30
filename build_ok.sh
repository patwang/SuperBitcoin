dependsdir = /home/zcash/depends/x86_64-unknown-linux-gnu

export BOOST_INCLUDEDIR="$(dependsdir)/include"
export BOOST_LIBRARYDIR="$(dependsdir)/lib"

cmake .  