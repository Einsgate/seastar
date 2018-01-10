#!/bin/bash

E_WRONGARGS=85
N_FILES=${#}

if ((N_FILES < 1))
then
	echo "Lack of source file."
	exit $E_WRONGARGS
fi

for file in $*
do
	case $file in
	*.cc	) ;;
	*	) echo "Incorrect file type (needs .cc file).";
	  	  exit $E_WRONGARGS;;	
	esac
done


SRC_FILE_NAME=$1
SRC_FILE_NAME_LEN=${#SRC_FILE_NAME}
EX_FILE_NAME_LEN=$((SRC_FILE_NAME_LEN - 3))
EX_FILE_NAME=${SRC_FILE_NAME:0:$EX_FILE_NAME_LEN}

c++ `pkg-config --cflags --libs $SEASTAR/build/release/seastar.pc` $* -o $EX_FILE_NAME &> log
cat log
