#!/bin/bash
unbuffer ./test slave 127.0.0.1 127.0.0.1 $1 $2 $3 &> $3.slave.log
