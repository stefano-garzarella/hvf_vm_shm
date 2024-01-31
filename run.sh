#!/bin/bash

set -xe

clang -o simplevm -O2 -framework Hypervisor -mmacosx-version-min=11.0 simplevm.c

codesign --entitlements simplevm.entitlements --force -s - simplevm

#spctl --add simplevm
