#include <cstdlib>
#include <algorithm>
#include <sstream>
#include <iostream>
#include <vector>
#include <limits>
#include <cmath>
#include <chrono>
#include <ctime>
#include <random>

#include <unordered_map>
#include <unordered_set>

#include <execinfo.h>
#include <unistd.h>
#include <signal.h>

#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <boost/program_options.hpp>

#include "cnn/training.h"
#include "cnn/cnn.h"
#include "cnn/expr.h"
#include "cnn/nodes.h"
#include "cnn/lstm.h"
#include "cnn/rnn.h"
#include "c2.h"

//this is the new version for dynamic orales.

cpyp::Corpus corpus;
volatile bool requested_stop = false;
unsigned LAYERS = 2;
unsigned INPUT_DIM = 40;
unsigned HIDDEN_DIM = 60;
unsigned ACTION_DIM = 36;
unsigned PRETRAINED_DIM = 50;
unsigned LSTM_INPUT_DIM = 60;
unsigned POS_DIM = 10;
unsigned REL_DIM = 8;


unsigned LSTM_CHAR_OUTPUT_DIM = 100; //Miguel
bool USE_SPELLING = false;

bool USE_POS = false;

constexpr const char* ROOT_SYMBOL = "ROOT";
unsigned kROOT_SYMBOL = 0;
unsigned ACTION_SIZE = 0;
unsigned VOCAB_SIZE = 0;
unsigned POS_SIZE = 0;

unsigned CHAR_SIZE = 255; //size of ascii chars... Miguel

unsigned CUR_ITER = 0;

using namespace cnn::expr;
using namespace cnn;
using namespace std;
namespace po = boost::program_options;

vector<unsigned> possible_actions;
unordered_map<unsigned, vector<float>> pretrained;

default_random_engine rng;
uniform_real_distribution<double> uniform(0,1.0);

bool use_arc_hybrid = 1;  // YG
set<string> all_lefts;
set<string> all_rights;

bool update_towards_one_best = 0; // YG
bool EXPLORE = 0; // YG
bool orig_compose=false;
float exploration_smoothing = 1.0;


unsigned sample_from(const vector<float>& log_events, float power) {
    float sum = 0.0;
    float sum2 = 0.0;
    vector<float> events;
    for (float e : log_events) { 
        float ee = exp(e); 
        if (ee <= 1e-5) (ee = 0);
        sum+=ee; events.push_back(ee);
    }
    for (int i = 0; i < events.size(); ++i) {
        events[i] = events[i]/sum;
        if (power != 1.0)
            events[i] = pow(events[i],power);
        sum2 += events[i];
        //cout << "e" << events[i] << endl;
    }
    //cout << "   sum:" << sum2 << endl;
    while (1) {
        float target = uniform(rng); 
        //cout << "target:" << target << endl;
        int r = 0;
        for (float e : events) { 
            target -= e / sum2;
            if (target <= 0) {
                //cout << "event:" << e << endl;
                return r;
            }
            r++;
        }
        cout << "beyond end. " << target << ", retrying." << std::endl;
    }
    cout << "beyond end. " << std::endl;
    return 0;
}

void InitCommandLine(int argc, char** argv, po::variables_map* conf) {
  po::options_description opts("Configuration options");
  opts.add_options()
        ("training_data,T", po::value<string>(), "List of Transitions - Training corpus")
        ("dev_data,d", po::value<string>(), "Development corpus")
        ("test_data,p", po::value<string>(), "Test corpus")
        ("unk_strategy,o", po::value<unsigned>()->default_value(1), "Unknown word strategy: 1 = singletons become UNK with probability unk_prob")
        ("unk_prob,u", po::value<double>()->default_value(0.2), "Probably with which to replace singletons with UNK in training data")
        ("model,m", po::value<string>(), "Load saved model from this file")
        ("use_pos_tags,P", "make POS tags visible to parser")
        ("beam_size,b", po::value<unsigned>()->default_value(1), "beam size")
        ("layers", po::value<unsigned>()->default_value(2), "number of LSTM layers")
        ("action_dim", po::value<unsigned>()->default_value(16), "action embedding size")
        ("input_dim", po::value<unsigned>()->default_value(32), "input embedding size")
        ("hidden_dim", po::value<unsigned>()->default_value(64), "hidden dimension")
        ("pretrained_dim", po::value<unsigned>()->default_value(50), "pretrained input dimension")
        ("pos_dim", po::value<unsigned>()->default_value(12), "POS dimension")
        ("rel_dim", po::value<unsigned>()->default_value(10), "relation dimension")
        ("lstm_input_dim", po::value<unsigned>()->default_value(60), "LSTM input dimension")
        ("train,t", "Should training be run?")
        ("words,w", po::value<string>(), "Pretrained word embeddings")
        ("use_spelling,S", "Use spelling model") //Miguel. Spelling model
        ("one_best_update", "Update towards one best") // YG
        ("archyb", "Use arc-hybrid") // YG 
        ("explore", "Use exploration-training") // YG 
        ("sampling_pow", po::value<float>()->default_value(1.0), "Power to use for sampling smoothing") // YG
        ("compose1", "Use original-composition") // YG 
        ("out_model,M", po::value<string>(), "save models to this file")
        ("help,h", "Help");
  po::options_description dcmdline_options;
  dcmdline_options.add(opts);
  po::store(parse_command_line(argc, argv, dcmdline_options), *conf);
  if (conf->count("help")) {
    cerr << dcmdline_options << endl;
    exit(1);
  }
  if (conf->count("training_data") == 0) {
    cerr << "Please specify --traing_data (-T): this is required to determine the vocabulary mapping, even if the parser is used in prediction mode.\n";
    exit(1);
  }
}

struct ParserBuilder {

  LSTMBuilder stack_lstm; // (layers, input, hidden, trainer)
  LSTMBuilder buffer_lstm;
  LSTMBuilder action_lstm;
  LookupParameters* p_w; // word embeddings
  LookupParameters* p_t; // pretrained word embeddings (not updated)
  LookupParameters* p_a; // input action embeddings
  LookupParameters* p_r; // relation embeddings
  LookupParameters* p_p; // pos tag embeddings
  Parameters* p_pbias; // parser state bias
  Parameters* p_A; // action lstm to parser state
  Parameters* p_B; // buffer lstm to parser state
  Parameters* p_S; // stack lstm to parser state
  Parameters* p_H; // head matrix for composition function
  Parameters* p_D; // dependency matrix for composition function
  Parameters* p_Rl; // relation matrix for composition function
  Parameters* p_Rr; // relation matrix for composition function
  Parameters* p_w2l; // word to LSTM input
  Parameters* p_p2l; // POS to LSTM input
  Parameters* p_t2l; // pretrained word embeddings to LSTM input
  Parameters* p_ib; // LSTM input bias
  Parameters* p_cbias; // composition function bias
  Parameters* p_p2a;   // parser state to action
  Parameters* p_action_start;  // action bias
  Parameters* p_abias;  // action bias
  Parameters* p_buffer_guard;  // end of buffer
  Parameters* p_stack_guard;  // end of stack

  Parameters* p_start_of_word;//Miguel -->dummy <s> symbol
  Parameters* p_end_of_word; //Miguel --> dummy </s> symbol
  LookupParameters* char_emb; //Miguel-> mapping of characters to vectors 


  LSTMBuilder fw_char_lstm; // Miguel
  LSTMBuilder bw_char_lstm; //Miguel


  explicit ParserBuilder(Model* model, const unordered_map<unsigned, vector<float>>& pretrained) :
      stack_lstm(LAYERS, LSTM_INPUT_DIM, HIDDEN_DIM, model),
      buffer_lstm(LAYERS, LSTM_INPUT_DIM, HIDDEN_DIM, model),
      action_lstm(LAYERS, ACTION_DIM, HIDDEN_DIM, model),
      p_w(model->add_lookup_parameters(VOCAB_SIZE, Dim(INPUT_DIM, 1))),
      p_a(model->add_lookup_parameters(ACTION_SIZE, Dim(ACTION_DIM, 1))),
      p_r(model->add_lookup_parameters(ACTION_SIZE, Dim(REL_DIM, 1))),
      p_pbias(model->add_parameters(Dim(HIDDEN_DIM, 1))),
      p_A(model->add_parameters(Dim(HIDDEN_DIM, HIDDEN_DIM))),
      p_B(model->add_parameters(Dim(HIDDEN_DIM, HIDDEN_DIM))),
      p_S(model->add_parameters(Dim(HIDDEN_DIM, HIDDEN_DIM))),
      p_H(model->add_parameters(Dim(LSTM_INPUT_DIM, LSTM_INPUT_DIM))),
      p_D(model->add_parameters(Dim(LSTM_INPUT_DIM, LSTM_INPUT_DIM))),
      p_Rl(model->add_parameters(Dim(LSTM_INPUT_DIM, REL_DIM))),
      p_Rr(model->add_parameters(Dim(LSTM_INPUT_DIM, REL_DIM))),
      p_w2l(model->add_parameters(Dim(LSTM_INPUT_DIM, INPUT_DIM))),
      p_ib(model->add_parameters(Dim(LSTM_INPUT_DIM, 1))),
      p_cbias(model->add_parameters(Dim(LSTM_INPUT_DIM, 1))),
      p_p2a(model->add_parameters(Dim(ACTION_SIZE, HIDDEN_DIM))),
      p_action_start(model->add_parameters(Dim(ACTION_DIM, 1))),
      p_abias(model->add_parameters(Dim(ACTION_SIZE, 1))),

