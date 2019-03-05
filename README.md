# bcs
Simulates the behaviour of systems written in the beacon calculus.  Development was done using gcc 5.4.0 on an Ubuntu 16.04 platform.

## Downloading and Compiling bcs
Clone the bcs repository by running:
```shell
git clone https://github.com/MBoemo/bcs.git
```
The bcs directory will appear in your current directory.  bcs was written in C++11 and uses OpenMP for parallel processing, but these are standard on most systems; there are no other third party dependencies.  Compile the software by running:
```shell
cd bcs
make
```
This will put the bcs executable into the bcs/bin directory.  bcs was tested to compile and run on both OSX and Linux systems.

## Manual
A manual for writing models in the beacon calculus and using bcs is available at https://www.michaelboemo.com/beacon-calculus-simulator.  It includes:
- quick-start tutorials,
- a bank of examples,
- a full list of features.

## Questions and Comments
If you have comments or suggestions for the beacon calculus, the bcs software, or the manual, please Email michael.boemo@path.ox.ac.uk.  Should any bugs arise, please report them to https://github.com/MBoemo/bcs/issues. 