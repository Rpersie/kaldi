#!/bin/bash
# 1f uses cmvn (both). 1f_noVar only normalizes mean.
# local/chain/compare_wer_general.sh e2e_1f e2e_1f_noVar e2e_1f_noL2_6ep
# System                   e2e_1f e2e_1f_noVar e2e_1f_noL2_6ep
# WER on train_dev(tg)      29.85     30.49     30.04
# WER on train_dev(fg)      28.48     29.06     28.70
# WER on eval2000(tg)        30.6      31.4      30.8
# WER on eval2000(fg)        29.2      29.8      29.4
# Final train prob         -0.318    -0.321    -0.283
# Final valid prob         -0.339    -0.348    -0.301
# Final train prob (xent)         0.000     0.000     0.000
# Final valid prob (xent)        0.0000    0.0000    0.0000

# local/chain/compare_wer_general.sh e2e_1f_l2n1fifth
# System                e2e_1f_l2n1fifth
# WER on train_dev(tg)      29.56
# WER on train_dev(fg)      28.24
# WER on eval2000(tg)        30.8
# WER on eval2000(fg)        29.2
# Final train prob         -0.293
# Final valid prob         -0.307


# System                exp/chain/e2e_1f_maxch3:acwt=0.5  10xpost
# WER on train_dev(tg)      28.43
# WER on train_dev(fg)      26.93
# WER on eval2000(tg)        29.2
# WER on eval2000(fg)        27.6
# Final train prob         -0.289
# Final valid prob         -0.318

# System                exp/chain/e2e_1f_maxch3: acwt=1.2 10xpost
# WER on train_dev(tg)      28.59
# WER on train_dev(fg)      27.27
# WER on eval2000(tg)        29.2
# WER on eval2000(fg)        27.8
# Final train prob         -0.289
# Final valid prob         -0.318


# System                exp/chain/e2e_1f_maxch3: acwt=0.75
# WER on train_dev(tg)      28.44
# WER on train_dev(fg)      26.93
# WER on eval2000(tg)        34.9
# WER on eval2000(fg)        27.5
# Final train prob         -0.289
# Final valid prob         -0.318

# local/chain/compare_wer_general.sh exp/chain/e2e_1f_maxch3: acwt:0.9
# System                exp/chain/e2e_1f_maxch3/
# WER on train_dev(tg)      28.51
# WER on train_dev(fg)      27.05
# WER on eval2000(tg)        29.1
# WER on eval2000(fg)        27.6
# Final train prob         -0.289
# Final valid prob         -0.318



# TO TRY: full set (no nodup)
set -e

# configs for 'chain'
affix=
stage=12
train_stage=-10
get_egs_stage=-10
speed_perturb=true
dir=exp/chain/e2e_1f  # Note: _sp will get added to this if $speed_perturb == true.
decode_iter=

# training options
num_epochs=5
initial_effective_lrate=0.001
final_effective_lrate=0.0001
max_param_change=2.0
final_layer_normalize_target=0.5
num_jobs_initial=3
num_jobs_final=16
minibatch_size=150=128,64/300=128,64,32/600=64,32,16/1200=16,8
remove_egs=false
common_egs_dir=exp/chain/e2e_1f/egs
no_mmi_percent=101
l2_regularize=0.00005
dim=800
frames_per_iter=2500000
cmvn_opts="--norm-means=true --norm-vars=true"
leaky_hmm_coeff=0.1
hid_max_change=0.75
final_max_change=1.5
self_repair=1e-5
acwt=1.2
post_acwt=12.0
num_scale_opts="--transition-scale=0.0 --self-loop-scale=0.0"
shared_phones_opt=
equal_align_iters=1000
den_use_initials=true
den_use_finals=false

# End configuration section.
echo "$0 $@"  # Print the command line for logging

. ./cmd.sh
. ./path.sh
. ./utils/parse_options.sh

if ! cuda-compiled; then
  cat <<EOF && exit 1
This script is intended to be used with GPUs but you have not compiled Kaldi with CUDA
If you want to use GPUs (and have them), go to src/, and configure and make on a machine
where "nvcc" is installed.
EOF
fi

train_set=train_nodup_seg_sp
lang=data/lang_chain_2y
treedir=exp/chain/e2e_tree_a

#local/nnet3/run_e2e_common.sh --stage $stage \
#  --speed-perturb $speed_perturb \
#  --generate-alignments $speed_perturb || exit 1;

if [ $stage -le 10 ]; then
  # Create a version of the lang/ directory that has one state per phone in the
  # topo file. [note, it really has two states.. the first one is only repeated
  # once, the second one has zero or more repeats.]
  rm -rf $lang
  cp -r data/lang $lang
  silphonelist=$(cat $lang/phones/silence.csl) || exit 1;
  nonsilphonelist=$(cat $lang/phones/nonsilence.csl) || exit 1;
  # Use our special topology... note that later on may have to tune this
  # topology.
  steps/nnet3/chain/gen_topo.py $nonsilphonelist $silphonelist >$lang/topo
fi

if [ $stage -le 11 ]; then
  steps/nnet3/chain/prepare_e2e.sh --nj 30 --cmd "$train_cmd" \
                                   --scale-opts "$num_scale_opts" \
                                   --shared-phones-opt "$shared_phones_opt" \
                                   data/$train_set $lang $treedir
fi

if [ $stage -le 12 ]; then
  echo "$0: creating neural net configs using the xconfig parser";

  num_targets=$(tree-info $treedir/tree |grep num-pdfs|awk '{print $2}')
  #learning_rate_factor=$(echo "print 0.5/$xent_regularize" | python)

  mkdir -p $dir/configs
  cat <<EOF > $dir/configs/network.xconfig
