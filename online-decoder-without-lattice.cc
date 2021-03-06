// 张; 杨
#include "onlinedecoder/online-decoder-without-lattice.h"
#include "fst/script/project.h"
#include "feat/resample.h"

#include <jansson.h>
#include <time.h>

clock_t start_s;
using namespace kaldi;
// load settings from config file
// Reference: gst_kaldinnet2onlinedecoder_init
OnlineDecoderWithoutLattice::OnlineDecoderWithoutLattice(int id, const string& configFilePath)
{
	id_ = id;
	this->trans_model_ = NULL;
	this->am_nnet3_ = NULL;
	this->decode_fst_ = NULL;
	this->feature_info_ = NULL;
	this->word_syms_ = NULL;
	this->phone_syms_ = NULL;
	this->word_boundary_info_ = NULL;
	this->decode_fst_ = NULL;
	this->adaptation_state_ = NULL;
	this->audio_source_ = NULL;
	this->lm_fst_ = NULL;
	this->lm_compose_cache_ = NULL;
	this->sample_rate_ = 0;
	this->decode_thread_ = NULL;

  this->opts_ = new OnlineDecoderWithoutLatticeOptions();
	this->endpoint_config_ = new OnlineEndpointConfig();
	this->feature_config_ = new OnlineNnet2FeaturePipelineConfig();
	this->nnet3_decodable_opts_ = new nnet3::NnetSimpleLoopedComputationOptions();
	this->decoder_opts_ = new FasterDecoderOptions();
	this->silence_weighting_config_ = new OnlineSilenceWeightingConfig();

  const char *usage = "ASR Decoder.";
  ParseOptions po(usage);
  
  this->opts_->Register(&po);
	this->endpoint_config_->Register(&po);
	this->feature_config_->Register(&po);
	this->silence_weighting_config_->Register(&po);
	
  this->nnet3_decodable_opts_->Register(&po);
  this->decoder_opts_->Register(&po, true);
	
  
	// setup singnal callbacks
	this->onDecoderSignalCallbacks_[PARTIAL_RESULT_SIGNAL] = std::vector<DecoderSignalCallback>();
	this->onDecoderSignalCallbacks_[FINAL_RESULT_SIGNAL] = std::vector<DecoderSignalCallback>();
	this->onDecoderSignalCallbacks_[FULL_FINAL_RESULT_SIGNAL] = std::vector<DecoderSignalCallback>();
	this->onDecoderSignalCallbacks_[EOS_SIGNAL] = std::vector<DecoderSignalCallback>();
	
	po.ReadConfigFile(configFilePath);

	// load models from files
	this->LoadModel();

	state_ = DecoderState::State_InitDecoding;
}

// Reference: gst_kaldinnet2onlinedecoder_allocate
bool OnlineDecoderWithoutLattice::LoadModel() {
	KALDI_VLOG(2) << "Loading Kaldi models and feature extractor";

	if (!this->audio_source_) {
		this->audio_source_ = new AudioBufferSource();
	}

	if (this->feature_info_ == NULL) {
		this->feature_info_ = new OnlineNnet2FeaturePipelineInfo(*(this->feature_config_));
	}

	this->sample_rate_ = (int) this->opts_->real_sample_rate_;
  
	if (!this->adaptation_state_) {
		this->adaptation_state_ = new OnlineIvectorExtractorAdaptationState(
			this->feature_info_->ivector_extractor_info);
	  if (!this->opts_->adaptation_state_str_.empty()) {
      std::istringstream str(this->opts_->adaptation_state_str_);
      try {
        this->adaptation_state_->Read(str, false);
      } catch (std::runtime_error& e) {
        KALDI_WARN << "Failed to read adaptation state from given string, resetting instead";
        delete this->adaptation_state_;
        this->adaptation_state_ = new OnlineIvectorExtractorAdaptationState(
            this->feature_info_->ivector_extractor_info);
      }
    }
	}

  if (!this->opts_->word_syms_filename_.empty()) {
    this->LoadWordSyms();
  }

  if (!this->opts_->phone_syms_filename_.empty()) {
	  this->LoadPhoneSyms();
	}

	if (!this->opts_->word_boundary_info_filename_.empty()) {
	  this->LoadWordBoundaryInfo();
	}

	if (!this->opts_->model_rspecifier_.empty()) {
	  this->LoadAcousticModel();
	}

	if (!this->opts_->fst_rspecifier_.empty()) {
	  this->LoadFst();
	}

  if (!this->opts_->lm_fst_rspecifier_.empty()) {
    LoadLmFst();
  }

	return true;
}

