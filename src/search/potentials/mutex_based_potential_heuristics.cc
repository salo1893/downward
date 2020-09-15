//
// Created by salome on 02.08.20.
//
#include "potential_function.h"
#include "potential_heuristic.h"
#include "potential_max_heuristic.h"
#include "potential_optimizer.h"
#include "util.h"

#include "../heuristics/hm_heuristic.h"

#include "../option_parser.h"
#include "../plugin.h"
#include "../global_state.h"
#include "../utils/rng.h"
#include "../utils/rng_options.h"

#include <memory>
#include <vector>
#include <set>
#include <map>

//For the table:
#include <iostream>
#include <fstream>
#include <zconf.h>

using namespace std;
using namespace hm_heuristic;
using Tuple = vector<FactPair>;
using Weight = tuple<int, int, long double>; // Variable id, value and corresponding weight

namespace potentials {

    /**
     * @param state the state
     * @param variable_id the id of the variable
     * @return whether this variables is assigned
     */
    static bool unassigned(map<int, int> &state, int variable_id) {
        return state.find(variable_id) == state.end();
    }

    /**
     * Used in fact_disambiguation's, to subtract all non-possible values (mutexes) from the domain of a variable
     * @param domain domain of the variable
     * @param mutex facts which are mutex with the current state
     * @param variable_id id of the variable
     * @return the remaining domain of the variable
     */
    vector<int> set_minus(const vector<int> &domain, const Tuple &mutex, int variable_id) {
        set<int> dom(domain.begin(), domain.end()); // Sets are unique
        for (FactPair m : mutex) {                  // For each fact in mutex...
            if (m.var == variable_id) {             // which affects this variable...
                dom.erase(m.value);                 // remove its value from the domain.
            }
        }
        return vector<int>(dom.begin(), dom.end());
    }

    /**
     * Used in multi_fact_disambiguation to get elements which are in both mutex sets.
     * @param one mutex-set one
     * @param two mutex-set two
     * @return three facts i both one and two
     */
    static Tuple intersection(const Tuple &one, const Tuple &two) {
        Tuple three;
        for (FactPair fact : one) {
            for (FactPair f : two) {
                if (f == fact) {
                    three.push_back(fact);
                    break;
                }
            }
        }
        return three;
    }

    /**
     * save all FactPairs which are mutex to the fact <variable, value>
     * @param variable id of the fact
     * @param value of the fact
     * @param mutexes set of mutexes
     * @param mf tuple in which to save the mutexes
     */
    static void get_mutex_with_fact(int variable, int value, vector<Tuple> &mutexes, Tuple &mf) {
        for (Tuple mutex : mutexes) {
            if (mutex[0].var == variable && mutex[0].value == value) {
                mf.push_back(mutex[1]);
            } else if (mutex[1].var == variable && mutex[1].value == value) {
                mf.push_back(mutex[0]);
            }
        }
    }

    /**
      * Save all facts which are unreachable for this (partial) state
      * @param variable id of the fact
      * @param value of the fact
      * @param mutexes set of mutexes
      * @return mf tuple in which to save the mutexes
      */
    static Tuple get_mutex_with_state(map<int, int> &state, vector<Tuple> &mutexes) {
        Tuple mf;
        for (Tuple mutex : mutexes) {
            if (!unassigned(state, mutex[0].var) && state.find(mutex[0].var)->second == mutex[0].value) {
                mf.push_back(mutex[1]);
            } else if (!unassigned(state, mutex[1].var) && state.find(mutex[1].var)->second == mutex[1].value) {
                mf.push_back(mutex[0]);
            }
        }
        return mf;
    }

    /**
      * Extend the partial state to all possible states with k more assigned values
      * @param state vector containing the facts of a partial state
      * @param k amount of remaining variables to assign
      * @param assigned vector of variable ids which were assigned from the beginning
      * @param domains domains of all variables
      * @param extended the states which have been extended so far
      * @return a vector of all extended vectors
      */
    static vector<map<int, int>>
    get_all_extensions(map<int, int> &state, int k, int last_assigned, vector<int> &assigned,
                       vector<vector<int>> &domains, vector<map<int, int>> &extended) {
        if (k == 0) {
            extended.push_back(state);  // Add the state to the list, if it is fully extended.
            return extended;
        }

        // variables assigned from beginning to end, therefore enough unassigned variables need to remain free
        int before = assigned.size();
        while (before > 0 && last_assigned < assigned[before - 1]) {
            before--;
        }
        before = (int) assigned.size() - before;
        for (size_t i = last_assigned; i < domains.size() - (k - 1) -
                                           before; i++) { //domains.size(), as there are exactly as many domains as variables in the state
            if (unassigned(state, i)) { // if the variable is unassigned
                for (int d : domains[i]) {
                    state[i] = d;               // assign it to all domains and get all extensions
                    get_all_extensions(state, k - 1, (int) i + 1, assigned, domains, extended);
                }
                state.erase(i);                 // unassign the variable
            }
        }
        return extended;
    }

