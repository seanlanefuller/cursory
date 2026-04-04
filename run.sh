#!/bin/bash

make clean
make
cd test
rm cursory.log
../cursory
cd ..