// load word syms
// Reference: gst_kaldinnet2onlinedecoder_load_word_syms
void OnlineDecoderWithoutLattice::LoadWordSyms()
{
  try {
	  fst::SymbolTable * new_word_syms = fst::SymbolTable::ReadText(this->opts_->word_syms_filename_);
	  if (!new_word_syms) {
		  throw std::runtime_error("Word symbol table not read.");
	  }
	
	  // Delete old objects if needed
	  if (this->word_syms_) {
		  delete this->word_syms_;
	  }
	
	  // Replace the symbol table
	  this->word_syms_ = new_word_syms;
	} catch (std::runtime_error& e) {
	  KALDI_WARN << "Error loading the word symbol table: " << this->opts_->word_syms_filename_;
	}
}

// load phone syms
// Reference: gst_kaldinnet2onlinedecoder_load_phone_syms
void OnlineDecoderWithoutLattice::LoadPhoneSyms()
{
  try {
	  fst::SymbolTable * new_phone_syms = fst::SymbolTable::ReadText(this->opts_->phone_syms_filename_);
	  if (!new_phone_syms) {
		  throw std::runtime_error("Phone symbol table not read.");
	  }
	
	  // Delete old objects if needed
	  if (this->phone_syms_) {
		  delete this->phone_syms_;
	  }

	  // Replace the symbol table
	  this->phone_syms_ = new_phone_syms;
	} catch (std::runtime_error& e) {
	  KALDI_WARN << "Error loading the phone symbol table: " << this->opts_->phone_syms_filename_;
	}
}

// load word boundary info
// Reference: gst_kaldinnet2onlinedecoder_load_word_boundary_info
void OnlineDecoderWithoutLattice::LoadWordBoundaryInfo()
{
  try {
	  WordBoundaryInfoNewOpts opts; // use default opts
	  WordBoundaryInfo* new_word_boundary_info = new WordBoundaryInfo(opts, this->opts_->word_boundary_info_filename_);

	  if (!new_word_boundary_info) {
	    throw std::runtime_error("Word boundary info not read.");
	  }

	  // Delete old objects if needed
	  if (this->word_boundary_info_) {
	    delete this->word_boundary_info_;
	  }

	  // Replace the word boundary info
	  this->word_boundary_info_ = new_word_boundary_info;
  } catch (std::runtime_error& e) {
	  KALDI_WARN << "Error loading the word boundary info: " << this->opts_->word_boundary_info_filename_;
	}
}

// load acoustic model
// Reference: gst_kaldinnet2onlinedecoder_load_model
void OnlineDecoderWithoutLattice::LoadAcousticModel()
{
	
	if (!this->trans_model_) {
    this->trans_model_ = new TransitionModel();
  }
  
  if (!this->am_nnet3_) {
    this->am_nnet3_ = new nnet3::AmNnetSimple();
  }
  
  // Make the objects read the new models
  try {
    bool binary;
	  Input ki(this->opts_->model_rspecifier_, &binary);
	  this->trans_model_->Read(ki.Stream(), binary);
	
	  this->am_nnet3_->Read(ki.Stream(), binary);
	  SetBatchnormTestMode(true, &(this->am_nnet3_->GetNnet()));
	  SetDropoutTestMode(true, &(this->am_nnet3_->GetNnet()));
	  // this object contains precomputed stuff that is used by all decodable
	  // objects.  It takes a pointer to am_nnet because if it has iVectors it has
	  // to modify the nnet to accept iVectors at intervals.
	  this->decodable_info_nnet3_ = new nnet3::DecodableNnetSimpleLoopedInfo(*(this->nnet3_decodable_opts_), this->am_nnet3_); 
	} catch (std::runtime_error& e) {
	  KALDI_WARN << "Error loading the acoustic model: " << this->opts_->model_rspecifier_;
	}
}

// load fst
// Reference: gst_kaldinnet2onlinedecoder_load_fst
void OnlineDecoderWithoutLattice::LoadFst()
{
  try {
	  fst::Fst<fst::StdArc> * new_decode_fst = fst::ReadFstKaldiGeneric(this->opts_->fst_rspecifier_);

    if (!new_decode_fst) {
	    throw std::runtime_error("FST decoding graph not read.");
	  }
	  
	  // Delete objects if needed
	  if (this->decode_fst_) {
	    delete this->decode_fst_;
	  }

	  // Replace the decoding graph
	  this->decode_fst_ = new_decode_fst;
	} catch (std::runtime_error& e) {
	  KALDI_WARN << "Error loading FST decoding graph: " << this->opts_->fst_rspecifier_;
	}
}

