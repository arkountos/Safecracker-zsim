#!/bin/bash

scp -r -P 2222 vagrant@localhost:/vagrant/ .
git add *
git commit -m "$1"
git pull
git push
