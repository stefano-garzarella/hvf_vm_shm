#!/bin/bash

set -xe

clang -o prepare_vm_mem -O2 -mmacosx-version-min=11.0 prepare_vm_mem.c

clang -o simplevm -O2 -framework Hypervisor -mmacosx-version-min=11.0 simplevm.c
codesign --entitlements simplevm.entitlements --force -s - simplevm

#spctl --add simplevm

./prepare_vm_mem &

sleep 1

./simplevm
