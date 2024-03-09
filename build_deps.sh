# Build Simint source code generator
#
# . /theoryfs2/ds/amwalla3/intel/oneapi/setvars.sh --config="/theoryfs2/ds/amwalla3/intel/oneapi/config-no-intelpython.txt" intel64
# . /theoryfs2/ds/amwalla3/intel/oneapi/mpi/2021.10.0/env/vars.sh
# . /theoryfs2/ds/amwalla3/intel/oneapi/compiler/2021.4.0/env/vars.sh
# . /theoryfs2/ds/amwalla3/intel/oneapi/compiler/2023.2.0/env/vars.sh # icc/icpc

# export CC=icc
# export CXX=icpc
# check if simint-generator exists
if [ ! -d simint-generator ]; then
    git submodule update --init --recursive
fi

export CC=$CONDA_PREFIX/bin/gcc
export CXX=$CONDA_PREFIX/bin/g++
export MPICC=$CONDA_PREFIX/bin/mpicc
export MPICXX=$CONDA_PREFIX/bin/mpicxx

export WORK_TOP=$PWD

# if [ ! -d simint ]; then
    cd simint-generator
    rm -r build
    mkdir build && cd build
    CC=$CC CXX=$CXX cmake ../ # -DCMAKE_CC_FLAGS="-fPIC -I/usr/include/x86_64-linux-gnu -std=c++14" -DCMAKE_CXX_FLAGS="-fPIC -I/usr/include/x86_64-linux-gnu -std=c++14"
    make -j8
    cd ..

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
    CC=$CC CXX=$CXX cmake ../ -DSIMINT_VECTOR=commonavx512 -DCMAKE_INSTALL_PREFIX=./install
    make -j8 install
    cd ../..
# fi
# exit

# if [ ! -d erd ]; then
#     git clone git@github.com:psi4/erd.git
#     cd erd
#     cmake -H. -Bobjdir -DCMAKE_INSTALL_PREFIX=./install # -DCMAKE_Fortran_FLAGS="${CMAKE_Fortran_FLAGS} -fno-underscoring"
#     cd objdir && make
#     make install
#     cd ../../
# fi


export SIMINT_LIBRARY_DIR=$PWD/simint/build-avx512/install
export ERD_STATIC=$PWD/erd/install/lib64/liberd.a
export LIBCINT_LIBRARY_DIR=$PWD/libcint/share/cmake/CInt
export GTMATRIX_LIBRARY_DIR=$PWD/GTMatrix/share/cmake/GTMatrix


# if [ ! -d $LIBCINT_LIBRARY_DIR ]; then
    echo "Building libcint"
    cd libcint
    export objdir=objdir_cint
    rm -r $objdir
    rm -r include
    rm -r lib
    mkdir -p $objdir
    cmake -S. -B${objdir} -G Ninja -DCMAKE_C_COMPILER=$CC -DCMAKE_CXX_COMPILER=$CXX  -DBUILD_SHARED_LIBS=ON -DCMAKE_PREFIX_PATH=${SIMINT_LIBRARY_DIR} -DCMAKE_INSTALL_PREFIX=. 
    ninja -C ${objdir} install
    cd ..
# fi
exit

# if [ ! -d $GMATRIX_LIBRARY_DIR]; then
    cd GTMatrix
    echo "Building GTMatrix"
    rm -r build
    rm -r include
    rm -r lib
    mkdir -p build
    # cmake -S. -Bbuild -G Ninja .. -DCMAKE_INSTALL_PREFIX=.. -DCMAKE_C_COMPILER=$CC -DCMAKE_PREFIX_PATH=${SIMINT_LIBRARY_DIR}
    cmake -S. -Bbuild -G Ninja .. -DCMAKE_C_COMPILER=$CC -DCMAKE_PREFIX_PATH=${SIMINT_LIBRARY_DIR} -DCMAKE_INSTALL_PREFIX=.
    ninja -C build install
    cd ..
# fi

echo "Building GTFock"
cd GTFock
rm -rf build
mkdir -p build
# export AR=xiar rcs

export MPICC=$CONDA_PREFIX/bin/mpicc
export MPICXX=$CONDA_PREFIX/bin/mpicxx

echo $MPICC
echo $MPICXX

export prefix_path="$SIMINT_LIBRARY_DIR"

cmake -S. -Bbuild -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCInt_DIR=${LIBCINT_LIBRARY_DIR} -DGTMatrix_DIR=${GTMATRIX_LIBRARY_DIR} -DCMAKE_C_COMPILER=$CC -DCMAKE_CXX_COMPILER=$CXX -DCMAKE_MPICC_COMPILER=$MPICC -DCMAKE_MPICXX_COMPILER=$MPICXX -G Ninja -DCMAKE_INSTALL_PREFIX=. -DCMAKE_PREFIX_PATH=${prefix_path}
ninja -C build 
cd ..
