#!/bin/bash
# Script to convert defines in compiler option in to C's defines
# Should be executed in make file and it take ccflags-y as the
# compiler options. The content will be redirected to the first arguement.

echo "#ifndef __SSV_CONF_PARSER_H__" >> $1
echo "#define __SSV_CONF_PARSER_H__" >> $1

echo "char const *conf_parser[] = {" >> $1

for flag in ${ccflags-y}; do
	if [[ "$flag" =~ ^-D.* ]]; then
		def=${flag:2}
        if [[ "$def" =~ .= ]]; then
            #def_1=`echo $def | awk -F'=' '{print $def_1}'`
            echo "//\"$def\"," >> $1
        else
		    echo "\"$def\"," >> $1
        fi
	fi
done

echo "\"\"};" >> $1

echo "#endif // __SSV_CONF_PARSER_H__" >> $1