      p_buffer_guard(model->add_parameters(Dim(LSTM_INPUT_DIM, 1))),
      p_stack_guard(model->add_parameters(Dim(LSTM_INPUT_DIM, 1))),

      p_start_of_word(model->add_parameters(Dim(LSTM_INPUT_DIM, 1))), //Miguel
      p_end_of_word(model->add_parameters(Dim(LSTM_INPUT_DIM, 1))), //Miguel 

      char_emb(model->add_lookup_parameters(CHAR_SIZE, Dim(INPUT_DIM, 1))),//Miguel

//      fw_char_lstm(LAYERS, LSTM_CHAR_OUTPUT_DIM, LSTM_INPUT_DIM, model), //Miguel
//      bw_char_lstm(LAYERS, LSTM_CHAR_OUTPUT_DIM, LSTM_INPUT_DIM,  model), //Miguel

      fw_char_lstm(LAYERS, LSTM_INPUT_DIM, LSTM_CHAR_OUTPUT_DIM/2, model), //Miguel 
      bw_char_lstm(LAYERS, LSTM_INPUT_DIM, LSTM_CHAR_OUTPUT_DIM/2, model) /*Miguel*/ {
    if (USE_POS) {
      p_p = model->add_lookup_parameters(POS_SIZE, Dim(POS_DIM, 1));
      p_p2l = model->add_parameters(Dim(LSTM_INPUT_DIM, POS_DIM));
    }
    if (pretrained.size() > 0) {
      p_t = model->add_lookup_parameters(VOCAB_SIZE, Dim(PRETRAINED_DIM, 1));
      for (auto it : pretrained)
        p_t->Initialize(it.first, it.second);
      p_t2l = model->add_parameters(Dim(LSTM_INPUT_DIM, PRETRAINED_DIM));
    } else {
      p_t = nullptr;
      p_t2l = nullptr;
    }
  }

static bool IsActionForbidden(const string& a, unsigned bsize, unsigned ssize, vector<int> stacki) {
  if (a[1]=='W' && ssize<3) return true; //MIGUEL

  if (a[1]=='W') { //MIGUEL

        int top=stacki[stacki.size()-1];
        int sec=stacki[stacki.size()-2];

        if (sec>top) return true;
  }

  if (use_arc_hybrid) { // YG
      bool is_shift = (a[0] == 'S' && a[1]=='H');
      bool is_left  = (a[0] == 'L');
      bool is_right = (a[0] == 'R');
      if (is_shift && bsize == 1) return true;
      if (is_left  && ssize < 2) return true;
      if (is_right && ssize < 3) return true;
      if (bsize == 2 && // ROOT is the only thing remaining on buffer
          ssize > 1 && // there is an element on the stack
          is_shift) return true;
      // only attach left to ROOT
      if (bsize == 1 && ssize == 3 && a[0] == 'R') return true;
      return false;
  }

  bool is_shift = (a[0] == 'S' && a[1]=='H');  //MIGUEL
  bool is_reduce = !is_shift;
  if (is_shift && bsize == 1) return true;
  if (is_reduce && ssize < 3) return true;
  if (bsize == 2 && // ROOT is the only thing remaining on buffer
      ssize > 2 && // there is more than a single element on the stack
      is_shift) return true;
  // only attach left to ROOT
  if (bsize == 1 && ssize == 3 && a[0] == 'R') return true;
  return false;
}

/*static bool IsActionForbidden(const string& a, unsigned bsize, unsigned ssize) {
  bool is_shift = (a[0] == 'S');
  bool is_reduce = !is_shift;
  if (is_shift && bsize == 1) return true;
  if (is_reduce && ssize < 3) return true;
  if (bsize == 2 && // ROOT is the only thing remaining on buffer
      ssize > 2 && // there is more than a single element on the stack
      is_shift) return true;
  // only attach left to ROOT
  if (bsize == 1 && ssize == 3 && a[0] == 'R') return true;
  return false;
}*/

static map<int,int> compute_heads(unsigned sent_len, const vector<unsigned>& actions, const vector<string>& setOfActions, map<int,string>* pr = nullptr) {
    // TODO YG Verify This!!!
  map<int,int> heads;
  map<int,string> r;
  map<int,string>& rels = (pr ? *pr : r);
  for(unsigned i=0;i<sent_len;i++) { heads[i]=-1; rels[i]="ERROR"; }
  vector<int> bufferi(sent_len + 1, 0), stacki(1, -999);
  for (unsigned i = 0; i < sent_len; ++i)
    bufferi[sent_len - i] = i;
  bufferi[0] = -999;
  for (auto action: actions) { // loop over transitions for sentence
    const string& actionString=setOfActions[action];
      //cout << "action:" << action << " " << actionString << endl;
    const char ac = actionString[0];
    const char ac2 = actionString[1];
    if (ac =='S' && ac2=='H') {  // SHIFT
      assert(bufferi.size() > 1); // dummy symbol means > 1 (not >= 1)
      stacki.push_back(bufferi.back());
      bufferi.pop_back();
    } 
   else if (ac=='S' && ac2=='W') {
        assert(stacki.size() > 2);

//	std::cout<<"SWAP"<<"\n";
        unsigned ii = 0, jj = 0;
        jj=stacki.back();
        stacki.pop_back();

        ii=stacki.back();
        stacki.pop_back();

        bufferi.push_back(ii);

        stacki.push_back(jj);
    }

    else { // LEFT or RIGHT
      assert(stacki.size() > 2); // dummy symbol means > 2 (not >= 2)
      assert(ac == 'L' || ac == 'R');
      unsigned depi = 0, headi = 0;
      (ac == 'R' ? depi : headi) = stacki.back();
      stacki.pop_back();
      (ac == 'R' ? headi : depi) = stacki.back();
      stacki.pop_back();
      stacki.push_back(headi);
      heads[depi] = headi; //cout << "assigning:" << depi+1 << " " << headi+1 << endl;
      // get the label
      int b = actionString.find("(");
      int e = actionString.find(")");
      rels[depi] = actionString.substr(b+1,e-b-1);
    }
  }
  assert(bufferi.size() == 1);
  //assert(stacki.size() == 2);
  return heads;
}

// computes the heads given actions, for the arc-hybrid system.
static map<int,int> compute_heads_hybrid(unsigned sent_len, const vector<unsigned>& actions, const vector<string>& setOfActions, map<int,string>* pr = nullptr) {
    map<int,int> heads;
    map<int,string> r;
    map<int,string>& rels = (pr ? *pr : r);
    for(unsigned i=0;i<sent_len;i++) { heads[i]=-1; rels[i]="ERROR"; }
    vector<int> bufferi(sent_len + 1, 0), stacki(1, -999);
    for (unsigned i = 0; i < sent_len; ++i)
        bufferi[sent_len - i] = i;
    bufferi[0] = -999;
    for (auto action: actions) { // loop over transitions for sentence
        const string& actionString=setOfActions[action];
        const char ac = actionString[0];
        const char ac2 = actionString[1];
        if (ac =='S' && ac2=='H') {  // SHIFT
            assert(bufferi.size() > 1); // dummy symbol means > 1 (not >= 1)
            stacki.push_back(bufferi.back());
            bufferi.pop_back();
        } else { // LEFT or RIGHT
            assert(stacki.size() > 1); // dummy symbol means > 1 (not >= 1)
            unsigned depi = 0, headi = 0;
            depi = stacki.back();
            stacki.pop_back();
            if (ac == 'R') {
                assert(stacki.size() > 1); // dummy symbol means > 2 (not >= 2)
                headi = stacki.back();
                stacki.pop_back();
                stacki.push_back(headi);
            }
            if (ac == 'L') {
                headi = bufferi.back();
                heads[depi] = headi;
            }
            heads[depi] = headi;
            // get the label
            int b = actionString.find("(");
            int e = actionString.find(")");
            rels[depi] = actionString.substr(b+1,e-b-1);
        }
    }
    assert(bufferi.size() == 1);
    //assert(stacki.size() == 2);
    return heads;
}




// given the first character of a UTF8 block, find out how wide it is
// see http://en.wikipedia.org/wiki/UTF-8 for more info
inline unsigned int UTF8Len(unsigned char x) {
  if (x < 0x80) return 1;
  else if ((x >> 5) == 0x06) return 2;
  else if ((x >> 4) == 0x0e) return 3;
  else if ((x >> 3) == 0x1e) return 4;
  else if ((x >> 2) == 0x3e) return 5;
  else if ((x >> 1) == 0x7e) return 6;
  else return 0;
}

// YG -- start of dynamic oracle
// The arc-standard oracle is only half dynamic: it allows more than one transition
// in some configurations, but will only work correctly as long as we are on the gold path.
// (once we deviate from the oracle's suggestions, his following answers will be wrong).
vector<string> arc_standard_oracle(const map<int,int>& gold_heads,
                                     const map<int,string>& gold_rels,
                                     const map<int,int>& gold_rmcs, // rightmost childs
                                     const map<int,int>& curr_heads,
                                     const vector<int>& stacki,
                                     const vector<int>& bufferi) {
    vector<string> results;
    // let s0 = top of stack, s1 = second on stack, b0 = first on buffer.
    // let s0R be the rightmost child of s0 in gold_heads.
    // if gold_heads[s1] == s0:
    //        add LEFT(gold_rels[s1])
    //        if s0R >= b0: add SHIFT  [*]
    // elif gold_heads[s0] == s1 and s0R < b0: 
    //        add RIGHT(gold_rels[s0])
    // else:
    //        add(SHIFT)
    //
    // The line marked with [*] is not strictly needed, but
    // does give us some dynamic behavior (someitmes there are two correct answers).
    // It is correct because if we don't do a left-arc now, we could do it later,
    // right after we do an right-arc between S0 and s0R, and S0 is on top of stack again.
    if (stacki.size() <= 2) {
        results.push_back("SHIFT");
        return results;
    }

    int s0 = stacki[stacki.size()-1];
    int s1 = stacki[stacki.size()-2];
    assert(s0 == stacki.back());
    int b0 = bufferi.back();
    int s0R = gold_rmcs.at(s0);
    //cout << "s0:" << s0 << " " << "s1:" << s1 << " b0:" << b0 << " s0R:" << s0R << endl;
    if (gold_heads.at(s1) == s0) {
        // YG -- comment the next line in order to get the "deterministic"
        // arc-standard oracle.
        if (s0R >= b0) { results.push_back("SHIFT"); }
        results.push_back("LEFT-ARC(" + gold_rels.at(s1) + ")");
    } else if (gold_heads.at(s0) == s1 && s0R < b0) {
        results.push_back("RIGHT-ARC(" + gold_rels.at(s0) + ")");
    } else {
        results.push_back("SHIFT");
    }
    return results;
}


// ArcHybrid Dynamic Oracle
int arc_hybrid_shift_loss(int b0,
                          const vector<int>& stacki,
                          const map<int,int>& gold_heads ) {
    int loss = 0;
    if (b0 < 0) { return -1; } // no buffer, can't shift.
    if (stacki.size() == 1) { // empty
        //cout << "empty stack shift" << endl;
        return 0;
    }
    // b0 can no longer have children on the stack
    // b0 can no longer have a parent on stack[:-1]
    for (int i = 1; i < stacki.size() - 1; i++) { 
        int si = stacki.at(i);
        //if (b0==9) { cout << "b0=" << b0+1 << " si=" << si+1 << " " << gold_heads.at(b0)+1 << endl; }
        if (gold_heads.at(si) == b0) loss++;
        if (gold_heads.at(b0) == si) loss++;
    }
    if ((stacki.back() >= 0) && gold_heads.at(stacki.back()) == b0) loss++;

    //cout << "shift loss is " << loss << "(b0=" << b0+1 << ")" << endl;
    return loss;
}
int arc_hybrid_left_loss(int b0, 
                         const vector<int>& stacki, 
                         const map<int,int>& gold_heads) {
    
   //cout<<"kroot:"<<kROOT_SYMBOL<<"\n"; -- MB. This is the root symbol in case we need it, and it is accessible.
    
    int loss = 0;
    if (stacki.size() - 1 < 0) { return -1; }
    if (b0 < 0) { return -1; }
    int s0 = stacki.at(stacki.size() - 1);
    // s0 can no longer have parents on buffer[1:]
    if (gold_heads.at(s0) > b0) loss++;
    // s0 can no longer have deps on buffer
    for (int i=b0; i < gold_heads.size(); ++i) { //TODO: boundaries? TODO: root?
        if (gold_heads.at(i) == s0) loss++;
    }
    // s0 can no longer have parents on stack[-2]
    if (stacki.size()-2 > 0) {
        int s1 = stacki.at(stacki.size() - 2);
        if (gold_heads.at(s0) == s1) loss++;
    }
    //cout << "left loss is " << loss << endl;
    return loss;
}
int arc_hybrid_right_loss(int b0,
                          const vector<int>& stacki,
                          const map<int,int>& gold_heads) {
   
    int loss = 0;
    if (stacki.size() - 2 < 0) { return -1; }
    int s0 = stacki.at(stacki.size() - 1);
    int s1 = stacki.at(stacki.size() - 2);
    // s0 can no longer have parents in buffer
    if (gold_heads.at(s0) >= b0) loss++;
    // s0 can no longer have modifiers in buffer
    if (b0 < 0) { return loss; }
    for (int m=b0; m < gold_heads.size(); ++m) { //TODO: boundaries? TODO: root?
        if (gold_heads.at(m) == s0) loss++;
    }
    //cout << "right loss is " << loss << endl;
    return loss;
}

vector<string> arc_hybrid_oracle(const map<int,int>& gold_heads,
                                 const map<int,string>& gold_rels,
                                 const map<int,int>& gold_rmcs, // rightmost childs
                                 const map<int,int>& curr_heads,
                                 const vector<int>& stacki,
                                 const vector<int>& bufferi) {

    vector<string> results;
    int b0 = bufferi.back();
    //cout << "oracle. b0=" << b0+1 << endl;
    if (b0 >= 0 && arc_hybrid_shift_loss(b0, stacki, gold_heads) == 0) {
        results.push_back("SHIFT");
        if (EXPLORE == 0) return results;
    }
    if (stacki.size() < 2) return results;
    int s0 = stacki[stacki.size()-1];
    string label = gold_rels.at(s0);
    if (arc_hybrid_left_loss(b0, stacki, gold_heads) == 0) {
        if (gold_heads.at(s0) == b0) {
            results.push_back("LEFT-ARC(" + label + ")");
        } else { // allow all labels
            for (const string& lbl : all_lefts) {
                results.push_back(lbl);
            }
        }
        if (EXPLORE == 0) return results;
    }
    if (stacki.size() < 3) {
        assert(results.size() > 0);
        return results;
    }
    int s1 = stacki.at(stacki.size()-2);
    if (arc_hybrid_right_loss(b0, stacki, gold_heads) == 0) {
        if (gold_heads.at(s0) == s1) {
            results.push_back("RIGHT-ARC(" + label + ")");
        } else { // allow all labels
            for (const string& lbl : all_rights) {
                results.push_back(lbl);
            }
        }
    }
    assert(results.size() > 0);
    return results;
}

// *** if correct_actions is empty, this runs greedy decoding ***
// returns parse actions for input sentence (in training just returns the reference)
// OOV handling: raw_sent will have the actual words
//               sent will have words replaced by appropriate UNK tokens
// this lets us use pretrained embeddings, when available, for words that were OOV in the
// parser training data
vector<unsigned> log_prob_parser(ComputationGraph* hg,
                     const vector<unsigned>& raw_sent,  // raw sentence
                     const vector<unsigned>& sent,  // sent with oovs replaced
                     const vector<unsigned>& sentPos,
                     const vector<unsigned>& correct_actions,
                     const vector<string>& setOfActions,
                     const map<unsigned, std::string>& intToWords,
		     double *right,
		     const map<int,int>& correctHeads, //Dynamic-oracles. new thing
             const map<int,string>& correctLabels //Dynamic-oracles. new thing
        ) {
    if (all_lefts.size() == 0) { // YG -- initalize once. TODO move to nicer place.
        for (const string& a : setOfActions) {
            if (a[0] == 'S') { continue; }
            if (a[0] == 'L') { all_lefts.insert(a); }
            if (a[0] == 'R') { all_rights.insert(a); }
        }
    }
    //for (unsigned i = 0; i < sent.size(); ++i) cerr << ' ' << intToWords.find(sent[i])->second;
    //cerr << endl;
    map<int,int> rightmost_child; // YG -- needed for oracle.
    for (auto& item : correctHeads) { // TODO verify correctness.
        int m = item.first;
        int h = item.second;
        if (rightmost_child.find(h) == rightmost_child.end() || rightmost_child[h] < m) {
            rightmost_child[h] = m;
        }
        if (rightmost_child.find(m) == rightmost_child.end()) { rightmost_child[m] = -1; }
    }

    map<int,int> heads;
    map<int,std::string> rels;

    vector<unsigned> results;
    const bool build_training_graph = correct_actions.size() > 0;

    stack_lstm.new_graph(*hg);
    buffer_lstm.new_graph(*hg);
    action_lstm.new_graph(*hg);
    stack_lstm.start_new_sequence();
    buffer_lstm.start_new_sequence();
    action_lstm.start_new_sequence();
    // variables in the computation graph representing the parameters
    Expression pbias = parameter(*hg, p_pbias);
    Expression H = parameter(*hg, p_H);
    Expression D = parameter(*hg, p_D);
    Expression Rl = parameter(*hg, p_Rl);
    Expression Rr = parameter(*hg, p_Rr);
    Expression cbias = parameter(*hg, p_cbias);
    Expression S = parameter(*hg, p_S);
    Expression B = parameter(*hg, p_B);
    Expression A = parameter(*hg, p_A);
    Expression ib = parameter(*hg, p_ib);
    Expression w2l = parameter(*hg, p_w2l);
    Expression p2l;
    if (USE_POS)
        p2l = parameter(*hg, p_p2l);
    Expression t2l;
    if (p_t2l)
        t2l = parameter(*hg, p_t2l);
    Expression p2a = parameter(*hg, p_p2a);
    Expression abias = parameter(*hg, p_abias);
    Expression action_start = parameter(*hg, p_action_start);

    action_lstm.add_input(action_start);

    vector<Expression> buffer(sent.size() + 1);  // variables representing word embeddings (possibly including POS info)
    vector<int> bufferi(sent.size() + 1);  // position of the words in the sentence
    // precompute buffer representation from left to right


    Expression word_end = parameter(*hg, p_end_of_word); //Miguel
    Expression word_start = parameter(*hg, p_start_of_word); //Miguel

    if (USE_SPELLING){ //{{{
        fw_char_lstm.new_graph(*hg);
        //    fw_char_lstm.add_parameter_edges(hg);

        bw_char_lstm.new_graph(*hg);
        //    bw_char_lstm.add_parameter_edges(hg);
    } //}}}

    for (unsigned i = 0; i < sent.size(); ++i) {
        assert(sent[i] < VOCAB_SIZE);
        //Expression w = lookup(*hg, p_w, sent[i]);

        unsigned wi=sent[i];
        std::string ww=intToWords.at(wi);
        Expression w;
        /**********SPELLING MODEL*****************/ // {{{
        if (USE_SPELLING) { 
            //std::cout<<"using spelling"<<"\n";
            if (ww.length()==4  && ww[0]=='R' && ww[1]=='O' && ww[2]=='O' && ww[3]=='T'){
                w=lookup(*hg, p_w, sent[i]); //we do not need a LSTM encoding for the root word, so we put it directly-.
            }
            else {

                fw_char_lstm.start_new_sequence();
                //cerr<<"start_new_sequence done"<<"\n";

                fw_char_lstm.add_input(word_start);
                //cerr<<"added start of word symbol"<<"\n";
                /*for (unsigned j=0;j<w.length();j++){

                //cerr<<j<<":"<<w[j]<<"\n"; 
                Expression cj=lookup(*hg, char_emb, w[j]);
                fw_char_lstm.add_input(cj, hg);

                //std::cout<<"Inputdim:"<<LSTM_INPUT_DIM<<"\n";  
                //hg->incremental_forward();

                }*/
                std::vector<int> strevbuffer;
                for (unsigned j=0;j<ww.length();j+=UTF8Len(ww[j])){

                    //cerr<<j<<":"<<w[j]<<"\n"; 
                    std::string wj;
                    for (unsigned h=j;h<j+UTF8Len(ww[j]);h++) wj+=ww[h];
                    //std::cout<<"fw"<<wj<<"\n";
                    int wjint=corpus.charsToInt[wj];
                    //std::cout<<"fw:"<<wjint<<"\n";
                    strevbuffer.push_back(wjint);
                    Expression cj=lookup(*hg, char_emb, wjint);
                    fw_char_lstm.add_input(cj);

                    //std::cout<<"Inputdim:"<<LSTM_INPUT_DIM<<"\n";  
                    //hg->incremental_forward();

                }
                fw_char_lstm.add_input(word_end);
                //cerr<<"added end of word symbol"<<"\n";



                Expression fw_i=fw_char_lstm.back();

                //cerr<<"fw_char_lstm.back() done"<<"\n";

                bw_char_lstm.start_new_sequence();
                //cerr<<"bw start new sequence done"<<"\n";

                bw_char_lstm.add_input(word_end);
                //for (unsigned j=w.length()-1;j>=0;j--){
                /*for (unsigned j=w.length();j-->0;){
                //cerr<<j<<":"<<w[j]<<"\n";
                Expression cj=lookup(*hg, char_emb, w[j]);
                bw_char_lstm.add_input(cj); 
                }*/
                //}
                while(!strevbuffer.empty()) {
                    int wjint=strevbuffer.back();
                    //std::cout<<"bw:"<<wjint<<"\n";
                    Expression cj=lookup(*hg, char_emb, wjint);
                    bw_char_lstm.add_input(cj);
                    strevbuffer.pop_back();
                }

                /*for (unsigned j=w.length()-1;j>0;j=j-UTF8Len(w[j])) {

                //cerr<<j<<":"<<w[j]<<"\n"; 
                std::string wj;
                for (unsigned h=j;h<j+UTF8Len(w[j]);h++) wj+=w[h];
                std::cout<<"bw"<<wj<<"\n";
                int wjint=corpus.charsToInt[wj];
                Expression cj=lookup(*hg, char_emb, wjint);
                bw_char_lstm.add_input(cj);

                //std::cout<<"Inputdim:"<<LSTM_INPUT_DIM<<"\n";  
                //hg->incremental_forward();

                }*/
                bw_char_lstm.add_input(word_start);
                //cerr<<"start symbol in bw seq"<<"\n";     

                Expression bw_i=bw_char_lstm.back();

                vector<Expression> tt = {fw_i, bw_i};
                w=concatenate(tt); //and this goes into the buffer...
                //cerr<<"fw and bw done"<<"\n";
            }

        } 
        /**************************************************/
        //cerr<<"concatenate?"<<"\n";

        //  }}}
        /***************NO SPELLING*************************************/

        // Expression w = lookup(*hg, p_w, sent[i]);
        else { //NO SPELLING
            //Don't use SPELLING
            //std::cout<<"don't use spelling"<<"\n";
            w=lookup(*hg, p_w, sent[i]);
        }

        Expression i_i;
        if (USE_POS) {
            Expression p = lookup(*hg, p_p, sentPos[i]);
            i_i = affine_transform({ib, w2l, w, p2l, p});
        } else {
            i_i = affine_transform({ib, w2l, w});
        }
        if (p_t && pretrained.count(raw_sent[i])) {
            Expression t = const_lookup(*hg, p_t, raw_sent[i]);
            i_i = affine_transform({i_i, t2l, t});
        }
        buffer[sent.size() - i] = rectify(i_i);
        bufferi[sent.size() - i] = i;
    }
    // dummy symbol to represent the empty buffer
    buffer[0] = parameter(*hg, p_buffer_guard);
    bufferi[0] = -999;
    for (auto& b : buffer)
        buffer_lstm.add_input(b);

    vector<Expression> stack;  // variables representing subtree embeddings
    vector<int> stacki; // position of words in the sentence of head of subtree
    stack.push_back(parameter(*hg, p_stack_guard));
    stacki.push_back(-999); // not used for anything
    // drive dummy symbol on stack through LSTM
    stack_lstm.add_input(stack.back());
    vector<Expression> log_probs;
    string rootword;
    unsigned action_count = 0;  // incremented at each prediction

    for (int i = 1; i < bufferi.size(); ++i) {
        //cout << "h[" << bufferi[i]+1 << "]=" << correctHeads.at(bufferi[i]) + 1 << endl; 
    }
    while(stacki.size() > 2 || bufferi.size() > 1) {
    //while(stack.size() > 2 || buffer.size() > 1) { //}

        // get list of possible actions for the current parser state
        vector<unsigned> current_valid_actions;
        for (auto a: possible_actions) {
            if (IsActionForbidden(setOfActions[a], buffer.size(), stack.size(), stacki))
                continue;
            current_valid_actions.push_back(a);
        }
        if (0) { // verify correctness mode
        // {{{
            vector<string> gold_actions = arc_hybrid_oracle(  correctHeads,correctLabels,rightmost_child,heads,stacki,bufferi);
            ++action_count;

            const string& actionString=gold_actions[0]; // TODO random
            for (int _i = 0; _i < setOfActions.size(); ++_i) {
                if (setOfActions[_i] == actionString) {
                    results.push_back(_i);
                    break;
                }
            }
            const char ac = actionString[0];
            const char ac2 = actionString[1];
            //cout << "action " << actionString << endl;


            if (ac =='S' && ac2=='H') {  // SHIFT
                stacki.push_back(bufferi.back());
                bufferi.pop_back();
            }  else { // LEFT or RIGHT
                    unsigned depi = 0, headi = 0;
                    depi = stacki.back();
                    stacki.pop_back();

                    if (ac == 'L') { // head is on buffer
                        headi = bufferi.back();
                    }
                    if (ac == 'R') { // head is next on stack
                        headi = stacki.back();
                        stacki.pop_back();
                    }

                    //for Dynamic oracles
                    heads[depi] = headi;        // not sure this is really needed
                    rels[depi] = actionString;  // not sure this is really needed

                    if (headi == sent.size() - 1) rootword = intToWords.find(sent[depi])->second;
                    if (ac == 'R') {
                        stacki.push_back(headi);
                    } else { // ac == 'L'
                        //MB -- if I'm not mistaken, this was missing. (see below in line 926. For bufferi it is not needed since it is the same representation).
                    }
                }
            continue;
        } 
        // }}}

        // p_t = pbias + S * slstm + B * blstm + A * almst
        Expression p_t = affine_transform({pbias, S, stack_lstm.back(), B, buffer_lstm.back(), A, action_lstm.back()});
        Expression nlp_t = rectify(p_t);
        // r_t = abias + p2a * nlp
        Expression r_t = affine_transform({abias, p2a, nlp_t});

        // adist = log_softmax(r_t, current_valid_actions)
        Expression adiste = log_softmax(r_t, current_valid_actions);
        vector<float> adist = as_vector(hg->incremental_forward());
        double best_score = adist[current_valid_actions[0]];
        unsigned best_a = current_valid_actions[0];
        for (unsigned i = 1; i < current_valid_actions.size(); ++i) {
            if (adist[current_valid_actions[i]] > best_score) {
                best_score = adist[current_valid_actions[i]];
                best_a = current_valid_actions[i];
            }
        }
        unsigned action = best_a;
        unsigned action_to_follow;
        vector<Expression> exp_golds;
        if (build_training_graph) {  // if we have reference actions (for training) use the reference action
            vector<string> gold_actions = (use_arc_hybrid) ?
                arc_hybrid_oracle(  correctHeads,correctLabels,rightmost_child,heads,stacki,bufferi)
                : arc_standard_oracle(correctHeads,correctLabels,rightmost_child,heads,stacki,bufferi); // YG
            // YG -- this block is just for verification sake
            // however, it will not work once we start deviating from the gold path.
            if (0) {
                action = correct_actions[action_count];
                string saction = setOfActions[action];
                //cout << saction << " " << gold_actions.front() << " " << gold_actions.back() << endl;
                assert(gold_actions.front() == saction || gold_actions.back() == saction);
            }
            // YG -- end verification block

            // YG -- let action = highest scoring in gold_actions
            // YG -- change below to if best_a is contained in gold_action (instead of ==)
            //if (best_a == action) { (*right)++; }
            unsigned best_gold = corpus.ractions[gold_actions[0]];
            double best_gold_score = adist[best_gold];
            int correct = 0;
            for (auto& sact : gold_actions) {
                unsigned gact = corpus.ractions[sact];
                exp_golds.push_back(exp(pick(adiste, gact)));
                if (gact == best_a) { correct=1; }
                if (adist[gact] >= best_gold_score) {
                    best_gold = gact;
                    best_gold_score = adist[gact];
                }
            }
            (*right)+=correct;
            action = best_gold; // YG -- TODO: change this once we have a really dynamic oracle.
            // once we have a dynamic oracle, need to decouple between the 
            // action to update towards, and the action to actually take.
            // the action we will be taking will not be the oracle's action,
            // but a random actions according to the model's probability.

            if (EXPLORE != 0 && (CUR_ITER > 50))
                action_to_follow = sample_from(adist, exploration_smoothing);
            else
                action_to_follow = best_gold;

            if (std::find(current_valid_actions.begin(), current_valid_actions.end(), action_to_follow) == current_valid_actions.end()) {
                cout << "damn. sampled action is invalid! " << setOfActions[action_to_follow] << std::endl;
                for (int a : current_valid_actions) { cout << " " << setOfActions[a] << std::endl; }
            }
            //cout << "sampled_action " << action_to_follow << ":" << exp(adist[action_to_follow]) << " vs. " << best_a << ":" << exp(adist[best_a]) << endl;
            // YG -- TODO: change below to follow action_to_follow instead of action (if training).
            //             the update should still be towards "action" (or better, towards ALL the correct actions).
            //             we can either follow "action to follow" from the beginning, or only after we already have an ok-ish model.

            // YG -- also, if we have two gold actions, need to put this in loss somehow.
            //       not sure how to do this technically, though. Chris?
            //       for now we can leave it as is and things will probably work,
            //       though slightly less good I guess.

        }
        ++action_count;
        // action_log_prob = pick(adist, action)
        if (update_towards_one_best) {
            log_probs.push_back(pick(adiste, action));
        } else {
            if (build_training_graph)
                log_probs.push_back(log(sum(exp_golds)));
            else
                log_probs.push_back(pick(adiste, action));
        }
        results.push_back(action);

        // YG here we switch to the action to follow?
        if (build_training_graph) { action = action_to_follow; }
        // add current action to action LSTM
        Expression actione = lookup(*hg, p_a, action);
        action_lstm.add_input(actione);

        // get relation embedding from action (TODO: convert to relation from action?)
        Expression relation = lookup(*hg, p_r, action);

        // do action
        const string& actionString=setOfActions[action];
        //cerr << "A=" << actionString << " Bsize=" << buffer.size() << " Ssize=" << stack.size() << endl;
        const char ac = actionString[0];
        const char ac2 = actionString[1];


        if (ac =='S' && ac2=='H') {  // SHIFT
            assert(buffer.size() > 1); // dummy symbol means > 1 (not >= 1)
            stack.push_back(buffer.back());
            stack_lstm.add_input(buffer.back());
            buffer.pop_back();
            buffer_lstm.rewind_one_step();
            stacki.push_back(bufferi.back());
            bufferi.pop_back();
        } 
        else if (ac=='S' && ac2=='W'){ //SWAP --- Miguel {{{
            assert(stack.size() > 2); // dummy symbol means > 2 (not >= 2)

            //std::cout<<"SWAP: "<<"stack.size:"<<stack.size()<<"\n";

            Expression toki, tokj;
            unsigned ii = 0, jj = 0;
            tokj=stack.back();
            jj=stacki.back();
            stack.pop_back();
            stacki.pop_back();

            toki=stack.back();
            ii=stacki.back();
            stack.pop_back();
            stacki.pop_back();

            buffer.push_back(toki);
            bufferi.push_back(ii);

            stack_lstm.rewind_one_step();
            stack_lstm.rewind_one_step();


            buffer_lstm.add_input(buffer.back());

            stack.push_back(tokj);
            stacki.push_back(jj);

            stack_lstm.add_input(stack.back());

            //stack_lstm.rewind_one_step();
            //buffer_lstm.rewind_one_step();
            // }}}
        } else { // LEFT or RIGHT
            if (use_arc_hybrid) { // YG
                assert(stack.size() > 1); // need one more than guard symbol
                Expression dep, head;
                unsigned depi = 0, headi = 0;
                dep = stack.back();
                depi = stacki.back();
                stack.pop_back();
                stacki.pop_back();

                if (ac == 'L') { // head is on buffer
                    head = buffer.back();
                    headi = bufferi.back();
                }
                if (ac == 'R') { // head is next on stack
                    head = stack.back();
                    headi = stacki.back();
                    stack.pop_back();
                    stacki.pop_back();
                }

                //for Dynamic oracles
                heads[depi] = headi;        // not sure this is really needed
                rels[depi] = actionString;  // not sure this is really needed

                if (build_training_graph) {  // if we are in training mode, and not using exploration, verify arc is correct (for oracle debugging)
                    //cout << depi << " " << heads.at(depi) << " " << correctHeads.at(depi) << endl;
                    //assert(heads.at(depi) == correctHeads.at(depi));
                }
                if (headi == sent.size() - 1) rootword = intToWords.find(sent[depi])->second;
                // composed = cbias + H * head + D * dep + R * relation
                Expression composed = (orig_compose || ac == 'L') ? affine_transform({cbias, H, head, D, dep, Rl, relation}) :
                    affine_transform({cbias, H, head, D, dep, Rr, relation});
                Expression nlcomposed = tanh(composed);
                // YG -- Miguel, can you verify these following lines?
                // I am pretty sure they are correct for the RIGHT transition, but not so sure for LEFT
                // MB -- see below.
                if (ac == 'R') {
                    stack_lstm.rewind_one_step();  
                    stack_lstm.rewind_one_step();
                    stack_lstm.add_input(nlcomposed);
                    stack.push_back(nlcomposed);
                    stacki.push_back(headi);
                } else { // ac == 'L'
                    stack_lstm.rewind_one_step();
                    buffer_lstm.rewind_one_step();
                    buffer_lstm.add_input(nlcomposed);
                    //MB -- if I'm not mistaken, this was missing. (see below in line 926. For bufferi it is not needed since it is the same representation).
                    buffer.pop_back();
                    buffer.push_back(nlcomposed);
                }
            } else { // arc-standard
                assert(stack.size() > 2); // dummy symbol means > 2 (not >= 2)
                assert(ac == 'L' || ac == 'R');
                Expression dep, head;
                unsigned depi = 0, headi = 0;
                (ac == 'R' ? dep : head) = stack.back();
                (ac == 'R' ? depi : headi) = stacki.back();
                stack.pop_back();
                stacki.pop_back();
                (ac == 'R' ? head : dep) = stack.back();
                (ac == 'R' ? headi : depi) = stacki.back();
                stack.pop_back();
                stacki.pop_back();

                //for Dynamic oracles
                heads[depi] = headi;
                rels[depi] = actionString;
                if (headi == sent.size() - 1) rootword = intToWords.find(sent[depi])->second;
                // composed = cbias + H * head + D * dep + R * relation
                Expression composed = (orig_compose || ac == 'L') ? affine_transform({cbias, H, head, D, dep, Rl, relation}) :
                    affine_transform({cbias, H, head, D, dep, Rr, relation});
                Expression nlcomposed = tanh(composed);
                stack_lstm.rewind_one_step();
                stack_lstm.rewind_one_step();
                stack_lstm.add_input(nlcomposed);
                stack.push_back(nlcomposed);
                stacki.push_back(headi);
            }
        }
    }
    //cout << "done sentence" << endl;
    assert(stack.size() == 2); // guard symbol, root
    assert(stacki.size() == 2);
    assert(buffer.size() == 1); // guard symbol
    assert(bufferi.size() == 1);
    Expression tot_neglogprob = -sum(log_probs);
    assert(tot_neglogprob.pg != nullptr);
    return results;
}

struct ParserState {
  LSTMBuilder stack_lstm;
  LSTMBuilder buffer_lstm;
  LSTMBuilder action_lstm;
  vector<Expression> buffer;
  vector<int> bufferi;
  vector<Expression> stack;
  vector<int> stacki;
  vector<unsigned> results;  // sequence of predicted actions
  bool complete;

