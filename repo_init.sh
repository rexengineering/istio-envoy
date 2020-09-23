#!/bin/bash
set -ex
git remote add sync https://github.com/istio/envoy.git
git pull --all
git checkout --track sync/master
git checkout rex/master