// TODO: load lm fst and big lm 
// Reference: gst_kaldinnet2onlinedecoder_load_lm_fst
void OnlineDecoderWithoutLattice::LoadLmFst() {
  try {
	  // Delete objects if needed
    if (this->lm_fst_) {
      delete this->lm_fst_;
    }
    if (this->lm_compose_cache_) {
      delete this->lm_compose_cache_;
    }
    fst::VectorFst<fst::StdArc> *std_lm_fst = fst::VectorFst<fst::StdArc>::Read(this->opts_->lm_fst_rspecifier_);
    fst::Project(std_lm_fst, fst::PROJECT_OUTPUT);
    
    if (std_lm_fst->Properties(fst::kILabelSorted, true) == 0) {
      // Make sure LM is sorted on ilabel.
      fst::ILabelCompare<fst::StdArc> ilabel_comp;
      fst::ArcSort(std_lm_fst, ilabel_comp);
    }
    
    // mapped_fst is the LM fst interpreted using the LatticeWeight semiring,
    // with all the cost on the first member of the pair (since it's a graph
    // weight).
    int32 num_states_cache = 50000;
    fst::CacheOptions cache_opts(true, num_states_cache);
    fst::MapFstOptions mapfst_opts(cache_opts);
    fst::StdToLatticeMapper<BaseFloat> mapper;
    this->lm_fst_ = new fst::MapFst<fst::StdArc, LatticeArc,
        fst::StdToLatticeMapper<BaseFloat> >(*std_lm_fst, mapper, mapfst_opts);
    delete std_lm_fst;
    
    // The next fifteen or so lines are a kind of optimization and
    // can be ignored if you just want to understand what is going on.
    // Change the options for TableCompose to match the input
    // (because it's the arcs of the LM FST we want to do lookup
    // on).
    fst::TableComposeOptions compose_opts(fst::TableMatcherOptions(),
                                          true, fst::SEQUENCE_FILTER,
                                          fst::MATCH_INPUT);

    // The following is an optimization for the TableCompose
    // composition: it stores certain tables that enable fast
    // lookup of arcs during composition.
    this->lm_compose_cache_ = new fst::TableComposeCache<fst::Fst<LatticeArc> >(compose_opts);
    
	} catch (std::runtime_error& e) {
	  KALDI_WARN << "Error loading LM FST decoding graph: " << this->opts_->lm_fst_rspecifier_;
	}
}

// Reference: gst_kaldinnet2onlinedecoder_load_big_lm

// add callback for a signal
void OnlineDecoderWithoutLattice::AddCallBack(DecoderSignal signal, DecoderSignalCallback onSignal)
{
	KALDI_ASSERT(this->onDecoderSignalCallbacks_.find(signal) != this->onDecoderSignalCallbacks_.end());
	this->onDecoderSignalCallbacks_[signal].push_back(onSignal);
}

// invoke callback for a signal
void OnlineDecoderWithoutLattice::InvokeCallBack(DecoderSignal signal, const char* pszResults)
{
	KALDI_ASSERT(this->onDecoderSignalCallbacks_.find(signal) != this->onDecoderSignalCallbacks_.end());
	for(int32 i = 0; i < this->onDecoderSignalCallbacks_[signal].size(); ++ i)
		this->onDecoderSignalCallbacks_[signal][i](id_, pszResults);
}

// Get phone alignment from the compact lattice
// TODO: Test
// Reference: gst_kaldinnet2onlinedecoder_phone_alignment
std::vector<PhoneAlignmentInfo> OnlineDecoderWithoutLattice::GetPhoneAlignment(
	const std::vector<int32>& alignment, 
	fst::VectorFst<LatticeArc> &fst_in) 
{
	std::vector<PhoneAlignmentInfo> result;
	KALDI_VLOG(2) << "Phoneme alignment...";
	
	// Output the alignment with the weights
	std::vector<std::vector<int32> > split;
	SplitToPhones((*this->trans_model_), alignment, &split);
	KALDI_VLOG(2) << "Split to phones finished";

	std::vector<int32> phones;
	for (size_t i = 0; i < split.size(); i++) {
		KALDI_ASSERT(split[i].size() > 0);
		phones.push_back(this->trans_model_->TransitionIdToPhone(split[i][0]));
	}
  
	ConvertLatticeToPhones((*this->trans_model_), &fst_in);
	CompactLattice phone_clat;
	ConvertLattice(fst_in, &phone_clat);  
	MinimumBayesRiskOptions mbr_opts;
	mbr_opts.decode_mbr = false; // we just want confidences
	mbr_opts.print_silence = false; 
	MinimumBayesRisk *mbr = new MinimumBayesRisk(phone_clat, phones, mbr_opts);
	std::vector<BaseFloat> confidences = mbr->GetOneBestConfidences();
	delete mbr;	

	int32 current_start_frame = 0;
	for (size_t i = 0; i < split.size(); i++) {
		KALDI_ASSERT(split[i].size() > 0);
		int32 phone = this->trans_model_->TransitionIdToPhone(split[i][0]);

		PhoneAlignmentInfo alignment_info;
		alignment_info.phone_id = phone;
		alignment_info.start_frame = current_start_frame;
		alignment_info.length_in_frames = split[i].size();
		if (confidences.size() > 0) {
			alignment_info.confidence = confidences[i];
		}

		result.push_back(alignment_info);
		current_start_frame += split[i].size();
	}
	return result;
}

