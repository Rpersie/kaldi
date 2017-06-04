#!/bin/bash
# Copyright 2017  Johns Hopkins University (Author: Hossein Hadian)
# Apache 2.0


# To be run from ..
# Flat start chain model training.

# Begin configuration section.
cmd=run.pl
nj=4
stage=0
# End configuration section.

echo "$0 $@"  # Print the command line for logging

if [ -f path.sh ]; then . ./path.sh; fi
. parse_options.sh || exit 1;

if [ $# != 3 ]; then
  echo "Usage: steps/prepare_e2e.sh [options] <data-dir> <lang-dir> <exp-dir>"
  echo " e.g.: steps/prepare_e2e.sh data/train_seg_sp data/lang_chain exp/chain/e2e"
  echo "main options (for others, see top of script file)"
  echo "  --config <config-file>                           # config containing options"
  echo "  --cmd (utils/run.pl|utils/queue.pl <queue opts>) # how to run jobs."
  exit 1;
fi

data=$1
lang=$2
dir=$3

oov_sym=`cat $lang/oov.int` || exit 1;

mkdir -p $dir/log

echo $nj > $dir/num_jobs
sdata=$data/split$nj;
[[ -d $sdata && $data/feats.scp -ot $sdata ]] || split_data.sh $data $nj || exit 1;

cp $lang/phones.txt $dir || exit 1;

echo "$0: Initializing monophone system."

[ ! -f $lang/phones/sets.int ] && exit 1;
shared_phones_opt="--shared-phones=$lang/phones/sets.int"

if [ $stage -le 0 ]; then
  $cmd $dir/log/init_mono_mdl_tree.log \
    gmm-init-mono $shared_phones_opt $lang/topo 10 \
    $dir/0.mdl $dir/tree || exit 1;
  copy-transition-model $dir/0.mdl $dir/0.trans_mdl
fi

if [ $stage -le 1 ]; then
  echo "$0: Compiling training graphs"
  $cmd JOB=1:$nj $dir/log/compile_graphs.JOB.log \
    compile-train-graphs --read-disambig-syms=$lang/phones/disambig.int $dir/tree $dir/0.mdl $lang/L.fst \
    "ark:sym2int.pl --map-oov $oov_sym -f 2- $lang/words.txt < $sdata/JOB/text|" \
    "ark,scp:$dir/fst.JOB.ark,$dir/fst.JOB.scp" || exit 1;
fi
