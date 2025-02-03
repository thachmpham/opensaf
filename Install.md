# Build & Install OpenSaf on Fedora 41

## Dependencies
```sh
yum -y install net-tools sqlite3 libxml2 redhat-lsb-core initscripts

yum -y install mercurial gcc gcc-c++ libxml2-devel automake m4 autoconf libtool pkg-config make python-devel sqlite-devel
```


## Build & Install
```sh
./bootstrap.sh

./configure CPPFLAGS="-DRUNASROOT" CXXFLAGS="-Wno-error -w -Wl,-z,muldefs" CFLAGS="-Wno-error -Wl,-z,muldefs"

make

make install
```