// Get word alignment from the compact lattice
// TODO: Test
// Reference: gst_kaldinnet2onlinedecoder_word_alignment
std::vector<WordAlignmentInfo> OnlineDecoderWithoutLattice::GetWordAlignment(
    const fst::VectorFst<LatticeArc> &fst_in) 
{
	std::vector<WordAlignmentInfo> result;
	std::vector<int32> words, times, lengths;
	CompactLattice clat;
	ConvertLattice(fst_in, &clat);
	CompactLattice aligned_clat;
	if (!WordAlignLattice(clat, *(this->trans_model_), *(this->word_boundary_info_), 0, &aligned_clat)) {
		KALDI_ERR << "Failed to word-align the lattice";
		return result;
	}

	if (!CompactLatticeToWordAlignment(aligned_clat, &words, &times, &lengths)) {
		KALDI_ERR << "Failed to do word alignment";
		return result;
	}
	KALDI_ASSERT(words.size() == times.size() &&
				 words.size() == lengths.size());
				 
	MinimumBayesRiskOptions mbr_opts;
	mbr_opts.decode_mbr = false; // we just want confidences
	mbr_opts.print_silence = false; 
	MinimumBayesRisk *mbr = new MinimumBayesRisk(clat, words, mbr_opts);
	std::vector<BaseFloat> confidences = mbr->GetOneBestConfidences();
	delete mbr;
	
	int confidence_i = 0;
	for (size_t i = 0; i < words.size(); i++) {
		if (words[i] == 0)  {
			// Don't output anything for <eps> links, which
			continue; // correspond to silence....
		}
		WordAlignmentInfo alignment_info;
		alignment_info.word_id = words[i];
		alignment_info.start_frame = times[i];
		alignment_info.length_in_frames = lengths[i];
		if (confidences.size() > 0) {
			alignment_info.confidence = confidences[confidence_i++];
		}
		result.push_back(alignment_info);
	}
	return result;
}

// Reference: gst_kaldinnet2onlinedecoder_words_to_string
std::string OnlineDecoderWithoutLattice::Words2String(const std::vector<int32> &words) {
	std::stringstream sentence;
	for (size_t i = 0; i < words.size(); i++) {
		std::string s = this->word_syms_->Find(words[i]);
		if (s == "")
			KALDI_ERR << "Word-id " << words[i] << " not in symbol table.";
		if (i > 0) {
			sentence << " ";
		}
		sentence << s;
	}
	return sentence.str();
}

// Reference: gst_kaldinnet2onlinedecoder_words_in_hyp_to_string
std::string OnlineDecoderWithoutLattice::WordsInHyp2String(const std::vector<WordInHypothesis> &words) {
	std::vector<int32> word_ids;
	for (size_t i = 0; i < words.size(); i++) {
		word_ids.push_back(words[i].word_id);
	}
	return this->Words2String(word_ids);
}

// Reference: gst_kaldinnet2onlinedecoder_nbest_results
OneBestResult OnlineDecoderWithoutLattice::GetOneBestResults(fst::VectorFst<LatticeArc> &fst_in) {

	OneBestResult onebest_result;

	std::vector<int32> words;
	std::vector<int32> alignment;
	LatticeWeight weight;
	GetLinearSymbolSequence(fst_in, &alignment, &words, &weight);

	onebest_result.likelihood = -(weight.Value1() + weight.Value2());
	onebest_result.num_frames = alignment.size();
	for (size_t j=0; j < words.size(); j++) {
		WordInHypothesis word_in_hyp;
		word_in_hyp.word_id = words[j];
		onebest_result.words.push_back(word_in_hyp);
	}
	if (this->opts_->do_phone_alignment_) {
	  onebest_result.phone_alignment =
		  this->GetPhoneAlignment(alignment, fst_in);
	}
	if (this->word_boundary_info_) {
		onebest_result.word_alignment = this->GetWordAlignment(fst_in);
	}

	return onebest_result;
}


