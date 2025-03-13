#! /usr/bin/env bash
pgrep -f 'px4' | xargs sudo kill -9
pgrep -f 'gz sim' | xargs sudo kill -9
