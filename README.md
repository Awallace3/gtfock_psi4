# gtfock_psi4
Provides a more standardized way to build simint and gtfock to be used in Psi4

# Install
1. We need to clone this repository recursively to clone the git submodules:
`git clone --recursive https://github.com/Awallace3/gtfock_psi4.git`
2. Create conda environment
`conda env create -f env.yml`
3. Activate conda environment
`conda activate p4devGTF`
4. Run `bash build_deps.sh`

## Objectives
- [ ] Build GTFock with gcc/g++ instead of icc/icpc
- [ ] switch from `build_deps.sh` to a more cmake friendly approach
    - [ ] submodules:
        - [X] libcint CMakeLists.txt
        - [X] ERD CMakeLists.txt
        - [X] OED CMakeLists.txt
        - [X] GTMatrix CMakeLists.txt
        - [ ] GTFock CMakeLists.txt
    - [ ] GTFock cmake builds it's dependencies