// Reference: gst_kaldinnet2onlinedecoder_full_final_result_to_json
std::string OnlineDecoderWithoutLattice::FullFinalResult2Json(
	const FullFinalResultWithoutLattice &full_final_result) {

	json_t *root = json_object();
	json_t *result_json_object = json_object();
	json_object_set_new(root, "speaker", json_string(full_final_result.spkr.c_str()));
	json_object_set_new( root, "result", result_json_object);
	json_object_set_new( result_json_object, "final", json_true());

	BaseFloat frame_shift = this->feature_info_->FrameShiftInSeconds();
	frame_shift *= this->nnet3_decodable_opts_->frame_subsampling_factor;
	json_object_set_new(root, "segment-start",  json_real(this->segment_start_time_));
	json_object_set_new(root, "segment-length",  json_real(full_final_result.onebest_result.num_frames * frame_shift));
	json_object_set_new(root, "total-length",  json_real(this->total_time_decoded_));
	json_t *nbest_json_arr = json_array();

	json_t *onebest_result_json_object = json_object();
	json_object_set_new(onebest_result_json_object, "transcript",
						json_string(this->WordsInHyp2String(full_final_result.onebest_result.words).c_str()));
	json_object_set_new(onebest_result_json_object, "likelihood",  json_real(full_final_result.onebest_result.likelihood));
	json_array_append( nbest_json_arr, onebest_result_json_object );
  if (full_final_result.onebest_result.phone_alignment.size() > 0) {
	  if (strcmp(this->opts_->phone_syms_filename_.c_str(), "") == 0) {
		  KALDI_ERR << "Phoneme symbol table filename (phone-syms) must be set to output phone alignment.";
	  } else if (this->phone_syms_ == NULL) {
		  KALDI_ERR << "Phoneme symbol table wasn't loaded correctly. Not outputting alignment.";
	  } else {
		  json_t *phone_alignment_json_arr = json_array();
		  for (size_t j = 0; j < full_final_result.onebest_result.phone_alignment.size(); j++) {
			  PhoneAlignmentInfo alignment_info = full_final_result.onebest_result.phone_alignment[j];
			  json_t *alignment_info_json_object = json_object();
			  std::string phone = this->phone_syms_->Find(alignment_info.phone_id);
			  json_object_set_new(alignment_info_json_object, "phone",
								  json_string(phone.c_str()));
			  json_object_set_new(alignment_info_json_object, "start",
								  json_real(alignment_info.start_frame * frame_shift));
			  json_object_set_new(alignment_info_json_object, "length",
								  json_real(alignment_info.length_in_frames * frame_shift));
			  json_object_set_new(alignment_info_json_object, "confidence",
								  json_real(alignment_info.confidence));
			  json_array_append(phone_alignment_json_arr, alignment_info_json_object);
		  }
		  json_object_set_new(onebest_result_json_object, "phone-alignment", phone_alignment_json_arr);
	  }
  }
  if (full_final_result.onebest_result.word_alignment.size() > 0) {
	  json_t *word_alignment_json_arr = json_array();
	  for (size_t j = 0; j < full_final_result.onebest_result.word_alignment.size(); j++) {
		  WordAlignmentInfo alignment_info = full_final_result.onebest_result.word_alignment[j];
		  json_t *alignment_info_json_object = json_object();
		  std::string word = this->word_syms_->Find(alignment_info.word_id);
		  json_object_set_new(alignment_info_json_object, "word",
							  json_string(word.c_str()));
		  json_object_set_new(alignment_info_json_object, "start",
							  json_real(alignment_info.start_frame * frame_shift));
		  json_object_set_new(alignment_info_json_object, "length",
							  json_real(alignment_info.length_in_frames * frame_shift));
		  json_object_set_new(alignment_info_json_object, "confidence",
							  json_real(alignment_info.confidence));
		  json_array_append(word_alignment_json_arr, alignment_info_json_object);
	  }
	  json_object_set_new(onebest_result_json_object, "word-alignment", word_alignment_json_arr);
  }

	json_object_set_new(result_json_object, "hypotheses", nbest_json_arr);

	char *ret_strings = json_dumps(root, JSON_REAL_PRECISION(6));

	json_decref( root );
	std::string result;
	result = ret_strings;
	return result;
}