    /**
      * Extend the partial state to all possible states with k more assigned values
      * @param state vector containing the facts of a partial state
      * @param task
      * @param k amount of remaining variables to assign
      * @param domains domains of all variables
      * @return a vector of all extended states as vectors of facts
      */
    static vector<map<int, int>>
    get_all_extensions(map<int, int> &state, int k, vector<vector<int>> &domains) {
        vector<map<int, int>> states;
        vector<int> variables;
        variables.reserve(state.size());
        for (auto s : state) {
            variables.push_back(s.first);
        }
        return get_all_extensions(state, k, 0, variables, domains, states);
    }

    /**
     * Smaller all the domains of all variables by pruning unreachable facts
     * @param state the state of interest as vector of facts (pairs<int, int>)
     * @param variables all variables of this task
     * @param mutexes the set of mutexes for this task
     * @return the smalled domains
     */
    static vector<vector<int>>
    multi_fact_disambiguation(map<int, int> &state, VariablesProxy &variables, vector<Tuple> &mutexes) {
        // get all domains and id's of the variables (D_V<- F_V for every V in Variables)
        vector<vector<int>> domains;
        for (VariableProxy v : variables) {
            vector<int> dom;
            // For already assigned values add only this fact, for the rest the whole domain
            if (!unassigned(state, v.get_id())) {
                dom.push_back(state[v.get_id()]);
            } else {
                for (int d = 0; d < v.get_domain_size(); d++) {
                    dom.push_back(d);
                }
            }
            domains.push_back(dom);
        }

        // A <- M_p
        Tuple a = get_mutex_with_state(state, mutexes);

        //while state changes check for single fact disambiguations and assign them
        bool changed = true;
        while (changed) {
            changed = false;
            for (VariableProxy v : variables) {
                int id = v.get_id();
                if (unassigned(state, id)) {    // Variable is not assigned
                    auto size = domains[id].size();
                    //algorithm 2 l. 7 (D_V <- D_V \ a)
                    domains[id] = set_minus(domains[id], a, id);

                    //algorithm 2 l. 8 (A <- A U [intersection over all facts in D_V of M_(p U {f})] )
                    if (domains[id].size() < size) {
                        vector<FactPair> mf, mf2;
                        get_mutex_with_fact(id, domains[id][0], mutexes, mf);
                        for (size_t f = 1; f < domains[id].size(); f++) {
                            get_mutex_with_fact(id, domains[id][f], mutexes, mf2);
                            mf = intersection(mf, mf2); // Get all facts which are not part of any reachable state
                            if (mf.empty()) break;
                            mf2.clear();
                        }
                        if (!mf.empty()) {
                            a.insert(a.begin(), mf.begin(), mf.end());  // add states to a
                        }
                        changed = true;
                    }
                }
            }
        }
        return domains;
    }

    /**
     * Calculate C^k_f(M) for one state, as it handles a list of states it can also be used for K_f
     * it is the sum [ of the products [ of the number of reachable facts ] over all variables ] over all (k-extended) states
     * @param states the extensions of the state of interest (P^{f}_k resp. P^(tU{f})_(|t|+k))
     * @param variables the variables of this task
     * @param mutexes the set of mutexes for this task
     * @return the sum over the product of all reachable facts of each state
     */
    static long double c_k_f(vector<map<int, int>> &states, VariablesProxy &variables, vector<Tuple>
    &mutexes) {
        long double sum = 0;
        long double mult;
        for (map<int, int> &e : states) {
            mult = 1;
            // This probably does some redundant work, would it be useful to give more narrower domains?
            vector<vector<int>> domains = multi_fact_disambiguation(e, variables,
                                                                    mutexes);
            for (const vector<int> &d : domains) {
                mult *= d.size();
                if (mult == 0) break;
            }
            sum += mult;
        }
        return sum;
    }

