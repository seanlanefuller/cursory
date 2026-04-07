#!/bin/bash

rm /tmp/cursory*
make clean
make
cd test
rm cursory.log
../cursory
cd ..
