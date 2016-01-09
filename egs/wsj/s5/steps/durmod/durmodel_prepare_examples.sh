#!/bin/bash

# Copyright 2015 Hossein Hadian

min_repeat_count=2
max_duration=0
left_context=4
right_context=2
cmd=run.pl

[ -f ./path.sh ] && . ./path.sh; # source the path.
. parse_options.sh || exit 1;

if [ $# != 3 ]; then
   echo "Usage: $0 [options] <phones-dir> <ali-dir> <duration-model-dir>"
   echo "... where <duration-model-dir> is assumed to be a sub-directory "
   echo " of the directory where the transition model (i.e. final.mdl) is."
   echo "e.g.: $0 data/lang/phones exp/mono_ali exp/mono/durmod"
   echo ""
   echo "Main options (for others, see top of script file):"
   echo "  --min-repeat-count <count>                     # minimum number of occurence for a duration; used to determine max-duration"
   echo "  --max-duration <duration-in-frames>            # max duration; if not set, it will be determined automatically"   
   echo "  --left-context <size>                          # left phone context size"   
   echo "  --right-context <size>                         # right phone context size"   
   exit 1;
fi

phonesdir=$1
alidir=$2
dir=$3

srcdir=`dirname $dir`
durmodel=$dir/durmodel.mdl
transmodel=$srcdir/final.mdl

for f in $transmodel $alidir/ali.1.gz $phonesdir/roots.int $phonesdir/extra_questions.int; do
  [ ! -f $f ] && echo "$0: Required file not found: $f" && exit 1;
done

rm $dir/all.ali 2>/dev/null
mkdir -p $dir/log || exit 1;
gunzip -c $alidir/ali.*.gz >> $dir/all.ali || exit 1;

if [ $max_duration == 0 ]; then
  echo "$0: Determining max-duration..."
  $cmd $dir/log/ali_to_phones.log \
       ali-to-phones --write-lengths $transmodel ark:$dir/all.ali \
                     ark,t:$dir/all.length_ali || exit 1;
  max_duration=`python steps/durmod/find_max_duration.py $dir/all.length_ali \
                $min_repeat_count | grep max-duration | awk '{print $2}'` || exit 1;
fi
echo "$0: Max duration: $max_duration" 

$cmd $dir/log/durmod_init.log \
     durmod-init --max-duration=$max_duration \
                 --left-context=$left_context \
                 --right-context=$right_context \
                 $phonesdir/roots.int $phonesdir/extra_questions.int \
                 $durmodel || exit 1;
$cmd $dir/log/durmod_make_examples.log \
     durmod-make-egs $durmodel $transmodel ark:$dir/all.ali ark,t:$dir/all.egs || exit 1;

echo "$0: Done"