  double score;
};

struct ParserStateCompare {
  bool operator()(const ParserState& a, const ParserState& b) const {
    return a.score > b.score;
  }
};

static void prune(vector<ParserState>& pq, unsigned k) {
  if (pq.size() == 1) return;
  if (k > pq.size()) k = pq.size();
  partial_sort(pq.begin(), pq.begin() + k, pq.end(), ParserStateCompare());
  pq.resize(k);
  reverse(pq.begin(), pq.end());
  //cerr << "PRUNE\n";
  //for (unsigned i = 0; i < pq.size(); ++i) {
  //  cerr << pq[i].score << endl;
  //}
}

static bool all_complete(const vector<ParserState>& pq) {
  for (auto& ps : pq) if (!ps.complete) return false;
  return true;
}

// run beam search
vector<unsigned> log_prob_parser_beam(ComputationGraph* hg,
                     const vector<unsigned>& raw_sent,  // raw sentence
                     const vector<unsigned>& sent,  // sent with OOVs replaced
                     const vector<unsigned>& sentPos,
                     const vector<string>& setOfActions,
                     unsigned beam_size, double* log_prob) {
    abort();
#if 0
    vector<unsigned> results;
    ParserState init;

    stack_lstm.new_graph(hg);
    buffer_lstm.new_graph(hg);
    action_lstm.new_graph(hg);
    // variables in the computation graph representing the parameters
    Expression pbias = parameter(*hg, p_pbias);
    Expression H = parameter(*hg, p_H);
    Expression D = parameter(*hg, p_D);
    Expression R = parameter(*hg, p_R);
    Expression cbias = parameter(*hg, p_cbias);
    Expression S = parameter(*hg, p_S);
    Expression B = parameter(*hg, p_B);
    Expression A = parameter(*hg, p_A);
    Expression ib = parameter(*hg, p_ib);
    Expression w2l = parameter(*hg, p_w2l);
    Expression p2l;
    if (USE_POS)
      i_p2l = parameter(*hg, p_p2l);
    Expression t2l;
    if (p_t2l)
      i_t2l = parameter(*hg, p_t2l);
    Expression p2a = parameter(*hg, p_p2a);
    Expression abias = parameter(*hg, p_abias);
    Expression action_start = parameter(*hg, p_action_start);

    action_lstm.add_input(i_action_start, hg);

    vector<Expression> buffer(sent.size() + 1);  // variables representing word embeddings (possibly including POS info)
    vector<int> bufferi(sent.size() + 1);  // position of the words in the sentence

    // precompute buffer representation from left to right
    for (unsigned i = 0; i < sent.size(); ++i) {
      assert(sent[i] < VOCAB_SIZE);
      Expression w = lookup(*hg, p_w, sent[i]);
      Expression i;
      if (USE_POS) {
        Expression p = lookup(*hg, p_p, sentPos[i]);
        i_i = hg->add_function<AffineTransform>({i_ib, i_w2l, i_w, i_p2l, i_p});
      } else {
        i_i = hg->add_function<AffineTransform>({i_ib, i_w2l, i_w});
      }
      if (p_t && pretrained.count(raw_sent[i])) {
        Expression t = hg->add_const_lookup(p_t, sent[i]);
        i_i = hg->add_function<AffineTransform>({i_i, i_t2l, i_t});
      }
      Expression inl = hg->add_function<Rectify>({i_i});
      buffer[sent.size() - i] = i_inl;
      bufferi[sent.size() - i] = i;
    }
    // dummy symbol to represent the empty buffer
    buffer[0] = parameter(*hg, p_buffer_guard);
    bufferi[0] = -999;
    for (auto& b : buffer)
      buffer_lstm.add_input(b, hg);

    vector<Expression> stack;  // variables representing subtree embeddings
    vector<int> stacki; // position of words in the sentence of head of subtree
    stack.push_back(parameter(*hg, p_stack_guard));
    stacki.push_back(-999); // not used for anything
    // drive dummy symbol on stack through LSTM
    stack_lstm.add_input(stack.back(), hg);

    init.stack_lstm = stack_lstm;
    init.buffer_lstm = buffer_lstm;
    init.action_lstm = action_lstm;
    init.buffer = buffer;
    init.bufferi = bufferi;
    init.stack = stack;
    init.stacki = stacki;
    init.results = results;
    init.score = 0;
    if (init.stacki.size() ==1 && init.bufferi.size() == 1) { assert(!"bad0"); }

    vector<ParserState> pq;
    pq.push_back(init);
    vector<ParserState> completed;
    while (pq.size() > 0) {
      const ParserState cur = pq.back();
      pq.pop_back();
      if (cur.stack.size() == 2 && cur.buffer.size() == 1) {
        completed.push_back(cur);
        if (completed.size() == beam_size) break;
        continue;
      }

      // get list of possible actions for the current parser state
      vector<unsigned> current_valid_actions;
      for (auto a: possible_actions) {
        if (IsActionForbidden(setOfActions[a], cur.buffer.size(), cur.stack.size(), stacki))
          continue;
        current_valid_actions.push_back(a);
      }

      // p_t = pbias + S * slstm + B * blstm + A * almst
      Expression p_t = hg->add_function<AffineTransform>({i_pbias, i_S, cur.stack_lstm.back(), i_B, cur.buffer_lstm.back(), i_A, cur.action_lstm.back()});

      // nlp_t = tanh(p_t)
      Expression nlp_t = hg->add_function<Rectify>({i_p_t});

      // r_t = abias + p2a * nlp
      Expression r_t = hg->add_function<AffineTransform>({i_abias, i_p2a, i_nlp_t});

      //cerr << "CVAs: " << current_valid_actions.size() << " (cur.buf=" << cur.bufferi.size() << " buf.sta=" << cur.stacki.size() << ")\n";
      // adist = log_softmax(r_t)
      hg->add_function<RestrictedLogSoftmax>({i_r_t}, current_valid_actions);
      vector<float> adist = as_vector(hg->incremental_forward());

      for (auto action : current_valid_actions) {
        pq.resize(pq.size() + 1);
        ParserState& ns = pq.back();
        ns = cur;  // copy current state to new state
        ns.score += adist[action];
        ns.results.push_back(action);

        // add current action to action LSTM
        Expression action = lookup(*hg, p_a, action);
        ns.action_lstm.add_input(i_action, hg);

        // do action
        const string& actionString=setOfActions[action];
        //cerr << "A=" << actionString << " Bsize=" << buffer.size() << " Ssize=" << stack.size() << endl;
        const char ac = actionString[0];
        if (ac =='S') {  // SHIFT
          assert(ns.buffer.size() > 1); // dummy symbol means > 1 (not >= 1)
          ns.stack.push_back(ns.buffer.back());
          ns.stack_lstm.add_input(ns.buffer.back(), hg);
          ns.buffer.pop_back();
          ns.buffer_lstm.rewind_one_step();
          ns.stacki.push_back(cur.bufferi.back());
          ns.bufferi.pop_back();
        } else { // LEFT or RIGHT
          assert(ns.stack.size() > 2); // dummy symbol means > 2 (not >= 2)
          assert(ac == 'L' || ac == 'R');
          Expression dep, head;
          unsigned depi = 0, headi = 0;
          (ac == 'R' ? dep : head) = ns.stack.back();
          (ac == 'R' ? depi : headi) = ns.stacki.back();
          ns.stack.pop_back();
          ns.stacki.pop_back();
          (ac == 'R' ? head : dep) = ns.stack.back();
          (ac == 'R' ? headi : depi) = ns.stacki.back();
          ns.stack.pop_back();
          ns.stacki.pop_back();
          // get relation embedding from action (TODO: convert to relation from action?)
          Expression relation = lookup(*hg, p_r, action);

          // composed = cbias + H * head + D * dep + R * relation
        Expression composed = (ac == 'L') ? affine_transform({cbias, H, head, D, dep, Rl, relation}) :
                                            affine_transform({cbias, H, head, D, dep, Rr, relation});
          // nlcomposed = tanh(composed)
          Expression nlcomposed = tanh(composed);
          ns.stack_lstm.rewind_one_step();
          ns.stack_lstm.rewind_one_step();
          ns.stack_lstm.add_input(i_nlcomposed, hg);
          ns.stack.push_back(i_nlcomposed);
          ns.stacki.push_back(headi);
        }
      } // all curent actions
      prune(pq, beam_size);
    } // beam search
    assert(completed.size() > 0);
    prune(completed, 1);
    results = completed.back().results;
    assert(completed.back().stack.size() == 2); // guard symbol, root
    assert(completed.back().stacki.size() == 2);
    assert(completed.back().buffer.size() == 1); // guard symbol
    assert(completed.back().bufferi.size() == 1);
    *log_prob = completed.back().score;


    return results;
#endif
  }
};

