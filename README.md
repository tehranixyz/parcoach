# PARCOACH (PARallel COntrol flow Anomaly CHecker)

**PARCOACH** automatically checks parallel applications to verify the correct use of collectives. This is **PARCOACH**. This version uses an inter-procedural control- and data-flow analysis to detect potential errors at compile-time.

## News

* <b> Parcoach is now on github! </b>



## Getting Started

These instructions will get you a copy of the project up and running on your local machine.

### Prerequisites

#### CMake `>= 3.1`

CMake can be downloaded from [http://www.cmake.org](http://www.cmake.org).

#### LLVM `= 3.9.1`

This version of PARCOACH is an LLVM pass. To get LLVM, follow these steps:

```bash
git clone -b llvmorg-3.9.1 --depth=1 --single-branch https://github.com/llvm/llvm-project.git
cd llvm-project/
mkdir build && cd build/
cmake ../llvm/ -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_ENABLE_PROJECTS='clang,libcxx,libcxxabi,compiler-rt' -DCMAKE_BUILD_TYPE=Release -G "Unix Makefiles"
make -j8
make install 
```

### Installation

First make sure to have installed a C++11 compiler, CMake and Git. Then install PARCOACH by running:

```bash
git clone https://github.com/parcoach/parcoach.git
mkdir parcoach/build
cd parcoach/build
cmake ..
make -j8
```

If CMake does not find LLVM, you can supply the path to your LLVM installation as follows  :
```bash
cmake .. -DCMAKE_PREFIX_PATH=/path/to/llvm-3.9.1/
```

## Usage
Codes with errors can be found in the [Parcoach Microbenchmark Suite](https://github.com/parcoach/microbenchmarks).

### Static checking

PARCOACH is an LLVM pass that can be run with the [opt](http://llvm.org/docs/CommandGuide/opt.html) tool. This tool makes part of LLVM and is already included with your installation of LLVM `3.9.1`. It takes as input LLVM bytecode.

#### 1) First, compile each file from your program with clang. Use the `-flto` option to generate LLVM bytecode:
```bash
clang -c -g -flto file1.c -o file1.o
clang -c -g -flto file2.c -o file2.o
clang -c -g -flto main.c -o main.o
```
 
 Do not forget to supply the `-g` option, so PARCOACH can provide more precise debugging information.
 
#### 2) Then, link all object files
```bash
clang -flto file1.o file2.o -o main.o -o main
```

#### 3) Finally, run the PARCOACH pass on the generated LLVM bytecode:
```bash
opt -load /path/to/parchoach/build/src/aSSA/libaSSA.so -parcoach -check-mpi < main
```

### Runtime checking

Coming soon

## Publications
Pierre Huchant, Emmanuelle Saillard, Denis Barthou and Patrick Carribault,
**[Multi-Valued Expression Analysis for Collective Checking](https://link.springer.com/chapter/10.1007%2F978-3-030-29400-7_3)**
*Euro-Par 2019: Parallel Processing, pages 29-43, 2019*

Pierre Huchant, Emmanuelle Saillard, Denis Barthou, Hugo Brunie and Patrick Carribault,  
**[PARCOACH Extension for a Full-Interprocedural Collectives Verification](https://doi.org/10.1109/Correctness.2018.00013)**,  
*2nd International Workshop on Software Correctness for HPC Applications (Correctness), pages 69-76, 2018*

Emmanuelle Saillard, Patrick Carribault, and Denis Barthou,  
**[PARCOACH: Combining static and dynamic validation of MPI collective communications](https://doi.org/10.1177%2F1094342014552204)**,  
*Intl. Journal on High Performance Computing Applications (IJHPCA), 28(4):425-434, 2014*

Emmanuelle Saillard, Patrick Carribault, and Denis Barthou,  
**[Combining Static and Dynamic Validation of MPI Collective Communications](https://doi.org/10.1145/2488551.2488555)**,  
*Proceedings of the European MPI User’s Group Meeting (EuroMPI), pages 117-122, 2013*

Emmanuelle Saillard, Patrick Carribault, and Denis Barthou,  
**[MPI Thread-Level Checking for MPI+OpenMP Applications](https://doi.org/10.1007/978-3-662-48096-0_3)**,  
*European Conference on Parallel Processing (EuroPar), pages 31-42, 2015*

Emmanuelle Saillard, Patrick Carribault, and Denis Barthou,  
**[Static Validation of Barriers and Worksharing Constructs in OpenMP Applications](https://doi.org/10.1007/978-3-319-11454-5_6)**,  
*Proc. IntL. Workshop on OpenMP (IWOMP), volume 8766 of Lect. Notes in Computer Science, pages 73-86, 2014*

Emmanuelle Saillard, Hugo Brunie, Patrick Carribault, and Denis Barthou,  
**[PARCOACH extension for Hybrid Applications with Interprocedural Analysis](https://doi.org/10.1007/978-3-319-39589-0_11)**,  
*In Tools for High Performance Computing 2015: Proceedings of the 9th International Workshop on Parallel Tools for High Performance Computing, pages 135-146, 2016*


## License
The project is licensed under the LGPL 2.1 license.
## External links

- [Official website](https://parcoach.github.io)
- [Parcoach Microbenchmark Suite](https://github.com/parcoach/microbenchmarks)
