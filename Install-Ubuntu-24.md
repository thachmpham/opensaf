# Build & Install OpenSaf on Ubuntu 24

## Dependencies
```sh

$ sudo apt-get install libc6-dev libxml2-dev libxml2-utils libsqlite3-dev libncurses5-dev libreadline6-dev libmnl-dev autoconf automake libtool pkg-config lsb-base make gcc g++ xterm git

$ apt install -y mercurial m4 python-dev-is-python3

```


## Build & Install
```sh
$ autoreconf -f -i

$ ./configure CPPFLAGS="-DRUNASROOT" CXXFLAGS="-Wno-error -w -Wl,-z,muldefs" CFLAGS="-Wno-error -Wl,-z,muldefs" PYTHON="/usr/bin/python3"

$ ./configure CPPFLAGS="-DRUNASROOT" CXXFLAGS="-Wno-error -w -Wl,--unresolved-symbols=ignore-all" CFLAGS="-Wno-error -Wl,--unresolved-symbols=ignore-all"

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