void signal_callback_handler(int /* signum */) {
  if (requested_stop) {
    cerr << "\nReceived SIGINT again, quitting.\n";
    _exit(1);
  }
  cerr << "\nReceived SIGINT terminating optimization early...\n";
  requested_stop = true;
}

unsigned compute_correct(const map<int,int>& ref, const map<int,int>& hyp, unsigned len) {
  unsigned res = 0;
  for (unsigned i = 0; i < len; ++i) {
    auto ri = ref.find(i);
    auto hi = hyp.find(i);
    assert(ri != ref.end());
    assert(hi != hyp.end());
    if (ri->second == hi->second) ++res;
  }
  return res;
}

void output_conll(const vector<unsigned>& sentence, const vector<unsigned>& pos,
                  const vector<string>& sentenceUnkStrings, 
                  const map<unsigned, string>& intToWords, 
                  const map<unsigned, string>& intToPos, 
                  const map<int,int>& hyp, const map<int,string>& rel_hyp) {
  for (unsigned i = 0; i < (sentence.size()-1); ++i) {
    auto index = i + 1;
    assert(i < sentenceUnkStrings.size() && 
           ((sentence[i] == corpus.get_or_add_word(cpyp::Corpus::UNK) &&
             sentenceUnkStrings[i].size() > 0) ||
            (sentence[i] != corpus.get_or_add_word(cpyp::Corpus::UNK) &&
             sentenceUnkStrings[i].size() == 0 &&
             intToWords.find(sentence[i]) != intToWords.end())));
    string wit = (sentenceUnkStrings[i].size() > 0)? 
      sentenceUnkStrings[i] : intToWords.find(sentence[i])->second;
    auto pit = intToPos.find(pos[i]);
    assert(hyp.find(i) != hyp.end());
    auto hyp_head = hyp.find(i)->second + 1;
    if (hyp_head == (int)sentence.size()) hyp_head = 0;
    auto hyp_rel_it = rel_hyp.find(i);
    assert(hyp_rel_it != rel_hyp.end());
    auto hyp_rel = hyp_rel_it->second;
    size_t first_char_in_rel = hyp_rel.find('(') + 1;
    size_t last_char_in_rel = hyp_rel.rfind(')') - 1;
    hyp_rel = hyp_rel.substr(first_char_in_rel, last_char_in_rel - first_char_in_rel + 1);
    cout << index << '\t'       // 1. ID 
         << wit << '\t'         // 2. FORM
         << "_" << '\t'         // 3. LEMMA 
         << "_" << '\t'         // 4. CPOSTAG 
         << pit->second << '\t' // 5. POSTAG
         << "_" << '\t'         // 6. FEATS
         << hyp_head << '\t'    // 7. HEAD
         << hyp_rel << '\t'     // 8. DEPREL
         << "_" << '\t'         // 9. PHEAD
         << "_" << endl;        // 10. PDEPREL
  }
  cout << endl;
}

