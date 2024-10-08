# Build Simint source code generator
if [ ! -d simint-generator ]; then
    git submodule update --init --recursive
fi

export FC=gfortran
export MPICC=mpicc
export MPICXX=mpicxx
export WORK_TOP=$PWD

# SIMINT requires icc/icpc maybe?
# export CC=gcc
# export CXX=g++
export CC=icc
export CXX=icpc

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

export SIMINT_LIBRARY_DIR=$PWD/simint/build-avx512/install

# export CC=gcc
# export CXX=g++

#exit
# export CC=icc
# export CXX=icpc
# export FC=ifort


export ERD_OED_LIB=$PWD/OptErd_Makefile/external/lib
export ERD_LIBRARY_DIR=$PWD/OptErd_Makefile/external/share/cmake/erd
export OED_LIBRARY_DIR=$PWD/OptErd_Makefile/external/share/cmake/oed
export LIBCINT_LIBRARY_DIR=$PWD/libcint/share/cmake/CInt
export GTMATRIX_LIBRARY_DIR=$PWD/GTMatrix/share/cmake/GTMatrix

# if [ ! $ERD_OED_LIB/../include/erd_profile.h ]; then
    echo "Building ERD"
    rm $ERD_OED_LIB/liberd.a
    cd ./OptErd_Makefile/external/erd
    rm -r build
    mkdir build
    cmake -S. -Bbuild -G Ninja -DCMAKE_INSTALL_PREFIX=.. -DCMAKE_Fortran_COMPILER=x86_64-conda-linux-gnu-f95
    ninja -C build install
    cd ../../../
# fi
# if [ ! $ERD_OED_LIB/liberd.a ]; then
    rm $ERD_OED_LIB/liboed.a
    cd ./OptErd_Makefile/external/oed
    rm -r build
    cmake -S. -Bbuild -G Ninja -DCMAKE_INSTALL_PREFIX=.. -DCMAKE_Fortran_COMPILER=x86_64-conda-linux-gnu-f95 # from conda-forge
    ninja -C build install
    cd ../../..
# fi
# if [ ! -d $LIBCINT_LIBRARY_DIR ]; then
    echo "Building libcint"
    cd libcint
    export objdir=objdir_cint
    rm -r $objdir
    rm -r include
    rm -r lib
    mkdir -p $objdir
    cmake -S. -B${objdir} -G Ninja -DCMAKE_C_COMPILER=$CC -DCMAKE_CXX_COMPILER=$CXX  -DBUILD_SHARED_LIBS=ON -DCMAKE_PREFIX_PATH=${SIMINT_LIBRARY_DIR}  -Doed_DIR=$OED_LIBRARY_DIR -DCMAKE_INSTALL_PREFIX=. -Derd_DIR=$ERD_LIBRARY_DIR
    ninja -C ${objdir} install
    cd ..
# fi
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
    cd test
    make
    cd ../..
# fi

echo "Building GTFock"
cd GTFock
# rm -rf build
# export AR=xiar rcs

echo $MPICC
echo $MPICXX

export prefix_path="$SIMINT_LIBRARY_DIR"

# if [ ! -d ./build ]; then
    rm -rf build
    mkdir -p build
    cmake -S. -Bbuild -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCInt_DIR=${LIBCINT_LIBRARY_DIR} -DGTMatrix_DIR=${GTMATRIX_LIBRARY_DIR} -Derd_DIR=$ERD_LIBRARY_DIR -Doed_DIR=$OED_LIBRARY_DIR -DCMAKE_C_COMPILER=$CC -DCMAKE_CXX_COMPILER=$CXX -G Ninja -DCMAKE_INSTALL_PREFIX=. -DCMAKE_PREFIX_PATH=${prefix_path} 
# fi
ninja -C build 
cd ..
