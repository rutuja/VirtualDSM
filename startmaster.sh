#!/bin/bash
unbuffer ./test master 127.0.0.1 127.0.0.1 $1 $2 $3 &> $3.master.log
