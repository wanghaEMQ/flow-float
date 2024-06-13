# Minimal flow language running environment

## Requires

* static boost libraries
* python3
* mono
* openssl

## Start

Add your project flowxxx to submodule.

```
cd flow-float
git submodule add https://github.com/xxx/flowxxx flowxxx
```

Add flowxxx to cmake.

```
add_subdirectory(flowxxx)
```

Compile and run.

```
mkdir _build
cd _build
cmake -G Ninja ..
ninja
```

```
./flowxxx/flowxxx
```

## Practice on ubuntu22.04

```
apt install vim g++ gcc git make ninja-build mono-devel libssl-dev libz-dev libbz2-dev libzstd-dev

# Install cmake
wget https://github.com/Kitware/CMake/releases/download/v3.29.3/cmake-3.29.3-linux-x86_64.sh
bash ./cmake-3.29.3-linux-x86_64.sh --skip-license --exclude-subdir --prefix=/usr/local

# Install boost static libraries
cd boost_1_78_0
./bootstrap.sh
./b2 link=static --with-iostreams install

git clone https://github.com/wanghaEMQ/flow-float.git
cd flow-float/
cp -r ~/flowxxx ./flowxxx
echo "add_subdirectory(flowxxx)" >> ./CMakeLists.txt
mkdir -p build && cd build
ninja -j4
```

## Thanks

Thanks follow projects.

```
https://github.com/jzhou77/flow-examples
```
