#!/bin/bash

DIR=$1
MAX=$2

while /bin/true ; do 
    file=$((RANDOM % $MAX))
    new_file=$((RANDOM % MAX))
    ln $file $DIR/$new_file 2> /dev/null
done