    /**
      * Calculates the weights of all facts for all states (Eq. 12)
      * @param k amount of variables to extend
      * @param variables all variables
      * @param mutexes all mutex facts
      * @param task the task
      * @return a vector containing for each state (outer vector) the weights of each fact (inner vectors)
      */
    static vector<Weight>
    opt_k_m(int k, map<int, int> &assigned_variables, VariablesProxy &variables, vector<Tuple> &mutexes) {
        vector<Weight> facts;               // vector containing facts and their corresponding weight.
        vector<Weight> weights_f;           // to temporarily store the c_k_f values of one variable
        vector<map<int, int >> states;      // to temporarily store the extended states of a fact
        vector<vector<int>> domains;        // temporarily contain the multi_fact_disambiguated domain with one fact
        long double w, sum;

        for (size_t i = 0; i < variables.size(); i++) {         // for all all unassigned variables V and each fact_V
            if (unassigned(assigned_variables, (int) i)) {
                sum = 0;
                weights_f.clear();
                for (int d = 0; d < variables[i].get_domain_size(); d++) {
                    assigned_variables[i] = d;                          // add this fact
                    domains = multi_fact_disambiguation(assigned_variables, variables,
                                                        mutexes);    // get all non-mutex domains
                    states = get_all_extensions(assigned_variables, k - 1,
                                                domains);            // get all extended states for this fact
                    // (k - 1, as one additional variables is already assigned)
                    w = c_k_f(states, variables,
                              mutexes);        // Get the non-normalized weight of this fact ...
                    states.clear();
                    weights_f.emplace_back(i, d, w);                    // and add it to the list.
                    sum += w;
                }

                // normalize all weights
                for (int d = 0; d < variables[i].get_domain_size(); d++) {
                    get<2>(weights_f[d]) = get<2>(weights_f[d]) / sum;
                    facts.push_back(weights_f[d]);
                }

                // remove state[i]
                assigned_variables.erase(assigned_variables.find(i));
            }
        }
        return facts;
    }

    /**
      * Calculates the weights of all facts for all states (Eq. 12)
      * @param k amount of variables to extend
      * @param variables all variables
      * @param mutexes all mutex facts
      * @param task the task
      * @return a vector containing for each state (outer vector) the weights of each fact (inner vectors)
      */
    static vector<Weight> opt_k_m(int k, VariablesProxy &variables, vector<Tuple> &mutexes) {
        map<int, int> assigned_variables;   // to temporarily store one 'state'
        return opt_k_m(k, assigned_variables, variables, mutexes);
    }

    /**
     * Calculate n optimization functions, using opt_t_k_m (eq. 14)
     * @param t size of partial state (t uniformly randomly chosen facts)
     * @param k
     * @param n
     * @param variables
     * @param mutexes
     * @param optimizer
     * @return
     */
    static vector<unique_ptr<PotentialFunction>>
    opt_t_k_m(int t, int k, int n, VariablesProxy &variables, vector<Tuple> &mutexes, PotentialOptimizer &optimizer) {
        random_device rd;
        mt19937 gen(rd()); // apparently this is necessary for uniform int distribution...
        uniform_int_distribution<> dist_v(0, variables.size() - 1);

        // generate n optimization functions for one randomly sampled state
        vector<unique_ptr<PotentialFunction>> functions;
        vector<Weight> weights;
        map<int, int> state;
        for (int i = 0; i < n; i++) {
            state.clear();
            for (int t_i = 0; t_i < t; t_i++) {
                int v = dist_v(gen);
                if (unassigned(state, v)) {
                    uniform_int_distribution<> dist_f(0, variables[v].get_domain_size() - 1); // mit oder ohne -1?
                    state[v] = dist_f(gen);
                } else t_i--;
            }
            weights = opt_k_m(k - t, state, variables, mutexes);
            optimizer.optimize_for_weighted_samples(weights);
            functions.push_back(optimizer.get_potential_function());
        }
        return functions;
    }

    static unique_ptr<PotentialFunction> create_mutex_based_potential_function(
            const Options &opts) {

        PotentialOptimizer optimizer(opts);
        const AbstractTask &task = *opts.get<shared_ptr<AbstractTask>>("transform");
        TaskProxy task_proxy(task);
        VariablesProxy variables = task_proxy.get_variables();

        State initial = task_proxy.get_initial_state();

        //get all mutexes
        //This creates a file containing the mutex table. If one already exists, it will be used instead.
        std::vector<Tuple> mutexes;
        string filename = "mutex_table.txt";
        ifstream table;
        table.open(filename);
        if (!table) {
            cout << "Perform hm-heuristic and store mutexes to " << filename << endl;
            // TODO: Keep this three lines, remove rest
            auto hm = make_shared<HMHeuristic>(opts);
            mutexes = hm->get_unreachable_tuples(initial);
            hm.reset();
            //write to file
            ofstream create(filename);
            if (create.is_open()) {
                for (Tuple mutex : mutexes) {
                    create << mutex[0].var << " " << mutex[0].value << " ";
                    create << mutex[1].var << " " << mutex[1].value << "\n";
                }
                create.close();
            } else {
                cout << "Unable to store mutex table";
            }
        } else {
            //read from file
            mutexes.clear();
            string line;
            string number;
            int var, value;
            while (getline(table, line)) {
                istringstream ss(line);
                ss >> number;
                var = stoi(number);
                ss >> number;
                value = stoi(number);
                FactPair fact1(var, value);
                ss >> number;
                var = stoi(number);
                ss >> number;
                value = stoi(number);
                FactPair fact2(var, value);

                mutexes.push_back({fact1, fact2});
            }
            table.close();
        }

        int k = opts.get<int>("k");
        assert(k < (int) variables.size()); // k my not be bigger than the size of one state TODO richtig so?
        vector<Weight> weights = opt_k_m(k, variables, mutexes);
        optimizer.optimize_for_weighted_samples(weights);
        return optimizer.get_potential_function();
    }