// Reference: gst_kaldinnet2onlinedecoder_final_result
void OnlineDecoderWithoutLattice::GenerateFinalResult(
	fst::VectorFst<LatticeArc> &fst_in, int32 *num_words, string spkr) {
	std::vector<int32> word_ids;
  fst::GetLinearSymbolSequence(fst_in,
                               static_cast<std::vector<int32> *>(0),
                               &word_ids,
                               static_cast<LatticeArc::Weight*>(0));
  
  FullFinalResultWithoutLattice full_final_result;
	KALDI_VLOG(2) << "Decoding n-best results";
	full_final_result.spkr = spkr;
	full_final_result.onebest_result = this->GetOneBestResults(fst_in);
 
	std::string best_transcript = this->WordsInHyp2String(full_final_result.onebest_result.words);

	KALDI_VLOG(2) << "Likelihood per frame is "
			  << full_final_result.onebest_result.likelihood/full_final_result.onebest_result.num_frames
			  << " over " << full_final_result.onebest_result.num_frames << " %d frames";
	KALDI_VLOG(2) << "Final: " <<  best_transcript.c_str();
	int32 hyp_length = best_transcript.length();
	*num_words = full_final_result.onebest_result.words.size();

	if (hyp_length > 0) {
		// Invoke the FINAL_RESULT_SIGNAL
		this->InvokeCallBack(FINAL_RESULT_SIGNAL, best_transcript.c_str());
		// Invoke the FULL_FINAL_RESULT_SIGNAL
		std::string full_final_result_as_json =	this->FullFinalResult2Json(full_final_result);
		KALDI_VLOG(2) << "Final JSON: " << full_final_result_as_json.c_str();
		this->InvokeCallBack(FULL_FINAL_RESULT_SIGNAL, full_final_result_as_json.c_str());
	}
}

// Reference: gst_kaldinnet2onlinedecoder_partial_result
void OnlineDecoderWithoutLattice::GeneratePartialResult(fst::MutableFst<LatticeArc> &fst_in) {
	std::vector<int32> word_ids;
	fst::GetLinearSymbolSequence(fst_in,
                              static_cast<std::vector<int32> *>(0),
                              &word_ids,
                              static_cast<LatticeArc::Weight*>(0));
                              
  std::string transcript = this->Words2String(word_ids);
	KALDI_VLOG(2) << "Partial: " << transcript.c_str();
	if (transcript.length() > 0) {
		// Invoke the PARTIAL_RESULT_SIGNAL signal
		this->InvokeCallBack(PARTIAL_RESULT_SIGNAL, transcript.c_str());
	}

}

