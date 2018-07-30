dependsdir = /home/zcash/depends/x86_64-unknown-linux-gnu

export BOOST_INCLUDE_DIR="$(dependsdir)/include"
export BOOST_LIBRARY_DIR="$(dependsdir)/lib"

cmake .