#  input dim=100 name=ivector
  input dim=40 name=input

  # please note that it is important to have input layer with the name=input
  # as the layer immediately preceding the fixed-affine-layer to enable
  # the use of short notation for the descriptor
#  fixed-affine-layer name=lda input=Append(-1,0,1) affine-transform-file=$dir/configs/lda.mat

  # the first splicing is moved before the lda layer, so no splicing here
  relu-layer name=tdnn1 input=Append(-1,0,1) dim=$dim max-change=$hid_max_change self-repair-scale=$self_repair
  relu-layer name=tdnn2 input=Append(-1,0,1) dim=$dim max-change=$hid_max_change self-repair-scale=$self_repair
  relu-layer name=tdnn3 input=Append(-1,0,1) dim=$dim max-change=$hid_max_change self-repair-scale=$self_repair
  relu-layer name=tdnn4 input=Append(-3,0,3) dim=$dim max-change=$hid_max_change self-repair-scale=$self_repair
  relu-layer name=tdnn5 input=Append(-3,0,3) dim=$dim max-change=$hid_max_change self-repair-scale=$self_repair
  relu-layer name=tdnn6 input=Append(-3,0,3) dim=$dim max-change=$hid_max_change self-repair-scale=$self_repair
  relu-layer name=tdnn7 input=Append(-3,0,3) dim=$dim max-change=$hid_max_change self-repair-scale=$self_repair

  ## adding the layers for chain branch
  relu-layer name=prefinal-chain input=tdnn7 dim=$dim target-rms=$final_layer_normalize_target self-repair-scale=$self_repair
  output-layer name=output include-log-softmax=true dim=$num_targets max-change=$final_max_change

EOF
  steps/nnet3/xconfig_to_configs.py --xconfig-file $dir/configs/network.xconfig --config-dir $dir/configs/
fi

if [ $stage -le 13 ]; then
  if [[ $(hostname -f) == *.clsp.jhu.edu ]] && [ ! -d $dir/egs/storage ]; then
    utils/create_split_dir.pl \
     /export/b0{5,6,7,8}/$USER/kaldi-data/egs/swbd-$(date +'%m_%d_%H_%M')/s5c/$dir/egs/storage $dir/egs/storage
  fi

#    --feat.online-ivector-dir exp/nnet3/ivectors_${train_set} \

  steps/nnet3/chain/train_e2e.py --stage $train_stage \
    --cmd "$decode_cmd" \
    --feat.cmvn-opts "$cmvn_opts" \
    --chain.leaky-hmm-coefficient $leaky_hmm_coeff \
    --chain.l2-regularize $l2_regularize \
    --chain.apply-deriv-weights false \
    --chain.lm-opts="--num-extra-lm-states=2000" \
    --egs.dir "$common_egs_dir" \
    --egs.stage $get_egs_stage \
    --egs.opts "--normalize-egs false" \
    --trainer.options="--compiler.cache-capacity=512 --den-use-initials=$den_use_initials --den-use-finals=$den_use_finals" \
    --trainer.no-mmi-percent $no_mmi_percent \
    --trainer.equal-align-iters $equal_align_iters \
    --trainer.num-chunk-per-minibatch $minibatch_size \
    --trainer.frames-per-iter $frames_per_iter \
    --trainer.num-epochs $num_epochs \
    --trainer.optimization.num-jobs-initial $num_jobs_initial \
    --trainer.optimization.num-jobs-final $num_jobs_final \
    --trainer.optimization.initial-effective-lrate $initial_effective_lrate \
    --trainer.optimization.final-effective-lrate $final_effective_lrate \
    --trainer.max-param-change $max_param_change \
    --cleanup.remove-egs $remove_egs \
    --feat-dir data/${train_set} \
    --tree-dir $treedir \
    --dir $dir  || exit 1;

fi
#mv $dir/final.mdl $dir/final_wop.mdl; nnet3-am-adjust-priors $dir/final_wop.mdl $dir/priors.vec $dir/final.mdl

if [ $stage -le 14 ]; then
  # Note: it might appear that this $lang directory is mismatched, and it is as
  # far as the 'topo' is concerned, but this script doesn't read the 'topo' from
  # the lang directory.
  utils/mkgraph.sh --self-loop-scale 1.0 data/lang_sw1_tg $dir $dir/graph_sw1_tg
fi

#          --online-ivector-dir exp/nnet3/ivectors_${decode_set} \

decode_suff=sw1_tg
graph_dir=$dir/graph_sw1_tg
if [ $stage -le 15 ]; then
  iter_opts=
  if [ ! -z $decode_iter ]; then
    iter_opts=" --iter $decode_iter "
  fi
  for decode_set in train_dev eval2000; do
      (
      steps/nnet3/decode.sh --acwt $acwt --post-decode-acwt $post_acwt \
          --nj 50 --cmd "$decode_cmd" $iter_opts \
          $graph_dir data/${decode_set}_hires $dir/decode_${decode_set}${decode_iter:+_$decode_iter}_${decode_suff} || exit 1;
      if $has_fisher; then
          steps/lmrescore_const_arpa.sh --cmd "$decode_cmd" \
            data/lang_sw1_{tg,fsh_fg} data/${decode_set}_hires \
            $dir/decode_${decode_set}${decode_iter:+_$decode_iter}_sw1_{tg,fsh_fg} || exit 1;
      fi
      ) &
  done
fi
wait;
exit 0;
