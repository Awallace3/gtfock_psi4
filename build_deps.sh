# Build Simint source code generator
#
# . /theoryfs2/ds/amwalla3/intel/oneapi/setvars.sh --config="/theoryfs2/ds/amwalla3/intel/oneapi/config-no-intelpython.txt" intel64
# . /theoryfs2/ds/amwalla3/intel/oneapi/mpi/2021.10.0/env/vars.sh
# . /theoryfs2/ds/amwalla3/intel/oneapi/compiler/2021.4.0/env/vars.sh
# . /theoryfs2/ds/amwalla3/intel/oneapi/compiler/2023.2.0/env/vars.sh # icc/icpc

export CC=icc
export CXX=icpc
# check if simint-generator exists
if [ ! -d simint-generator ]; then
    git submodule update --init --recursive
fi

cd simint-generator
rm -r build
mkdir build && cd build
# CC=$CC CXX=$CXX cmake ../ -DCMAKE_CXX_FLAGS="-fPIC -I/usr/include/x86_64-linux-gnu -std=c++14"
CC=$CC CXX=$CXX cmake .. -DCMAKE_CC_FLAGS="-fPIC -I/usr/include/x86_64-linux-gnu -std=c++14" -DCMAKE_CXX_FLAGS="-fPIC -I/usr/include/x86_64-linux-gnu -std=c++14"
make -j16
cd ..
# exit script
exit 0

# Generate Simint source code (requires Python3)
# Run ./create.py --help to see the details of the parameters
./create.py -g build/generator/ostei -l 5 -p 4 -d 0 -ve 4 -vg 5 -he 4 -hg 5 simint
rm -r ../simint
mv simint ../

# Compile Simint
cd ../simint  # Should at $WORK_TOP/simint
# See the README file in Simint directory to see which SIMINT_VECTOR variable you should use
# Commonly used SIMINT_VECTOR: commonavx512, avx2

mkdir build-avx512 && cd build-avx512
CC=$CC CXX=$CXX cmake ../ -DSIMINT_VECTOR=commonavx512 -DCMAKE_INSTALL_PREFIX=./install -DCMAKE_CXX_FLAGS="-fPIC"
make -j16 install
cd ../..


cd libcint
bash build.sh

cd ../GTMatrix
bash build.sh

cd ../GTFock
bash build.sh
