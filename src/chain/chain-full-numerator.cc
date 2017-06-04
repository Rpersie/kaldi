// chain/chain-full-numerator.cc

// Copyright      2015   Hossein Hadian

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.


#include "chain/chain-full-numerator.h"
#include "chain/chain-kernels-ansi.h"

namespace kaldi {
namespace chain {

FullNumeratorComputation::FullNumeratorComputation(
                                    const ChainTrainingOptions &opts,
                                    const NumeratorGraph &num_graph,
                                    const CuMatrixBase<BaseFloat> &nnet_output):
    opts_(opts),
    num_graph_(num_graph),
    num_sequences_(num_graph.NumSequences()),
    frames_per_sequence_(nnet_output.NumRows() / num_sequences_),
    exp_nnet_output_transposed_(nnet_output, kTrans),
    nnet_output_deriv_transposed_(
        exp_nnet_output_transposed_.NumRows(),
        std::min<int32>(exp_nnet_output_transposed_.NumCols(),
                        static_cast<int32>(kMaxDerivTimeSteps) *
                        num_sequences_)),
    alpha_(frames_per_sequence_ + 1,
           num_graph_.MaxNumStates() * num_sequences_ + num_sequences_,
           kSetZero),
    // we actually do not need beta for state num_graph_.MaxNumStates(),
    // so no "+ num_sequences_"
    beta_(2, num_graph_.MaxNumStates() * num_sequences_,
          kSetZero),
    tot_prob_(num_sequences_, kUndefined),
    tot_log_prob_(num_sequences_, kUndefined),
    ok_(true) {
  //KALDI_ASSERT(opts_.leaky_hmm_coefficient > 0.0 &&
  //             opts_.leaky_hmm_coefficient < 1.0);

  KALDI_ASSERT(nnet_output.NumRows() % num_sequences_ == 0);
  exp_nnet_output_transposed_.ApplyExp();
//  std::cout << "exp nnet output transposed: \n";
//  exp_nnet_output_transposed_.Write(std::cout, false);
}


void FullNumeratorComputation::AlphaFirstFrame() {
  // select alpha for time 0
  BFloat *first_frame_alpha = alpha_.RowData(0);
  // now make a view of the first num_sequences elements (i.e. alpha_0(0)
  // for all sequences)
  // initializer takes [pointer, length].
  SubVector<BFloat> alpha_hmm_state0(first_frame_alpha, num_sequences_);
  // set alpha_0(0) for all sequences to 1.0 and leave the rest to be 0.0.
  // i.e. the only start state is state 0.
  // alpha_hmm_state0.Set(1.0e-200);
  //  alpha_hmm_state0.Write(std::cout, false);
  alpha_hmm_state0.Set(1.0);
  // Now compute alpha-sums for t==0 which is obviously 1.0 for each sequence
  SubVector<BFloat> alpha_sum_vec(
                                     first_frame_alpha +
                                     num_graph_.MaxNumStates() * num_sequences_,
                                     num_sequences_);
  alpha_sum_vec.Set(1.0);
  // KALDI_LOG << "al-sum for seq=7, t=" << 0 << " is " << alpha_sum_vec(7);
}


// the alpha computation for some 0 < t <= num_time_steps_.
void FullNumeratorComputation::AlphaGeneralFrame(int32 t) {
  KALDI_ASSERT(t > 0 && t <= frames_per_sequence_);
  BFloat *this_alpha = alpha_.RowData(t);
  const BFloat *prev_alpha = alpha_.RowData(t - 1);
  const Int32Pair *backward_transitions = num_graph_.BackwardTransitions();
  const DenominatorGraphTransition *transitions = num_graph_.Transitions();
  int32 num_pdfs = exp_nnet_output_transposed_.NumRows(),
      max_num_hmm_states = num_graph_.MaxNumStates(),
      num_sequences = num_sequences_;

  // 'probs' is the matrix of pseudo-likelihoods for frame t - 1.
  SubMatrix<BaseFloat> probs(exp_nnet_output_transposed_, 0, num_pdfs,
                               (t - 1) * num_sequences_, num_sequences_);
  const BaseFloat *prob_data = probs.Data();

  int32 prob_stride = probs.Stride();
  for (int32 s = 0; s < num_sequences; s++) {
    BFloat inv_arbitrary_scale =
        prev_alpha[max_num_hmm_states * num_sequences + s];  //#SCC#
    for (int32 h = 0; h < num_graph_.NumStates()[s]; h++) {
      double this_tot_alpha = 0.0;
      const DenominatorGraphTransition
          *trans_iter = transitions +
            backward_transitions[s*max_num_hmm_states+h].first,
          *trans_end = transitions +
            backward_transitions[s*max_num_hmm_states+h].second;
      for (; trans_iter != trans_end; ++trans_iter) {
        BFloat transition_prob = trans_iter->transition_prob;
        int32 pdf_id = trans_iter->pdf_id,
              prev_hmm_state = trans_iter->hmm_state;
        BFloat prob = prob_data[pdf_id * prob_stride + s],
            this_prev_alpha = prev_alpha[prev_hmm_state * num_sequences + s];
        this_tot_alpha += this_prev_alpha / inv_arbitrary_scale * transition_prob * prob; //#SCC#

        if (this_tot_alpha - this_tot_alpha != 0) {
          //KALDI_LOG << "t: " << t << ", seq: " << s << ", h: " << h
          //<< ", prev-alpha: " << this_prev_alpha
          //<< ", prob: " << prob
          //<< ", inv_arbitrary_scale: " << inv_arbitrary_scale;
          //KALDI_ERR << "Beta failure.";
        }


      }
      // Let arbitrary_scale be the inverse of the alpha-sum value that we
      // store in the same place we'd store the alpha for the state numbered
      // 'max_num_hmm_states'. We multiply this into all the
      // transition-probabilities from the previous frame to this frame, in
      // both the forward and backward passes, in order to keep the alphas in
      // a good numeric range.  This won't affect the posteriors, but when
      // computing the total likelihood we'll need to compensate for it later
      // on.
      //KALDI_ASSERT(this_tot_alpha - this_tot_alpha == 0);
      this_alpha[h * num_sequences + s] = this_tot_alpha ;//* arbitrary_scale; //#SCC#
    }
  }

  // Now compute alpha-sums for frame t:
  SubMatrix<BFloat> alpha_mat(this_alpha,
                                 num_graph_.MaxNumStates(),
                                 num_sequences_,
                                 num_sequences_);
  if (t == frames_per_sequence_) {// last alpha
    Matrix<BFloat> dd(num_graph_.FinalProbs());
    alpha_mat.MulElements(dd);
  }
  SubVector<BFloat> alpha_sum_vec(this_alpha +
                                     num_graph_.MaxNumStates() * num_sequences_,
                                     num_sequences_);
  alpha_sum_vec.AddRowSumMat(1.0, alpha_mat, 0.0);
  if (GetVerboseLevel() >= 3) {
    std::cout << "Alpha(t=" << t << "): ";
    for (int i = 0; i < num_graph_.MaxNumStates(); i++)
      std::cout << alpha_mat(i, 0) << " ";
    std::cout << "\n";
  }
  if (GetVerboseLevel() >= 3) {
    std::cout << "Alphasum(t=" << t << "): ";
    for (int i = 0; i < num_graph_.MaxNumStates(); i++)
      std::cout << alpha_sum_vec(i) << " ";
    std::cout << "\n";
  }


  //std::cout << "alpha-sums for t = " << t << ": \n";
  //alpha_sum_vec.Write(std::cout, false);
  //  std::cout << "t = " << t << ": ";
  //  for (int s=0;s<=117;s++) std::cout << " " << alpha_mat(s, 0);
  //  std::cout<<"\n";
  //if (t == frames_per_sequence_) {
  //  const Matrix<BaseFloat> &x = num_graph_.FinalProbs();
  //  for (int i = 0; i < num_graph_.MaxNumStates(); i++)
  //    if (x(i, 0) != 0.0)
  //std::cout << "i=" << i << ": " << x(i, 0) << " and alpha(i,0)=" << alpha_mat(i, 0) << "\n";
  //  }
/*
  std::cout << "Alphas for seq 7, t= " << t << ":\n";
  for (int h = 0; h < num_graph_.NumStates()[7]; h++)
    std::cout << alpha_mat(h, 7) << " ";
  std::cout << std::endl;
*/
 //  KALDI_LOG << "al-sum for seq=7, t=" << t << " is " << alpha_sum_vec(7);
}

BaseFloat FullNumeratorComputation::Forward() {
  AlphaFirstFrame();
  for (int32 t = 1; t <= frames_per_sequence_; t++) {
    AlphaGeneralFrame(t);
  }
  return ComputeTotLogLike();
}

BaseFloat FullNumeratorComputation::ComputeTotLogLike() {
  tot_prob_.Resize(num_sequences_);
  // View the last alpha as a matrix of size num-hmm-states by num-sequences.
  SubMatrix<BFloat> last_alpha(
      alpha_.RowData(frames_per_sequence_),
      num_graph_.MaxNumStates(),
      num_sequences_,
      num_sequences_);

// TODO(hhadian): last frame alpha should be multiplied with fianl prob of states before being summed-over to give us tot-prob


  tot_prob_.AddRowSumMat(1.0, last_alpha, 0.0);
  // we should probably add an ApplyLog() function that takes a vector argument.
  tot_log_prob_ = tot_prob_;
  tot_log_prob_.ApplyLog();
  if (num_graph_.AreFirstTransitionsScaled())
    tot_log_prob_.AddVec(-1.0, num_graph_.FirstTransitionOffsets());
  BFloat tot_log_prob = tot_log_prob_.Sum();

  // We now have to add something for the arbitrary scaling factor.  [note: the
  // purpose of the arbitrary scaling factors was to keep things in a good
  // floating-point range]
  // The inverses of all the tot-alpha quantities, for t = 0
  // ... frames_per_sequence_ - 1, were included as the 'arbitrary factors' in
  // the transition-probs, so we need to multiply them all together (not
  // inversed) and add them as a correction term to the total log-likes.
  // These tot-alpha quantities were stored in the same place that we would
  // have stored the HMM-state numbered 'max_num_hmm_states'.
  int32 max_num_hmm_states = num_graph_.MaxNumStates();
  SubMatrix<BFloat> inv_arbitrary_scales(
      alpha_, 0, frames_per_sequence_,
      num_sequences_ * max_num_hmm_states, num_sequences_);
  Matrix<BFloat> log_inv_arbitrary_scales(
      inv_arbitrary_scales);
  log_inv_arbitrary_scales.ApplyLog();
  BFloat log_inv_arbitrary_scales_product =
      log_inv_arbitrary_scales.Sum();
  return tot_log_prob + log_inv_arbitrary_scales_product;
}


bool FullNumeratorComputation::Backward(
    BaseFloat deriv_weight,
    CuMatrixBase<BaseFloat> *nnet_output_deriv) {
  BetaLastFrame();
  for (int32 t = frames_per_sequence_ - 1; t >= 0; t--) {
    BetaGeneralFrame(t);
    if (GetVerboseLevel() >= 1 || t == 0)
      BetaGeneralFrameDebug(t);
    if (t % kMaxDerivTimeSteps == 0) {
      // commit the derivative stored in exp_nnet_output_transposed_ by adding
      // its transpose to the appropriate sub-matrix of 'nnet_output_deriv'.
      int32 chunk_frames = std::min<int32>(static_cast<int32>(kMaxDerivTimeSteps),
                                           frames_per_sequence_ - t),
                num_pdfs = exp_nnet_output_transposed_.NumRows();
      SubMatrix<BaseFloat> transposed_deriv_part(
          nnet_output_deriv_transposed_,
          0, num_pdfs,
          0, chunk_frames * num_sequences_);
      CuMatrix<BaseFloat> tmp(transposed_deriv_part);
      CuSubMatrix<BaseFloat> output_deriv_part(
          *nnet_output_deriv,
          t * num_sequences_, chunk_frames * num_sequences_,
          0, num_pdfs);
      output_deriv_part.AddMat(deriv_weight, tmp, kTrans);
      if (t != 0)
        transposed_deriv_part.SetZero();
    }
  }
//  nnet_output_deriv->AddMat(
//                           deriv_weight, nnet_output_deriv_transposed_, kTrans);
  return ok_;
}

void FullNumeratorComputation::BetaLastFrame() {
  // sets up the beta quantity on the last frame (frame ==
  // frames_per_sequence_).  Note that the betas we use here contain a
  // 1/(tot-prob) factor in order to simplify the backprop.

  int32 t = frames_per_sequence_;
  BFloat *last_frame_beta = beta_.RowData(t % 2);

  // create a 'fake matrix' - view this row as a matrix.
  SubMatrix<BFloat> beta_mat(last_frame_beta,
                                num_graph_.MaxNumStates(),
                                num_sequences_,
                                num_sequences_);

  // There is only 1 final state in each sequence's HMM, and its prob is 1.0
  // Please refer to chain-supervision.h,cc for more info
  // since final state indexes are different for each sequence, we set them in
  // a for loop.
  /*int32 *num_states_cpu = new int32[num_graph_.NumSequences()];
  num_graph_.CopyNumStatesToCpu(num_states_cpu);  //TODO(hhadian) this might be really slow -- check it
  for (int32 seq = 0; seq < num_sequences_; seq++) {
    int32 final_state = num_states_cpu[seq] - 1;
    beta_mat(final_state, seq) = 1.0 / tot_prob_(seq);
  }
  delete num_states_cpu;*/

  Vector<BFloat> inv_tot_prob(tot_prob_);
  if (GetVerboseLevel() >= 2) {
    std::cout << "tot_prob: ";
    inv_tot_prob.Write(std::cout, false);
  }
  inv_tot_prob.InvertElements();
  beta_mat.CopyRowsFromVec(inv_tot_prob);
  Matrix<BFloat> dd(num_graph_.FinalProbs());
  beta_mat.MulElements(dd);
//  const Matrix<BaseFloat> &x = num_graph_.FinalProbs();
//  for (int i = 0; i < num_graph_.MaxNumStates(); i++)
//    std::cout << x(i, 0) << " ";
  if (GetVerboseLevel() >= 3) {
    std::cout << "Beta(T): ";
    for (int i = 0; i < num_graph_.MaxNumStates(); i++)
      std::cout << beta_mat(i, 0) << " ";
    std::cout << "\n";
  }
}

void FullNumeratorComputation::BetaGeneralFrame(int32 t) {
  KALDI_ASSERT(t >= 0 && t < frames_per_sequence_);
  int32 num_pdfs = exp_nnet_output_transposed_.NumRows();
  // t_wrapped gives us the time-index we use when indexing
  // nnet_output_deriv_transposed_; to save memory we limit the size of the
  // matrix, storing only chunks of frames at a time, and we add it to the
  // non-transposed output whenever we finish a chunk.
  int32 t_wrapped = t % static_cast<int32>(kMaxDerivTimeSteps);
  BFloat *this_alpha = alpha_.RowData(t),
                  *next_beta = beta_.RowData((t + 1) % 2);
  BFloat *this_beta = beta_.RowData(t % 2);
  const Int32Pair *forward_transitions = num_graph_.ForwardTransitions();
  const DenominatorGraphTransition *transitions = num_graph_.Transitions();
  // 'probs' is the matrix of pseudo-likelihoods for frame t.
  SubMatrix<BaseFloat> probs(exp_nnet_output_transposed_, 0, num_pdfs,
                               t * num_sequences_, num_sequences_),
      log_prob_deriv(nnet_output_deriv_transposed_, 0, num_pdfs,
                     t_wrapped * num_sequences_, num_sequences_);

  int32 max_num_hmm_states = num_graph_.MaxNumStates(),
        num_sequences = num_sequences_;


  int32 prob_stride = probs.Stride(),
        deriv_stride = log_prob_deriv.Stride();
  const BaseFloat *prob_data = probs.Data();
  BaseFloat *log_prob_deriv_data = log_prob_deriv.Data();
  for (int32 s = 0; s < num_sequences; s++) {
    for (int32 h = 0; h < num_graph_.NumStates()[s]; h++) {
      BaseFloat this_alpha_prob = this_alpha[h * num_sequences + s],
          inv_arbitrary_scale =
          this_alpha[max_num_hmm_states * num_sequences + s];
      double tot_variable_factor = 0.0;
      //BFloat occupation_factor = this_alpha_prob /
      //    inv_arbitrary_scale;
      const DenominatorGraphTransition
          *trans_iter = transitions +
            forward_transitions[s*max_num_hmm_states + h].first,
          *trans_end = transitions +
            forward_transitions[s*max_num_hmm_states + h].second;
      for (; trans_iter != trans_end; ++trans_iter) {
        BaseFloat transition_prob = trans_iter->transition_prob;
        int32 pdf_id = trans_iter->pdf_id,
            next_hmm_state = trans_iter->hmm_state;
        BFloat variable_factor = transition_prob *
            next_beta[next_hmm_state * num_sequences + s] *
            prob_data[pdf_id * prob_stride + s] / inv_arbitrary_scale;  // #SCC#
        tot_variable_factor += variable_factor;
        BFloat occupation_prob = variable_factor * this_alpha_prob; //occupation_factor; #SCC#
        log_prob_deriv_data[pdf_id * deriv_stride + s] += occupation_prob;


        if (variable_factor - variable_factor != 0  || tot_variable_factor - tot_variable_factor != 0) {
          //KALDI_LOG << "t: " << t << ", seq: " << s << ", h: " << h << ", pdf_id: " << pdf_id
          //<< ", var-factor: " << variable_factor
          //<< ", tot-var-factor: " << tot_variable_factor
          //<< ", trans-prob: " << transition_prob
          //<< ", ocup-prob: " << occupation_prob
          //<< ", next-beta: " << next_beta[next_hmm_state * num_sequences + s]
          //<< ", this_alpha_prob: " << this_alpha_prob
          //<< ", obs-prob: " << prob_data[pdf_id * prob_stride + s]
          //<< ", inv arbitrary_scale: " << inv_arbitrary_scale;
          //KALDI_ERR << "Beta failure.";
        }


      }
      this_beta[h * num_sequences + s] =
          tot_variable_factor ; /// inv_arbitrary_scale; #SCC#
    }
  }
  SubMatrix<BFloat> beta_mat(this_beta,
                                num_graph_.MaxNumStates(),
                                num_sequences_,
                                num_sequences_);
  if (GetVerboseLevel() >= 3) {
    std::cout << "Beta(t=" << t << "): ";
    for (int i = 0; i < num_graph_.MaxNumStates(); i++)
      std::cout << beta_mat(i, 0) << " ";
    std::cout << "\n";
  }
  if (GetVerboseLevel() >= 2) {
    std::cout << "Deriv(t=" << t << "): ";
    for (int i = 0; i < num_pdfs; i++)
      std::cout << log_prob_deriv_data[i * deriv_stride] << " ";
    std::cout << "\n";
  }
  //  Vector<BaseFloat> beta_sum_vec(num_sequences_);
  //  beta_sum_vec.AddRowSumMat(1.0, beta_mat, 0.0);
//  std::cout << "beta-sums for t = " << t+1 << ": \n";
//  beta_sum_vec.Write(std::cout, false);
}

void FullNumeratorComputation::BetaGeneralFrameDebug(int32 t) {
  int32 max_num_hmm_states = num_graph_.MaxNumStates(),
      alpha_beta_size = max_num_hmm_states * num_sequences_;
  SubVector<BFloat> this_alpha(alpha_.RowData(t), alpha_beta_size),
      this_beta(beta_.RowData(t % 2), alpha_beta_size);
  int32 t_wrapped = t % static_cast<int32>(kMaxDerivTimeSteps),
        num_pdfs = exp_nnet_output_transposed_.NumRows();
  SubMatrix<BaseFloat> this_log_prob_deriv(
      nnet_output_deriv_transposed_, 0, num_pdfs,
      t_wrapped * num_sequences_, num_sequences_);
  BFloat alpha_beta_product = VecVec(this_alpha,
                                        this_beta),
      this_log_prob_deriv_sum = this_log_prob_deriv.Sum();
  if (!ApproxEqual(alpha_beta_product, num_sequences_)) {
    KALDI_WARN << "On time " << t << ", alpha-beta product "
               << alpha_beta_product << " != " << num_sequences_
               << " alpha-sum = " << this_alpha.Sum()
               << ", beta-sum = " << this_beta.Sum();
    if (fabs(alpha_beta_product - num_sequences_) > 2.0 || alpha_beta_product - alpha_beta_product != 0) {
      KALDI_WARN << "Excessive error detected, will abandon this minibatch";
      ok_ = false;
    }
  }
  // use higher tolerance, since we are using randomized pruning for the
  // log-prob derivatives.
  if (!ApproxEqual(this_log_prob_deriv_sum,
                   num_sequences_, 0.01)) {
    KALDI_WARN << "On time " << t << ", log-prob-deriv sum "
               << this_log_prob_deriv_sum << " != " << num_sequences_;
    if (fabs(this_log_prob_deriv_sum - num_sequences_) > 2.0 || this_log_prob_deriv_sum - this_log_prob_deriv_sum != 0) {
      KALDI_WARN << "Excessive error detected, will abandon this minibatch";
      ok_ = false;
    }
  }
}


}  // namespace chain
}  // namespace kaldi