int main(int argc, char** argv) {
  cnn::Initialize(argc, argv);

  cerr << "COMMAND:"; 
  for (unsigned i = 0; i < static_cast<unsigned>(argc); ++i) cerr << ' ' << argv[i];
  cerr << endl;
  unsigned status_every_i_iterations = 100;

  po::variables_map conf;
  InitCommandLine(argc, argv, &conf);
  USE_POS = conf.count("use_pos_tags");

  EXPLORE = conf.count("explore");
  use_arc_hybrid = conf.count("archyb");
  update_towards_one_best = conf.count("one_best_update");
  orig_compose = conf.count("compose1");
  exploration_smoothing = conf["sampling_pow"].as<float>();
  cerr << "Exploration smoothing is:" << exploration_smoothing << endl;

  USE_SPELLING=conf.count("use_spelling"); //Miguel
  corpus.USE_SPELLING=USE_SPELLING;

  LAYERS = conf["layers"].as<unsigned>();
  INPUT_DIM = conf["input_dim"].as<unsigned>();
  PRETRAINED_DIM = conf["pretrained_dim"].as<unsigned>();
  HIDDEN_DIM = conf["hidden_dim"].as<unsigned>();
  ACTION_DIM = conf["action_dim"].as<unsigned>();
  LSTM_INPUT_DIM = conf["lstm_input_dim"].as<unsigned>();
  POS_DIM = conf["pos_dim"].as<unsigned>();
  REL_DIM = conf["rel_dim"].as<unsigned>();
  const unsigned beam_size = conf["beam_size"].as<unsigned>();
  const unsigned unk_strategy = conf["unk_strategy"].as<unsigned>();
  cerr << "Unknown word strategy: ";
  if (unk_strategy == 1) {
    cerr << "STOCHASTIC REPLACEMENT\n";
  } else {
    abort();
  }
  const double unk_prob = conf["unk_prob"].as<double>();
  assert(unk_prob >= 0.); assert(unk_prob <= 1.);
  ostringstream os;
  os << "parser_" << (USE_POS ? "pos" : "nopos")
     << '_' << LAYERS
     << '_' << INPUT_DIM
     << '_' << HIDDEN_DIM
     << '_' << ACTION_DIM
     << '_' << LSTM_INPUT_DIM
     << '_' << POS_DIM
     << '_' << REL_DIM
     << '_' << (use_arc_hybrid ? "archyb" : "arcstd")
     << '_' << (EXPLORE ? "exp" : "noexp")
     << '_' << (update_towards_one_best ? "1best" : "dist")
     << '_' << (orig_compose ? "comp1" : "comp2")
     << "-pid" << getpid() << ".params";
  int best_correct_heads = 0;
  string fname = os.str();
  if (conf.count("out_model") > 0) {
      fname = conf["out_model"].as<string>();
  }
  cerr << "Writing parameters to file: " << fname << endl;
  bool softlinkCreated = false;
  corpus.load_correct_actions(conf["training_data"].as<string>());	
  const unsigned kUNK = corpus.get_or_add_word(cpyp::Corpus::UNK);
  kROOT_SYMBOL = corpus.get_or_add_word(ROOT_SYMBOL);
  //cout <<"KROOT:"<<kROOT_SYMBOL<<"\n";

  if (conf.count("words")) {
    pretrained[kUNK] = vector<float>(PRETRAINED_DIM, 0);
    cerr << "Loading from " << conf["words"].as<string>() << " with" << PRETRAINED_DIM << " dimensions\n";
    ifstream in(conf["words"].as<string>().c_str());
    string line;
    getline(in, line);
    vector<float> v(PRETRAINED_DIM, 0);
    string word;
    while (getline(in, line)) {
      istringstream lin(line);
      lin >> word;
      for (unsigned i = 0; i < PRETRAINED_DIM; ++i) lin >> v[i];
      unsigned id = corpus.get_or_add_word(word);
      pretrained[id] = v;
    }
  }

  set<unsigned> training_vocab; // words available in the training corpus
  set<unsigned> singletons;
  {  // compute the singletons in the parser's training data
    map<unsigned, unsigned> counts;
    for (auto sent : corpus.sentences)
      for (auto word : sent.second) { training_vocab.insert(word); counts[word]++; }
    for (auto wc : counts)
      if (wc.second == 1) singletons.insert(wc.first);
  }

  cerr << "Number of words: " << corpus.nwords << endl;
  VOCAB_SIZE = corpus.nwords + 1;

  cerr << "Number of UTF8 chars: " << corpus.maxChars << endl;
  if (corpus.maxChars>255) CHAR_SIZE=corpus.maxChars;

  ACTION_SIZE = corpus.nactions + 1;
  //POS_SIZE = corpus.npos + 1;
  POS_SIZE = corpus.npos + 10;
  possible_actions.resize(corpus.nactions);
  for (unsigned i = 0; i < corpus.nactions; ++i)
    possible_actions[i] = i;

  Model model;
  ParserBuilder parser(&model, pretrained);
  if (conf.count("model")) {
    ifstream in(conf["model"].as<string>().c_str());
    boost::archive::text_iarchive ia(in);
    ia >> model;
  }

  // OOV words will be replaced by UNK tokens
  corpus.load_correct_actionsDev(conf["dev_data"].as<string>());
  if (USE_SPELLING) VOCAB_SIZE = corpus.nwords + 1;
  //TRAINING
  if (conf.count("train")) {
    signal(SIGINT, signal_callback_handler);
    SimpleSGDTrainer sgd(&model);
    //MomentumSGDTrainer sgd(&model);
    sgd.eta_decay = 0.08;
    //sgd.eta_decay = 0.05;
    cerr << "Training started."<<"\n";
    vector<unsigned> order(corpus.nsentences);
    for (unsigned i = 0; i < corpus.nsentences; ++i)
      order[i] = i;
    double tot_seen = 0;
    status_every_i_iterations = min(status_every_i_iterations, corpus.nsentences);
    unsigned si = corpus.nsentences;
    cerr << "NUMBER OF TRAINING SENTENCES: " << corpus.nsentences << endl;
    unsigned trs = 0;
    double right = 0;
    double llh = 0;
    bool first = true;
    int iter = -1;
    while(!requested_stop) {
      ++iter;
      CUR_ITER = iter;
      for (unsigned sii = 0; sii < status_every_i_iterations; ++sii) {
          if (si == corpus.nsentences) {
              si = 0;
              if (first) { first = false; } else { sgd.update_epoch(); }
              cerr << "**SHUFFLE\n";
              random_shuffle(order.begin(), order.end());
          }
          tot_seen += 1;
          const vector<unsigned>& sentence=corpus.sentences[order[si]];
          vector<unsigned> tsentence=sentence;
          if (unk_strategy == 1) {
              for (auto& w : tsentence)
                  if (singletons.count(w) && cnn::rand01() < unk_prob) w = kUNK;
          }
          const vector<unsigned>& sentencePos=corpus.sentencesPos[order[si]]; 
          const vector<unsigned>& actions=corpus.correct_act_sent[order[si]];
          ComputationGraph hg;

          map<int, string> rel_ref1;
          map<int,int> ref1 = parser.compute_heads(sentence.size(), actions, corpus.actions, &rel_ref1); //Dynamic oracles. New thing.
          parser.log_prob_parser(&hg,sentence,tsentence,sentencePos,actions,corpus.actions,corpus.intToWords,&right,ref1,rel_ref1);

          double lp = as_scalar(hg.incremental_forward());
          if (lp < 0) {
              cerr << "Log prob < 0 on sentence " << order[si] << ": lp=" << lp << endl;
              //assert(lp >= 0.0);
          }
          if (lp >= 0) {
              hg.backward();
              sgd.update(1.0);
              llh += lp;
              trs += actions.size();
          }
          ++si;
      }
      sgd.status();
      cerr << "update #" << iter << " (epoch " << (tot_seen / corpus.nsentences) << ")\tllh: "<< llh<<" ppl: " << exp(llh / trs) << " err: " << (trs - right) / trs << endl;
      llh = trs = right = 0;

      static int logc = 0;
      ++logc;
      if (logc % 25 == 1) { // report on dev set
          unsigned dev_size = corpus.nsentencesDev;
          // dev_size = 100;
          double llh = 0;
          double trs = 0;
          double right = 0;
          double correct_heads = 0;
          double total_heads = 0;
          auto t_start = std::chrono::high_resolution_clock::now();
          for (unsigned sii = 0; sii < dev_size; ++sii) {
              const vector<unsigned>& sentence=corpus.sentencesDev[sii];
              const vector<unsigned>& sentencePos=corpus.sentencesPosDev[sii]; 
              const vector<unsigned>& actions=corpus.correct_act_sentDev[sii];
              vector<unsigned> tsentence=sentence;
              if (!USE_SPELLING) {
                  for (auto& w : tsentence)
                      if (training_vocab.count(w) == 0) w = kUNK;
              }

              ComputationGraph hg;
              map<int, string> rel_ref2;
              map<int,int> ref2= parser.compute_heads(sentence.size(), actions, corpus.actions, &rel_ref2); //Dynamic oracles. New thing.
              vector<unsigned> pred = parser.log_prob_parser(&hg,sentence,tsentence,sentencePos,vector<unsigned>(),corpus.actions,corpus.intToWords,&right,ref2,rel_ref2);
              double lp = 0;
              //vector<unsigned> pred = parser.log_prob_parser_beam(&hg,sentence,sentencePos,corpus.actions,beam_size,&lp);
              llh -= lp;
              trs += actions.size();
              map<int,int> ref = parser.compute_heads(sentence.size(), actions, corpus.actions);
              map<int,int> hyp = parser.compute_heads_hybrid(sentence.size(), pred, corpus.actions); // YG -- TODO: hybrid or standard
              //output_conll(sentence, corpus.intToWords, ref, hyp);
              correct_heads += compute_correct(ref, hyp, sentence.size() - 1);
              total_heads += sentence.size() - 1;
          }
          auto t_end = std::chrono::high_resolution_clock::now();
          cerr << "  **dev (iter=" << iter << " epoch=" << (tot_seen / corpus.nsentences) << ")\tllh=" << llh << " ppl: " << exp(llh / trs) << " err: " << (trs - right) / trs << " uas: " << (correct_heads / total_heads) << "\t[" << dev_size << " sents in " << std::chrono::duration<double, std::milli>(t_end-t_start).count() << " ms]" << endl;
          if (correct_heads > best_correct_heads) {
              best_correct_heads = correct_heads;
              ofstream out(fname);
              boost::archive::text_oarchive oa(out);
              oa << model;
              // Create a soft link to the most recent model in order to make it
              // easier to refer to it in a shell script.
              if (!softlinkCreated) {
                  string softlink = " latest_model";
                  if (system((string("rm -f ") + softlink).c_str()) == 0 && 
                          system((string("ln -s ") + fname + softlink).c_str()) == 0) {
                      cerr << "Created " << softlink << " as a soft link to " << fname 
                          << " for convenience." << endl;
                  }
                  softlinkCreated = true;
              }
          }
      }
    }
  } // should do training?
  if (true) { // do test evaluation
    double llh = 0;
    double trs = 0;
    double right = 0;
    double correct_heads = 0;
    double total_heads = 0;
    auto t_start = std::chrono::high_resolution_clock::now();
    unsigned corpus_size = corpus.nsentencesDev;
    for (unsigned sii = 0; sii < corpus_size; ++sii) {
      const vector<unsigned>& sentence=corpus.sentencesDev[sii];
      const vector<unsigned>& sentencePos=corpus.sentencesPosDev[sii]; 
      const vector<string>& sentenceUnkStr=corpus.sentencesStrDev[sii]; 
      const vector<unsigned>& actions=corpus.correct_act_sentDev[sii];
      vector<unsigned> tsentence=sentence;
      if (!USE_SPELLING) {
        for (auto& w : tsentence)
	  if (training_vocab.count(w) == 0) w = kUNK;
      }
      ComputationGraph cg;
      double lp = 0;
      vector<unsigned> pred;
      if (beam_size == 1) {
	map<int, string> rel_ref3;
        map<int,int> ref3 = parser.compute_heads(sentence.size(), actions, corpus.actions, &rel_ref3); //Dynamic oracles. New thing
        pred = parser.log_prob_parser(&cg,sentence,tsentence,sentencePos,vector<unsigned>(),corpus.actions,corpus.intToWords,&right,ref3, rel_ref3);
      }
      else
        pred = parser.log_prob_parser_beam(&cg,sentence,tsentence,sentencePos,corpus.actions,beam_size,&lp);
      llh -= lp;
      trs += actions.size();
      map<int, string> rel_ref, rel_hyp;
      map<int,int> ref = parser.compute_heads(sentence.size(), actions, corpus.actions, &rel_ref);
      map<int,int> hyp = parser.compute_heads_hybrid(sentence.size(), pred, corpus.actions, &rel_hyp); // YG -- TODO: hybrid or standard
      output_conll(sentence, sentencePos, sentenceUnkStr, corpus.intToWords, corpus.intToPos, hyp, rel_hyp);
      correct_heads += compute_correct(ref, hyp, sentence.size() - 1);
      total_heads += sentence.size() - 1;
    }
    auto t_end = std::chrono::high_resolution_clock::now();
    cerr << "TEST llh=" << llh << " ppl: " << exp(llh / trs) << " err: " << (trs - right) / trs << " uas: " << (correct_heads / total_heads) << "\t[" << corpus_size << " sents in " << std::chrono::duration<double, std::milli>(t_end-t_start).count() << " ms]" << endl;
  }
  for (unsigned i = 0; i < corpus.actions.size(); ++i) {
    //cerr << corpus.actions[i] << '\t' << parser.p_r->values[i].transpose() << endl;
    //cerr << corpus.actions[i] << '\t' << parser.p_p2a->values.col(i).transpose() << endl; 
  }
}