// Reference: gst_kaldinnet2onlinedecoder_nnet3_unthreaded_decode_segment
void OnlineDecoderWithoutLattice::DecodeSegment(AudioState &audio_state, int32 chunk_length, BaseFloat traceback_period_secs) {
  OnlineNnet2FeaturePipeline feature_pipeline(*(this->feature_info_));
  
  feature_pipeline.SetAdaptationState(*(this->adaptation_state_));
  
  OnlineSilenceWeighting silence_weighting(*(this->trans_model_),
                                           *(this->silence_weighting_config_));
            
  SingleUtteranceNnet3DecoderWithoutLattice decoder(*(this->decoder_opts_),
                                      *(this->trans_model_), 
                                      *(this->decodable_info_nnet3_),
                                      *(this->decode_fst_),
                                      &feature_pipeline);
 

  Vector<BaseFloat> wave_part(chunk_length);
  std::vector<std::pair<int32, BaseFloat> > delta_weights;
  KALDI_VLOG(2) << "Reading audio in " << wave_part.Dim() << " sample chunks...";
  BaseFloat last_traceback = 0.0;
  BaseFloat num_seconds_decoded = 0.0;
  string spkr;
  
  while (true) {
	  audio_state = this->audio_source_->ReadData(&wave_part, spkr);
	  // check if any data is read
	  if (spkr.empty())
	  {
		  // if no data is read, that audio state should be SpkrEnd or AudioEnd
		  KALDI_ASSERT(audio_state == AudioState::SpkrEnd || audio_state == AudioState::AudioEnd);
		  // skip starting empty speaker end change, 
		  // this occurs when last segment is the end of an old spk and we start a new spk in this segment
		  if (num_seconds_decoded == 0 && audio_state == AudioState::SpkrEnd)
			  continue;
		  else
			  break; // otherwise, exit and end this segment decoding
	  }
	  // std::cout << "Recieved data, decoding ..." << std::endl;
	  // if some data is read, proceed to decoding it
	  if (this->sample_rate_ > this->feature_info_->mfcc_opts.frame_opts.samp_freq) {
	    std::cout << "WARNING: the sample rate of audio is not match that of models, " 
	      << "downsample will take lots of time." << std::endl;
	    Vector<BaseFloat> downsampled_wave(wave_part);
      DownsampleWaveForm(this->sample_rate_, wave_part,
                         this->feature_info_->mfcc_opts.frame_opts.samp_freq, &downsampled_wave);
      feature_pipeline.AcceptWaveform(this->feature_info_->mfcc_opts.frame_opts.samp_freq, downsampled_wave);
    } else {
      feature_pipeline.AcceptWaveform(this->sample_rate_, wave_part);
    }
	  
	  // if the audio state is SpkrEnd or AudioEnd, it means an end of the current segment, so let's finish feature input
    if (audio_state == AudioState::SpkrEnd || audio_state == AudioState::AudioEnd) {
      feature_pipeline.InputFinished();
    }
    if (silence_weighting.Active() && 
        feature_pipeline.IvectorFeature() != NULL) {
      silence_weighting.ComputeCurrentTraceback(decoder.Decoder());
      silence_weighting.GetDeltaWeights(feature_pipeline.IvectorFeature()->NumFramesReady(), 
                                        &delta_weights);
      feature_pipeline.IvectorFeature()->UpdateFrameWeights(delta_weights);
    }
    decoder.AdvanceDecoding();
    KALDI_VLOG(2) <<  decoder.NumFramesDecoded() << " frames decoded";
	  BaseFloat num_seconds = (BaseFloat) wave_part.Dim() / this->sample_rate_;
    num_seconds_decoded += num_seconds;
    this->total_time_decoded_ += num_seconds;
    KALDI_VLOG(2) << "Total amount of audio processed: " << this->total_time_decoded_ << " seconds";

	  // if this is the end of a speaker or audio, exit decoding current segment
    if (audio_state == AudioState::SpkrEnd) {
		  KALDI_VLOG(2) << "Speaker change detected!";
      break;
    }
	  if (audio_state == AudioState::AudioEnd) {
		  KALDI_VLOG(2) << "Audio end detected!";
		  break;
	  }
	  // if an end pointing is detected, also exit decoding current segment
    if (this->opts_->do_endpointing_
        && (decoder.NumFramesDecoded() > 0)
        && decoder.EndpointDetected(*(this->endpoint_config_))) {
      KALDI_VLOG(2) << "Endpoint detected!";
      break;
    }
	  // generate partial result every traceback_period_secs
    if ((num_seconds_decoded - last_traceback > traceback_period_secs)
        && (decoder.NumFramesDecoded() > 0)) {
      fst::VectorFst<LatticeArc> fst_out;
      decoder.GetBestPath(false, &fst_out);
      this->GeneratePartialResult(fst_out);
      last_traceback += traceback_period_secs;
      
      clock_t ended_s = clock();
      std::cout << float (ended_s - start_s) / CLOCKS_PER_SEC - this->total_time_decoded_ << std::endl;
    }
  }
  // generate final results
 if (num_seconds_decoded > 0.1) {
    fst::VectorFst<LatticeArc> fst_out;
    bool end_of_utterance = true;
    decoder.GetBestPath(end_of_utterance, &fst_out);
    int32 num_words = 0;
    this->GenerateFinalResult(fst_out, &num_words, spkr);

    if (num_words >= this->opts_->min_words_for_ivector_) {
      // Only update adaptation state if the utterance contained enough words
      feature_pipeline.GetAdaptationState(this->adaptation_state_);
    }
  } else {
    KALDI_VLOG(2) << "Less than 0.1 seconds decoded, discarding ...";
  }
}

void OnlineDecoderWithoutLattice::ChangeState(DecoderState newState)
{
	std::lock_guard<std::mutex> state_locker(state_mtx_);
	switch (newState)
	{
	case DecoderState::State_InitDecoding:
		state_ = DecoderState::State_InitDecoding;
		break;
	case DecoderState::State_OnDecoding:
		KALDI_ASSERT(state_ == DecoderState::State_InitDecoding || state_ == DecoderState::State_SuspendDecoding);
		state_ = DecoderState::State_OnDecoding;
		break;
	case DecoderState::State_SuspendDecoding:
		KALDI_ASSERT(state_ == DecoderState::State_OnDecoding);
		state_ = DecoderState::State_SuspendDecoding;
		break;
	case DecoderState::State_StopDecoding:
		KALDI_ASSERT(state_ != DecoderState::State_StopDecoding);
		state_ = DecoderState::State_StopDecoding;
		break;
	case DecoderState::State_EndDecoding:
		state_ = DecoderState::State_EndDecoding;
		break;
	default:
		KALDI_ERR << "Invalid Decoder State!";
	}
	state_cond_.notify_one();
}

void OnlineDecoderWithoutLattice::StartDecoding()
{
	this->audio_source_->SetEnded(false);
	this->ChangeState(DecoderState::State_OnDecoding);
	decode_thread_ = new std::thread(&OnlineDecoderWithoutLattice::DecodeLoop, this);
}

