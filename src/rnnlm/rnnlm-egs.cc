// rnnlm/rnnlm-egs.cc

// Copyright 2017  Daniel Povey

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

#include <numeric>
#include "rnnlm/rnnlm-egs.h"

namespace kaldi {
namespace rnnlm {


RnnlmMinibatchSampler::RnnlmMinibatchSampler(
    const RnnlmEgsConfig &config, const ArpaForSampling &arpa):
    config_(config), arpa_(arpa) {

  // The unigram distribution from the LM, modified according to
  // config_.special_symbol_prob and config_.uniform_prob_mass...
  std::vector<BaseFloat> unigram_distribution;
  arpa.GetUnigramDistribution(&unigram_distribution);
  double sum = std::accumulate(unigram_distribution.begin(),
                                  unigram_distribution.end(),
                                  0.0);
  KALDI_ASSERT(abs(sum - 1.0) < 0.01 &&
               "Unigram distribution from ARPA does not sum "
               "to (close to) 1");
  int32 num_words = unigram_distribution.size();
  if (config_.unigram_prob_mass > 0.0) {
    BaseFloat x = config_unigram_prob_mass / (num_words - 1);
    for (int32 i = 1; i < num_words; i++)
      unigram_distribution[i] += x;
  }
  // If these are not zero, either something is wrong with your language model
  // or you supplied the wrong --bos-symbol or --brk-symbol options.  We allow
  // tiny values because the ARPA files sometimes give -99 as the unigram prob
  // for <s>.
  KALDI_ASSERT(unigram_distribution[config_.bos_symbol] < 1.0e-10 &&
               unigram_distribution[config_.brk_symbol] < 1.0e-10);

  unigram_distribution[config_.bos_symbol] = config_.special_symbol_prob;
  unigram_distribution[config_.brk_symbol] = config_.special_symbol_prob;
  double new_sum = std::accumulate(unigram_distribution.begin(),
                                   unigram_distribution.end(),
                                   0.0),
      scale = 1.0 / new_sum;
  // rescale so it sums to almost 1; this is a requirement of the constructor
  // of class Sampler.

  int32 num_words_nonzero_prob = 0;
  for (std::vector<BaseFloat>::iterator iter = unigram_distribution.begin(),
           end = unigram_distriubution.end(); iter != end; ++iter) {
    if (*iter != 0.0) num_words_nonzero_prob++;
    *iter *= scale;
  }

  int32 min_num_samples = std::min<int32>(num_words,
                                          config_.minibatch_size *
                                          config_.sample_group_size);
  if (config_.num_samples > num_words_nonzero_prob) {
    KALDI_WARN << "The number of samples (--num-samples=" << config_.num_samples
               << ") exceeds the number of words with nonzero probability "
               << num_words_nonzero << " -> not doing sampling.  You could "
               << "skip creating the ARPA file, and not provide it, which "
               << "might save some bother.";
    config_.num_samples = 0;
  }
  if (config_.num_samples == 0) {
    sampler_ = NULL;
  } else {
    sampler_ = new Sampler(unigram_distribution_);
  }
}


void RnnlmMinibatchSampler::SampleForMinibatch(RnnlmMinibatch *minibatch) const {
  if (sampler_ == NULL) return;  // we're not actually sampling.
  KALDI_ASSERT(minibatch->chunk_length == config_.chunk_length &&
               minibatch->num_sequences == config_.minibatch_size &&
               config_.sample_group_size % config_.chunk_length == 0 &&
               static_cast<int32>(minibatch->input_words.dim()) ==
               config_.chunk_length * config_.minibatch_size);
  int32 num_samples = config_.num_samples,
      sample_group_size = config_.sample_group_size,
      chunk_length = config_.chunk_length,
      num_groups = chunk_length / sample_group_size;
  minibatch->num_samples = num_samples;
  minibatch->sample_group_size = sample_group_size;
  minibatch->sampled_words.resize(num_groups * num_samples);
  minibatch->sample_probs.resize(num_groups * num_samples);

  for (int32 g = 0; g < num_groups; g++) {
    SampleForGroup(g, minibatch);
  }
}


void RnnlmMinibatchSampler::SampleForGroup(int32 g,
                                           RnnlmMinibatch *minibatch) const {
  // All words that appear on the output are required to appear in the sample.  we
  // need to figure what this set of words is.
  int32 sample_group_size = config_.sample_group_size,
      chunk_length = config_.chunk_length,
      minibatch_size = config_.minibatch_size;
  std::unordered_map<int32> words_we_must_sample;
  for (int32 t = g * config_.sample_group_size;
       t < (g + 1) * config_.sample_group_size; t++) {
    for (int32 n = 0; n < config_.minibatch_size; n++) {
      int32 i = t * config_.minibatch_size + n;
      int32 output_word = minibatch->output_words[i];
      words_we_must_sample.insert(output_word);
    }
  }

  std::vector<std::pair<HistType, BaseFloat> > hist_weights;
  GetHistoriesForGroup(g, *minibatch, &hist_weights);
}

BaseFloat RnnlmMinibatchSampler::GetHistoriesForGroup(
    int32 g, const RnnlmMinibatch &minibatch,
    std::vector<std::pair<std::vector<int32>, BaseFloat> > *hist_weights) const {
  // initially store as an unordered_map so we can remove duplicates.
  std::unordered_map<std::vector<int32>, BaseFloat, VectorHasher> hist_to_weight;

  hist_weights->clear();
  KALDI_ASSERT(arpa_.Order() > 0);
  int32 history_length = arpa.Order() - 1;

  for (int32 t = g * config_.sample_group_size;
       t < (g + 1) * config_.sample_group_size; t++) {
    for (int32 n = 0; n < config_.minibatch_size; n++) {
      BaseFloat this_weight = minibatch.output_weights[t * minibatch_size + n];
      KALDI_ASSERT(this_weight >= 0);
      if (this_weight == 0.0)
        continue;
      std::vector<int32> history;
      GetHistory(t, n, minibatch, &history);
      // note: if the value did not exist in the map, it is as
      // if it were zero, see here:
      // https://stackoverflow.com/questions/8943261/stdunordered-map-initialization
      // .. this is at least since C++11, maybe since C++03.
      hist_to_weight[history] += weight;
    }
  }
  if (hist_to_weight.empty()) {
    KALDI_WARN << "No histories seen (we don't expect to see this very often)";
    std::vector<int32> empty_history;
    hist_to_weight[empty_history] = 1.0;
  }
  std::unordered_map<std::vector<int32>, BaseFloat, VectorHasher>::const_iterator
      iter = hist_to_weight.begin(), end = hist_to_weight.end();
  hist_weights->reserve(hist_to_weight.size());
  for (; iter != end; ++iter)
    hist_weights->push_back(std::pair<std::vector<int32>, BaseFloat>(
        iter->first, iter->second);
}

void RnnlmMinibatchSampler::GetHistory(
    int32 t, int32 n,
    const RnnlmMinibatch &minibatch,
    int32 max_history_length,
    std::vector<int32> *history) {
  history->reserve(max_history_length);
  history->clear();
  int32 minibatch_size = config_.minibatch_size;

  // e.g. if 'max_history_length' is 2, we iterate over t_step = [0, -1].
  // you'll notice that the first history-position we look for when
  // predicting position 't' is 'hist_t = t + 0 = t'.  This may be
  // surprising-- you might be expecting that t-1 would be the first
  // position we'd look at-- but notice that we're looking at the
  // input word, not the output word.
  for (int32 t_step = 0; t_step > -max_history_length; t_step--) {
    int32 hist_t = t + t_step;
    KALDI_ASSERT(hist_t >= 0);  // .. or we should have done 'break' below
                                // before reaching this value of t_step.  If
                                // this assert fails it means that a minibatch
                                // doesn't start with input_word equal to
                                // bos_symbol or brk_symbol, which is a bug.
    int32 i = hist_t * minibatch_size + n,
        history_word = minibatch.input_words[i];
    history->push_back(history_word);
    if (history_word == config_.bos_symbol ||
        history_word == config_.brk_symbol)
      break;
  }
  // we want the most recent word to be the last word in 'history', so the order
  // needs to be reversed.
  std::reverse(history->begin(), history->end());
}




void RnnlmMinibatchCreator::AcceptSequence(
    BaseFloat weight, std::vector<int32> &words) {
  CheckSequence(weight, words);
  SplitSequenceIntoChunks(weight, words);
  while (chunks_.size() > static_cast<size_t>(config_.chunk_buffer_size)) {
    if (!WriteMinibatch())
      break;
  }
}

RnnlmMinibatchCreator::~RnnlmMinibatchCreator() {
  for (size_t i = 0; i < chunk_.size(); i++)
    delete chunks_[i];
}

RnnlmMinibatchCreator::SingleMinibatchCreator::SingleMinibatchCreator(
    const RnnlmEgsConfig &config):
    config_(config),
    chunks_(config_.minibatch_size) {
  for (int32 i = 0; i < config_.minibatch_size; i++)
    empty_eg_chunks_.push_back(i);
}

bool RnnlmMinibatchCreator::SingleMinibatchCreator::AcceptChunk(
    RnnlmMinibatchCreator::SequenceChunk *chunk) {
  int32 chunk_len = chunk->Length();
  if (chunk_len == config_.chunk_length) {  // maximum-sized chunk.
    if (empty_eg_chunks_.empty()) {
      return false;
    } else  {
      int32 i = empty_eg_chunks_.back();
      KALDI_ASSERT(size_t(i) < egs_chunks_.size() && eg_chunks_[i].empty());
      eg_chunks_[i].push_back(chunk);
      empty_eg_chunks_.pop_back();
      return true;
    }
  } else {  // smaller-sized chunk than maximum chunk size.
    KALDI_ASSERT(chunk_len < config_.chunk_length);
    // Find the index best_i into partial_eg_chunks_, such
    // that partial_eg_chunks_[best_i] is a pair (best_j,
    // best_space_left) such that space_left >= chunk_len, with
    // best_space_left as small as possible.
    int32 best_i = -1, best_j = -1,
        best_space_left = std::numeric_limits<int32>::max(),
        size = partial_eg_chunks_.size();
    for (int32 i = 0; i < size; i++) {
      int32 this_space_left = partial_eg_chunks_[i].second;
      if (this_space_left >= chunk_len && this_space_left < best_space_left) {
        best_i = i;
        best_j = partial_eg_chunks_[i].first;
        best_space_left = this_space_left;
      }
    }
    if (best_i != -1) {
      partial_eg_chunks_[best_i] = partial_eg_chunks_.back();
      partial_eg_chunks_.pop_back();
    } else {
      // consume a currently-unused chunk, if available.
      if (empty_eg_chunks_.empty()) {
        return false;
      } else {
        best_j = empty_eg_chunks_.back();
        empty_eg_chunks_.pop_back();
        best_space_left = config_.chunk_length;
      }
    }
    int32 new_space_left = best_space_left - chunk_len;
    KALDI_ASSERT(new__space_left >= 0);
    if (new_space_left > 0) {
      partial_eg_chunks_.push_back(std::pair<int32, int32>(best_j,
                                                           new_space_left_));
    }
    eg_chunks_[best_j].push_back(chunk);
    return true;
  }
}

BaseFloat RnnlmMinibatchCreator::SingleMinibatchCreator::ProportionFull() {
  int32 den = config_.minibatch_size * config_.chunk_length;
  int32 num = den -
      config_.chunk_length * static_cast<int32>(empty_eg_chunks_.size());
  for (size_t i = 0; i < partial_eg_chunks_.size(); i++)
    num -= partial_eg_chunks_[i].second;
  KALDI_ASSERT(num >= 0);
  return num * 1.0 / den;
}

RnnlmMinibatchCreator::SingleMinibatchCreator::~SingleMinibatchCreator() {
  for (size_t i = 0; i < eg_chunks_.size(); i++)
    for (size_t j = 0; j < eg_chunks_[i].size(); j++)
      delete eg_chunks_[i][j];
}


void RnnlmMinibatchCreator::SingleMinibatchCreator::CreateMinibatchOneSequence(
    int32 n, RnnlmMinibatch *minibatch) {
  // Much of the code here is about figuring out what to do if we haven't
  // completely used up the potential length of the sequence.  We first try
  // giving extra left-context to any split-up pieces of sequence that could potentially
  // use extra left-context; when that avenue is exhausted, we
  // pad at the end with </s> symbols with zero weight.


  KALDI_ASSERT(static_cast<size_t>(n) < eg_chunks_.size());
  const std::vector<SequenceChunk*> &this_chunks = eg_chunks_[n];
  int32 num_chunks = this_chunks.size();
  // note: often num_chunks will be 1, occasionally 0 (if we've run out of
  // data), and sometimes more than 1 (if we're appending multiple chunks
  // together because they were shorter than config_.chunk_length).


  // total_current_chunk_length is the total Length() of all the chunks.
  int32 total_current_chunk_length = 0;
  for (int32 c = 0; c < num_chunks; c++) {
    total_current_chunk_length += this_chunks[c]->Length();
  }
  KALDI_ASSERT(total_current_chunk_length <= config_.chunk_length);
  int32 extra_length_available = config_.chunk_length - total_current_chunk_length;

  while (true) {
    bool changed = false;
    for (int32 c = 0; c < num_chunks; c++) {
      if (this_chunks[c]->context_begin > 0 && extra_length_available > 0) {
        changed = true;
        this_chunks[c]->context_begin--;
        extra_length_available--;
      }
    }
    if (!changed)
      break;
  }

  int32 pos = 0;  // position in the sequence (we increase this every time a word
                  // gets added).
  for (int32 c = 0; c < num_chunks; c++) {
    SequenceChunk &chunk = *(this_chunks[c]);

    // note: begin and end are the indexes of the first and the last-plus-one
    // words in the sequence that we *predict*.
    // you can think of real_begin as the index of the first real word in the
    // sequence that we use as left context (however it will be preceded by
    // either a <s> or a <brk>, depending whether 'real_begin' is 0 or >0).
    // For these positions that are only used as left context, and not predicted
    // the weight of the output (predicted) word is zero.  'begin' is the index
    // of the first predicted word.
    int32 context_begin = chunk.context_begin,
        begin = chunk.begin,
        end = chunk.end;
    for (int32 i = context_begin; i < end; i++) {
      int32 output_word = (*chunk.sequence)[i],
          input_word;
      if (i == context_begin) {
        if (context_begin == 0) input_word = config_.bos_symbol;
        else input_word = config_.brk_symbol;
      } else {
        input_word = (*chunk.sequence)[i - 1];
      }
      BaseFloat weight = (i < begin ? 0.0 : chunk.weight);
      Set(n, pos, input_word, output_word, weight, minibatch);
      pos++;
    }
  }
  for (; pos < config_.chunk_length; pos++) {
    // fill the rest with <s> as input and </s> as output
    // and weight of 0.0.  The symbol-id doesn't really matter
    // so we pick ones that we know are valid inputs and outputs.
    int32 input_word = config_.bos,
        output_word = config_.eos;
    BaseFloat weight = 0.0;
    Set(n, pos, input_word_output_word, weight, minibatch);
  }
}


void RnnlmMinibatchCreator::SingleMinibatchCreator::Set(
    int32 n, int32 t, int32 input_word, int32 output_word,
    BaseFloat weight, RnnlmMinibatch *minibatch) const {
  KALDI_ASSERT(n >= 0 && n < config_.minibatch_size &&
               t >= 0 && t < config_.chunk_length &&
               weight >= 0.0);

  int32 i = t * config_.minibatch_size + n;
  minibatch->input_words[i] = input_word;
  minibatch->output_words[i] = output_word;
  minibatch->weights[i] = weight;
}


void RnnlmMinibatchCreator::SingleMinibatchCreator::CreateMinibatchSampling(
    RnnlmMinibatch *minibatch) {
  // This does the sampling parts of creating a minibatch.
}

void RnnlmMinibatchCreator::SingleMinibatchCreator::CreateMinibatch(
    RnnlmMinibatch *minibatch) {
  minibatch->num_sequences = config_.minibatch_size;
  minibatch->chunk_length = config_.chunk_length;
  minibatch->num_samples = config_.num_samples;
  int32 num_words = config_.chunk_length * config_.minibatch_size;
  minibatch->input_words.resize(num_words);
  minibatch->output_words.resize(num_words);
  minibatch->output_weights.Resize(num_words);
  minibatch->sampled_words.Resize(chunk_length * config_.num_samples);
  for (int32 n = 0; n < config_.minibatch_size; n++) {
    CreateMinibatchOneSequence(n, minibatch);
  }
}

RnnlmMinibatchCreator::SequenceChunk* RnnlmMinibatchCreator::GetRandomChunk() {
  KALDI_ASSERT(!chunks_.empty());
  int32 pos = RandInt(0, chunks_.size() - 1);
  SequenceChunk *ans = chunks_[pos];
  chunks_[pos] = chunks_.back();
  chunks_.pop_back();
  return ans;
}

bool RnnlmMinibatchCreator::WriteMinibatch() {


}


void RnnlmMinibatchCreator::SplitSequenceIntoChunks(
    BaseFloat weight, const std::vector<int32> &words) {
  std::shared_ptr<std::vector<int32> > ptr = new std::vector<int32>();
  ptr->reserve(words.size() + 1);
  ptr->insert(ptr->end(), words.begin(), words.end());
  ptr->push_back(config_.eos_symbol);  // add the terminating </s>.

  int32 sequence_length = ptr->size();  // == words.size() + 1
  if (sequence_length <= config_.chunk_length)) {
  chunks_.push_back(new SequenceChunk(config_, ptr, weight,
                                      0, sequence_length));
  } else {
    std::vector<int32> chunk_lengths;
    ChooseChunkLengths(sequence_length, &chunk_lengths);
    int32 cur_start = 0;
    for (size_t i = 0; i < chunk_lengths.size(); i++) {
      int32 this_end = cur_start + chunk_lengths[i];
      chunks_.push_back(new SequenceChunk(config_, ptr, weight,
                                          cur_start, this_end));
      cur_start = this_end;
    }
  }
}

// see comment in rnnlm-egs.h, by its declaration.
void RnnlmMinibatchCreator::ChooseChunkLengths(
    int32 sequence_length,
    std::vector<int32> *chunk_lengths) {
  KALDI_ASSERT(sequence_length > config_.chunk_length);
  chunk_lengths->clear();
  int32 tot = sequence_length - config_.min_split_context,
     chunk_length_no_context = config_.chunk_length - config_.min_split_context;
  KALDI_ASSERT(chunk_length_no_context > 0);
  // divide 'tot' into pieces of size <= config_.chunk_length - config_.min_split_context.

  // note:
  for (int32 i = 0; i < tot / chunk_length_no_context; i++)
    chunk_lengths->push_back(chunk_length_no_context);
  KALDI_ASSERT(!chunk_lengths->empty());
  int32 remaining_size = tot % chunk_length_no_context;
  if (remaining_size != 0) {
    // put the smaller piece in a random location.
    (*chunk_lengths)[RandInt(0, chunk_lengths.size() - 1)] = remaining_size;
    chunk_lengths->push_back(chunk_length_no_context);
  }
  (*chunk_lengths)[0] += config_.min_split_context;
  KALDI_ASSERT(std::accumulate(chunk_lengths->begin(), chunk_length->end(), 0)
               == sequence_length);
}

void RnnlmMinibatchCreator::CheckSequence(
    BaseFloat weight,
    const std::vector<int32> &words) {
  KALDI_ASSERT(weight > 0.0);
  int32 bos_symbol = config_.bos_symbol,
      brk_symbol = config_.brk_symbol,
      eos_symbol = config_.eos_symbo;
  for (size_t i = 0; i < words.size(); i++) {
    KALDI_ASSERT(words[i] != bos_symbol && words[i] != brk_symbol);
  }
  if (!words.empty() && words.back() == eos_symbol) {
    // we may rate-limit this warning eventually if people legitimately need to
    // do this.
    KALDI_WARN << "Raw word sequence contains </s> at the end.  "
        "Is this a bug in your data preparation?  We'll add another one.";
  }
}





}  // namespace rnnlm
}  // namespace kaldi


