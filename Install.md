# Build & Install OpenSaf on Fedora 41

## Dependencies
```sh
$ yum -y install net-tools sqlite3 libxml2 redhat-lsb-core initscripts

$ yum -y install mercurial gcc gcc-c++ libxml2-devel automake m4 autoconf libtool pkg-config make python-devel sqlite-devel
```


## Build & Install
```sh
$ ./bootstrap.sh

$ ./configure CPPFLAGS="-DRUNASROOT" CXXFLAGS="-Wno-error -w -Wl,-z,muldefs" CFLAGS="-Wno-error -Wl,-z,muldefs" PYTHON="/usr/bin/python3"

$ make

$ make install

$ ldconfig
```


## Known Issues
### Missing python
- Error.
```sh
$ ./immxml-configure --trace
error: immxml-merge SC templates failed. Aborting script! exitCode: 127
```

- Solution
```sh
ln -s /usr/bin/python3 /usr/bin/python
```