void OnlineDecoderWithoutLattice::SuspendDecoding() {
	// set the audio source to ended to stop receive more data
	KALDI_VLOG(2) << "Suspend Processing";
	this->audio_source_->SetEnded(true);
	this->ChangeState(DecoderState::State_SuspendDecoding);
}

void OnlineDecoderWithoutLattice::ResumeDecoding() {
	// set the audio source to start receive more data
	KALDI_VLOG(2) << "Resume Processing";
	this->audio_source_->SetEnded(false);
	this->ChangeState(DecoderState::State_OnDecoding);
}

void OnlineDecoderWithoutLattice::StopDecoding() {
	// set the audio source to ended to stop receive more data
	this->audio_source_->SetEnded(true);
	this->ChangeState(DecoderState::State_StopDecoding);
}

void OnlineDecoderWithoutLattice::WaitForEndOfDecoding()
{
	if (decode_thread_ != NULL)
	{
		decode_thread_->join();
		delete decode_thread_;
		decode_thread_ = NULL;
	}
}

// reference: gst_kaldinnet2onlinedecoder_loop
void OnlineDecoderWithoutLattice::DecodeLoop() {
  start_s = clock();
	KALDI_VLOG(2) << "Starting decoding loop..";
	BaseFloat traceback_period_secs = this->opts_->traceback_period_in_secs_;
	int32 chunk_length = int32(this->sample_rate_ * this->opts_->chunk_length_in_secs_);

	Vector<BaseFloat> remaining_wave_part;
	this->segment_start_time_ = 0.0;
	this->total_time_decoded_ = 0.0;
	
	AudioState audio_state = AudioState::SpkrContinue;
	while (true) {
		// check state for stop and suspend
		if (state_ == DecoderState::State_StopDecoding || 
		    state_ == DecoderState::State_SuspendDecoding)
		{
			std::unique_lock<std::mutex> state_locker(state_mtx_);
			if (state_ == DecoderState::State_StopDecoding)
				break;
			if (state_ == DecoderState::State_SuspendDecoding)
			{
			  KALDI_VLOG(2) << "Suspend Recognizer";
				// unlock the state locker to allow change decode state while processin remaining data
				state_locker.unlock();
				// Process remaining data in the audio buffer
				while (audio_state != AudioState::AudioEnd)
				{
					this->DecodeSegment(audio_state, chunk_length, traceback_period_secs);
					this->segment_start_time_ = this->total_time_decoded_;
				}
				state_locker.lock();
				
				// wait for suspend state to change
				KALDI_VLOG(2) << "Waiting State Change";
				state_cond_.wait(state_locker, [this] {return this->state_ != DecoderState::State_SuspendDecoding; });
				KALDI_VLOG(2) << "State Changed";
				// if changed to stop state, then stop decoding
				if (state_ == DecoderState::State_StopDecoding)
					break;
			}
		}
		
	  this->DecodeSegment(audio_state, chunk_length, traceback_period_secs);
	  this->segment_start_time_ = this->total_time_decoded_;
	}

	// Process remaining data in the audio buffer
	while (audio_state != AudioState::AudioEnd)
	{
		this->DecodeSegment(audio_state, chunk_length, traceback_period_secs);
		this->segment_start_time_ = this->total_time_decoded_;
	}

	KALDI_VLOG(2) << "Finished decoding loop";
	KALDI_VLOG(2) << "Pushing EOS event";
	this->InvokeCallBack(EOS_SIGNAL, NULL);
}

// Reference: gst_kaldinnet2onlinedecoder_finalize
OnlineDecoderWithoutLattice::~OnlineDecoderWithoutLattice() {
	KALDI_ASSERT(state_ == DecoderState::State_InitDecoding || DecoderState::State_EndDecoding);
	delete this->endpoint_config_;
	delete this->feature_config_;
	delete this->nnet3_decodable_opts_;
	delete this->decoder_opts_;
	delete this->silence_weighting_config_;
	delete this->opts_;
	if (this->feature_info_) {
		delete this->feature_info_;
	}
	if (this->trans_model_) {
		delete this->trans_model_;
	}
	if (this->am_nnet3_) {
		delete this->am_nnet3_;
	}
	if (this->decode_fst_) {
		delete this->decode_fst_;
	}
	if (this->lm_fst_) {
		delete this->lm_fst_;
	}
	if (this->lm_compose_cache_) {
		delete this->lm_compose_cache_;
	}
	if (this->word_syms_) {
		delete this->word_syms_;
	}
	if (this->adaptation_state_) {
		delete this->adaptation_state_;
	}
}