    static vector<unique_ptr<PotentialFunction>> create_mutex_based_ensemble_potential_function(
            const Options &opts) {

        PotentialOptimizer optimizer(opts);
        const AbstractTask &task = *opts.get<shared_ptr<AbstractTask>>("transform");
        TaskProxy task_proxy(task);
        VariablesProxy variables = task_proxy.get_variables();

        State initial = task_proxy.get_initial_state();

        //get all mutexes
        //This creates a file containing the mutex table. If one already exists, it will be used instead.
        std::vector<Tuple> mutexes;
        string filename = "mutex_table.txt";
        ifstream table;
        table.open(filename);
        if (!table) {
            cout << "Perform hm-heuristic and store mutexes to " << filename << endl;
            // TODO: Keep this three lines, remove rest
            auto hm = make_shared<HMHeuristic>(opts);
            mutexes = hm->get_unreachable_tuples(initial);
            hm.reset();
            //write to file
            ofstream create(filename);
            if (create.is_open()) {
                for (Tuple mutex : mutexes) {
                    create << mutex[0].var << " " << mutex[0].value << " ";
                    create << mutex[1].var << " " << mutex[1].value << "\n";
                }
                create.close();
            } else {
                cout << "Unable to store mutex table";
            }
        } else {
            //read from file
            mutexes.clear();
            string line;
            string number;
            int var, value;
            while (getline(table, line)) {
                istringstream ss(line);
                ss >> number;
                var = stoi(number);
                ss >> number;
                value = stoi(number);
                FactPair fact1(var, value);
                ss >> number;
                var = stoi(number);
                ss >> number;
                value = stoi(number);
                FactPair fact2(var, value);

                mutexes.push_back({fact1, fact2});
            }
            table.close();
        }

        int k = opts.get<int>("k");
        assert(k < (int) variables.size()); // k my not be bigger than the size of one state TODO richtig so?
        int t = opts.get<int>("t");
        assert(t <= k);                     // t my not be bigger than k TODO richtig so?
        int n = opts.get<int>("n");

        vector<unique_ptr<PotentialFunction>> pot = opt_t_k_m(t, k, n, variables, mutexes, optimizer);
        return pot;
    }

    static shared_ptr<Heuristic> _parse_single(OptionParser &parser) {
        parser.document_synopsis(
                "Mutex-based potential heuristics",
                get_admissible_potentials_reference());
        parser.add_option<int>(
                "m",
                "use h2 heuristic",
                "2",
                Bounds("0", "infinity"));
        parser.add_option<int>(
                "k",
                "size of extended state",
                "1",
                Bounds("0", "infinity"));
        prepare_parser_for_admissible_potentials(parser);
        Options opts = parser.parse();
        if (parser.dry_run())
            return nullptr;

        return make_shared<PotentialHeuristic>(
                opts, create_mutex_based_potential_function(opts));
    }

    static shared_ptr<Heuristic> _parse_ensemble(OptionParser &parser) {
        parser.document_synopsis(
                "Mutex-based ensemble potential heuristics",
                get_admissible_potentials_reference());
        parser.add_option<int>(
                "m",
                "use h2 heuristic",
                "2",
                Bounds("0", "infinity"));
        parser.add_option<int>(
                "k",
                "size of extended state",
                "2",
                Bounds("0", "infinity"));
        parser.add_option<int>(
                "t",
                "amount of uniformly randomly chosen facts (t <= k)",
                "1",
                Bounds("0", "infinity"));
        parser.add_option<int>(
                "n",
                "Number of states to sample",
                "50",
                Bounds("0", "infinity"));
        prepare_parser_for_admissible_potentials(parser);
        Options opts = parser.parse();
        if (parser.dry_run())
            return nullptr;

        return make_shared<PotentialMaxHeuristic>(
                opts, create_mutex_based_ensemble_potential_function(opts));
    }

    static Plugin<Evaluator> _plugin_single(
            "mutex_based_potential", _parse_single, "heuristics_potentials");

    static Plugin<Evaluator> _plugin_ensemble(
            "mutex_based_ensemble_potential", _parse_ensemble, "heuristics_potentials");
}