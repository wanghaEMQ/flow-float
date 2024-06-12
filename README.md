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
