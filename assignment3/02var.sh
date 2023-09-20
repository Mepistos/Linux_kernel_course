#!/bin/sh
myvar="Hi sys"
echo $myvar
echo "$myvar"
echo '$myvar'
echo \$myvar
echo type something you want:
read myvar
echo '$myvar' = $myvar
exit 0

