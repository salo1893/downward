//
// Created by salome on 02.08.20.
//
#include "potential_function.h"
#include "potential_heuristic.h"
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

//For the table:
#include <iostream>
#include <fstream>

using namespace std;
using namespace hm_heuristic;
using Tuple = vector<FactPair>;
using Sates_Weights = tuple<vector<State>, vector<vector<float>>>;

namespace potentials {

    /**
     * Used in fact_disambiguation's, to substract all non-possible values (mutexes) from the domain of a variabel
     * @param domain domain of the variable
     * @param mutex facts which are mutex with the current state
     * @param variable_id id of the variable
     * @return the remaining domain of the variable
     */
    vector<int> set_minus(const vector<int>& domain, const Tuple& mutex, int variable_id) {
        vector<int> set_minus;
        for (FactPair m : mutex) {
            if (m.var == variable_id) {
                for (int d : domain) {
                    if (m.value != d) {
                        set_minus.push_back(d);
                    }
                }
            }
        }
        return set_minus;
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
      * Save all mutex pairs which are unreachable for this (partial) state
      * @param variable id of the fact
      * @param value of the fact
      * @param mutexes set of mutexes
      * @return mf tuple in which to save the mutexes
      */
    static Tuple get_mutex_with_state(State &state, vector<Tuple> &mutexes) {
        Tuple mf;
        for (Tuple mutex : mutexes) {
            if (state[mutex[0].var].get_value() == mutex[0].value) {
                mf.push_back(mutex[1]);
            } else if (state[mutex[1].var].get_value() == mutex[1].value) {
                mf.push_back(mutex[0]);
            }
        }
        return mf;
    }

    /**
     * Used in multi_fact_disambiguation to get elements which are in both mutex sets.
     * @param one mutexset ones
     * @param two mutexset two
     * @return three facts i both one and two
     */
    static Tuple intersection(const Tuple& one, const Tuple& two) {
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
      * Extend the partial state to all possible states with k more assigned values
      * @param state vector containing the facts of a partial state
      * @param task
      * @param k amount of remaining variables to assign
      * @param f_v position of the first assigned variable
      * @param domains domains of all variables
      * @param extended the states which have been extended
      * @param last_assigned_variable the variable which was assigned last
      * @return a vector of all extended vectors
      */
    static vector<State>
    get_all_extensions(vector<int> state, TaskProxy &task, int k, int f_v, vector<vector<int>> &domains,
                       int last_assigned_variable,
                       vector<State> &extended) {
        if (k == 0) {
            vector<int>temp = state;
            extended.push_back(task.create_state(move(temp)));
            return extended;
        }
        // variables assigned from beginning to end, therefore enough unassigned variables need to remain
        // size - k if only not assigned values left, size - k - 1 otherwise
        int before = f_v < last_assigned_variable ? 0 : 1;
        for (size_t i = last_assigned_variable; i < state.size() - k - before; i++) {
            for (int d : domains[i]) {
                state[i] = d;
                //TODO: nur aufrufe, falls k > 0, sonst e anhängen? für bessere performance...
                for (const State& s : get_all_extensions(state, task, k - 1, f_v, domains, i + 1, extended)) {
                    extended.push_back(s);
                }
                state[i] = -1;
            }
        }
        return extended;
    }

    /**
      * Extend the partial state to all possible states with k more assigned values
      * @param state vector containing the facts of a partial state
      * @param task
      * @param k amount of remaining variables to assign
      * @param f_v position of the first assigned variable
      * @param domains domains of all variables
      * @return a vector of all extended vectors
      */
    static vector<State>
    get_all_extensions(vector<int> state, TaskProxy &task, int k, int f_v, vector<vector<int>> &domains) {
        vector<State> states;
        return get_all_extensions(state, task, k, f_v, domains, 0, states);
    }

    /**
     * TODO: The following probably have the first 11 lines in common... AND is single_fact needed? can it be used somewhere to simplify things, eg. et the beginning of mutli fact?
    static bool single_fact_diasambiguation(State &state, VariablesProxy &variables, vector<Tuple> &mutexes){
        //get all domains and id's of the variables
        vector<vector<int>> domains;
        vector<int> variable_id;
        for(VariableProxy v : variables) {
            vector<int> dom;
            for (int d = 0; d < v.get_domain_size(); d++){
                dom.push_back(d);
            }
            domains.push_back(dom);
            variable_id.push_back(v.get_id());
        }
        Tuple mp = get_mutex_with_state(state, mutexes);

        //while state changes check for single fact disambiguations and assign them
        bool changed = true;
        while (changed) {
            changed = false;
            for(size_t v = 0; v < state.size(); v++) {
                if (!state.is_defined(v)){
                    //algorithm 1 l. 4 (D_V <- F_V \ M_p)
                    domains[v] = set_minus(domains[v], mp, variable_id[v]);

                    //algorithm 1 l. 6
                    if (domains[v].size() == 0) {
                        return false;
                    }

                    //algorithm 1 l. 5
                    if (domains[v].size() == 1){
                        get_mutex_with_fact(variable_id[v], domains[v][0], mutexes, mp);
                        changed = true;
                    }
                }
            }
        }
        return true;
    }
    */

    /**
     * Smaller all the domains of all variables by pruning unreachable facts
     * @param state the state of interest
     * @param variables all variables of this task
     * @param mutexes the set of mutexes for this task
     * @return the smallered domains
     */
    static vector<vector<int>>
    multi_fact_diasambiguation(State state, VariablesProxy &variables, vector<Tuple> &mutexes) {
        //get all domains and id's of the variables
        vector<vector<int>> domains;
        vector<int> variable_id;
        for (VariableProxy v : variables) {
            vector<int> dom;
            // TODO: for assigned values, add all values or only the assigned one?
            if (!state.is_defined(v.get_id())) {
                for (int d = 0; d < v.get_domain_size(); d++) {
                    dom.push_back(d);
                }
            } else {
                dom.push_back(state[v.get_id()].get_value());
            }
            domains.push_back(dom);
            variable_id.push_back(v.get_id());
        }
        Tuple a = get_mutex_with_state(state, mutexes);

        //while state changes check for single fact disambiguations and assign them
        bool changed = true;
        while (changed) {
            changed = false;
            for (size_t v = 0; v < state.size(); v++) {
                if (!state.is_defined(v)) {
                    auto size = domains[v].size();
                    //algorithm 2 l. 7 (D_V <- D_V \ a)
                    domains[v] = set_minus(domains[v], a, variable_id[v]);

                    //algorithm 2 l. 8
                    if (domains[v].size() < size) {
                        vector<FactPair> mf, mf2;
                        get_mutex_with_fact(variable_id[v], domains[v][0], mutexes, mf);
                        for (size_t f = 1; f < domains[v].size(); f++) {
                            get_mutex_with_fact(variable_id[v], domains[v][f], mutexes, mf2);
                            mf = intersection(mf, mf2);
                            mf2.clear();
                            if (mf2.size() == 0 || mf.size() == 0) {
                                break;
                            }
                        }
                        if (mf.size() > 0) {
                            a.insert(a.begin(), mf.begin(), mf.end());
                        }
                        changed = true;
                    }
                }
            }
        }
        return domains;
    }

     /**
      * Calculate C^k_f(M) for one state, i.e. his extensions states
      * @param states the extensions of the state of interest (P^t_k)
      * @param variables the variables of this task
      * @param mutexes the set of mutexes for this task
      * @return the sum over the product of all reachable facts of each state
      */
    static int c_k_f(vector<State> states, VariablesProxy &variables, vector<Tuple> &mutexes) {
        int sum = 0;
        int mult;
        for (State e : states) {
            mult = 1;
            vector<vector<int>> domains = multi_fact_diasambiguation(e, variables,
                                                                     mutexes); // This probably does some redundant work, would it be useful to give more narrowed down domains?
            for (vector<int> d : domains) {
                mult *= d.size();
            }
            sum += mult;
        }
        return sum;
    }

    /**
      * Calculates the weights of all facts for all states (Eq. 12)
      * @param k size extended states should have
      * @param variables all variables
      * @param mutexes all mutex facts
      * @param task the task
      * @return a vector containing for each state (outer vector) the weights of each fact (inner vectors)
      */
    static Sates_Weights opt_k_m(int k, VariablesProxy &variables, vector<Tuple> &mutexes, TaskProxy task) {
        // variables to return
        vector<State> extended_states;
        vector<vector<float>> weights;

        //needed to calculate the weights
        vector<float> weights_f;
        float w = 0;
        float sum = 0;

        //needed to get all states
        vector<int> values (variables.size(), -1);
        vector<State> states;
        vector<vector<int>> domains;
        //TODO: generate all states and look for them, instead of generating all multiple times?

        for (size_t i = 0; i < variables.size(); i++) {
            sum = 0;
            weights_f.clear();
            for (int d = 0; d < variables[i].get_domain_size(); d++) {
                //get and add the new state and all its extensions
                values[i] = d;
                //TODO: move values removes the values!!! need a copy instead. This might be a problem at some other places as well...
                vector<int> temp = values;
                domains = multi_fact_diasambiguation(task.create_state(move(temp)), variables, mutexes);
                states = get_all_extensions(values, task, k, i, domains);
                for (State& s : states) {
                    extended_states.push_back(s);
                }

                //get and add the weight of this fact
                w = (float) c_k_f(states, variables, mutexes);
                weights_f.push_back(w);
                sum += w;

                states.clear();
            }
            values[i] = -1;
            for (int d = 0; d < variables[i].get_domain_size(); d++) {
                weights_f[d] /= sum;
            }
            weights.push_back(weights_f);
        }

        Sates_Weights weighted_States(extended_states, weights);
        return weighted_States;
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
        if(!table) {
            cout << "Perform hm-heuristic and store mutexes to " << filename << endl;
            auto hm = make_shared<HMHeuristic>(opts);
            mutexes = hm->get_unreachable_tuples(initial);
            hm.reset();
            //write to file
            ofstream create (filename);
            if (create.is_open()){
                for (Tuple mutex : mutexes) {
                    create << mutex[0].var << " " << mutex[0].value << " ";
                    create << mutex[1].var << " " << mutex[1].value << "\n";
                }
                create.close();
            }else {
                cout << "Unable to store mutex table";
            }
        } else {
            //read from file
            string line;
            string number;
            int var, value;
            while(getline(table, line)) {
                istringstream ss (line);
                ss >> number;
                var = stoi(number);
                ss >> number;
                value = stoi(number);
                FactPair fact1 (var, value);
                ss >> number;
                var = stoi(number);
                ss >> number;
                value = stoi(number);
                FactPair fact2 (var, value);
                
                mutexes.push_back({fact1, fact2});
            }
            table.close();
        }

        int k = 1; //TODO: get from options
        Sates_Weights weighted_states = opt_k_m(k, variables, mutexes, task_proxy);
        optimizer.optimize_for_weighted_samples(get<0>(weighted_states), get<1>(weighted_states));

        // optimize the optimizer for the initial state
        optimizer.optimize_for_state(initial);
        return optimizer.get_potential_function();
    }

    static shared_ptr<Heuristic> _parse(OptionParser &parser) {
        parser.document_synopsis(
                "Mutex-based potential heuristics",
                get_admissible_potentials_reference());
        parser.add_option<int>("m", "use h2 heuristic", "2", Bounds("0", "infinity"));
        prepare_parser_for_admissible_potentials(parser);
        Options opts = parser.parse();
        if (parser.dry_run())
            return nullptr;

        return make_shared<PotentialHeuristic>(
                opts, create_mutex_based_potential_function(opts));
    }

    static Plugin<Evaluator> _plugin(
            "mutex_based_potential", _parse, "heuristics_potentials");
}