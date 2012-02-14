/*******************************************************************************
 * Copyright (c) 2012, Dougal J. Sutherland (dsutherl@cs.cmu.edu).             *
 * All rights reserved.                                                        *
 *                                                                             *
 * Redistribution and use in source and binary forms, with or without          *
 * modification, are permitted provided that the following conditions are met: *
 *                                                                             *
 *     * Redistributions of source code must retain the above copyright        *
 *       notice, this list of conditions and the following disclaimer.         *
 *                                                                             *
 *     * Redistributions in binary form must reproduce the above copyright     *
 *       notice, this list of conditions and the following disclaimer in the   *
 *       documentation and/or other materials provided with the distribution.  *
 *                                                                             *
 *     * Neither the name of Carnegie Mellon University nor the                *
 *       names of the contributors may be used to endorse or promote products  *
 *       derived from this software without specific prior written permission. *
 *                                                                             *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" *
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE   *
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE  *
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE   *
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR         *
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF        *
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS    *
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN     *
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)     *
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE  *
 * POSSIBILITY OF SUCH DAMAGE.                                                 *
 ******************************************************************************/
#ifndef SDM_HPP_
#define SDM_HPP_
#include "sdm/basics.hpp"
#include "sdm/kernels/kernel.hpp"
#include "sdm/kernel_projection.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

#include <np-divs/matrix_arrays.hpp>
#include <np-divs/np_divs.hpp>
#include <flann/util/matrix.h>
#include <svm.h>

namespace sdm {

// TODO memory ownership with this is all screwy...make it clearer
template <typename Scalar>
class SDM {
    const svm_model &svm;
    const svm_problem &svm_prob; // needs to live at least as long as the model
    const npdivs::DivFunc *div_func;
    const Kernel *kernel;
    const npdivs::DivParams div_params;
    const size_t num_classes;

    const flann::Matrix<Scalar> *train_bags;
    const size_t num_train;

    public:
        SDM(const svm_model &svm, const svm_problem &svm_prob,
            const npdivs::DivFunc &div_func, const Kernel &kernel,
            const npdivs::DivParams &div_params,
            size_t num_classes,
            const flann::Matrix<Scalar> *train_bags, size_t num_train)
        :
            svm(svm), svm_prob(svm_prob),
            kernel(new_clone(kernel)), div_func(new_clone(div_func)),
            div_params(div_params), num_classes(num_classes),
            train_bags(train_bags), num_train(num_train)
        { }

        ~SDM() {
            delete div_func;
            delete kernel;
        }

        void destroyModelAndProb();

        int predict(const flann::Matrix<Scalar> &test_bag) const;
        int predict(const flann::Matrix<Scalar> &test_bag,
                std::vector<double> &vals) const;

        std::vector<int> predict(
                const flann::Matrix<Scalar> *test_bags, size_t num_test)
            const;
        std::vector<int> predict(
                const flann::Matrix<Scalar> *test_bags, size_t num_test,
                std::vector< std::vector<double> > &vals)
            const;
};

// set up default values for training

namespace detail {
    const double cvals[11] = { // 2^-9, 2^-6, ..., 2^21
        1./512., 1./64., 1./8., 1, 1<<3, 1<<6, 1<<9, 1<<12, 1<<15, 1<<18, 1<<21
    };
}


const svm_parameter default_svm_params = {
    C_SVC, // svm_type
    PRECOMPUTED, // kernel_type
    0,    // degree - not used
    0,    // gamma - not used
    0,    // coef0 - not used
    100,  // cache_size, in MB
    1e-3, // eps
    1,    // C
    0,    // nr_weight
    NULL, // weight_label
    NULL, // weight
    0,    // nu - not used
    0,    // p - not used
    1,    // shrinking
    1     // probability
};
const std::vector<double> default_c_vals(detail::cvals, detail::cvals + 11);

// Function to train a new SDM. Note that the caller is responsible for deleting
// the svm and svm_prob attributes.
//
// TODO: a mass-training method for more than one kernel
// TODO: option to do the projection on test data as well
// TODO: more flexible tuning CV options...tune on a subset of the data?
template <typename Scalar>
SDM<Scalar> * train_sdm(
    const flann::Matrix<Scalar> *train_bags, size_t num_train,
    const std::vector<int> &labels,
    const npdivs::DivFunc &div_func,
    const KernelGroup &kernel_group,
    const npdivs::DivParams &div_params,
    const std::vector<double> &c_vals = default_c_vals,
    const svm_parameter &svm_params = default_svm_params,
    size_t tuning_folds = 3);

////////////////////////////////////////////////////////////////////////////////
// Helper functions

namespace detail {
    void store_kernel_matrix(svm_problem &prob, double *divs, bool alloc) {
        size_t n = prob.l;
        if (alloc) prob.x = new svm_node*[n];

        for (size_t i = 0; i < n; i++) {
            if (alloc) prob.x[i] = new svm_node[n+2];
            prob.x[i][0].value = i+1;
            for (size_t j = 0; j < n; j++) {
                prob.x[i][j+1].index = j+1;
                prob.x[i][j+1].value = divs[i*n + j];
            }
            prob.x[i][n+1].index = -1;
        }
    }

    void print_null(const char *s) {}

    // see whether a kernel is just so horrendous we shouldn't bother
    bool terrible_kernel(double* km, size_t n, double const_thresh=1e-4) {
        const double l = n*n;

        // is it all a constant?
        bool is_const = true;
        const double v = km[0];
        const_thresh = std::max(const_thresh, v*const_thresh);
        for (size_t i = 1; i < l; i++) {
            if (std::abs(km[i] - v) > const_thresh) {
                is_const = false;
                break;
            }
        }
        if (is_const) { // TODO: real logging
            fprintf(stderr, "Skipping tuning over constant kernel matrix\n");
            return true;
        }

        // TODO: other tests?

        return false;
    }

    template <typename T>
    T pick_rand(std::vector<T> vec) {
        size_t n = vec.size();
        if (n == 0) {
            throw std::domain_error("picking from empty vector");
        } else if (n == 1) {
            return vec[0];
        } else {
            // use c++'s silly random number generator; good enough for this
            std::srand(std::time(NULL));
            return vec[std::rand() % n];
        }
    }
}

template <typename Scalar>
void SDM<Scalar>::destroyModelAndProb() {
    // FIXME: rework SDM memory model to avoid gross const_casts

    svm_free_model_content(const_cast<svm_model*> (&svm));
    delete const_cast<svm_model*>(&svm);

    for (size_t i = 0; i < svm_prob.l; i++)
        delete[] svm_prob.x[i];
    delete[] svm_prob.x;
    delete[] svm_prob.y;
    delete const_cast<svm_problem*>(&svm_prob);
}

////////////////////////////////////////////////////////////////////////////////
// Training

template <typename Scalar>
SDM<Scalar> * train_sdm(
        const flann::Matrix<Scalar> *train_bags, size_t num_train,
        const std::vector<int> &labels,
        const npdivs::DivFunc &div_func,
        const KernelGroup &kernel_group,
        const npdivs::DivParams &div_params,
        const std::vector<double> &c_vals,
        const svm_parameter &svm_params,
        size_t tuning_folds)
{   // TODO - logging

    if (c_vals.size() == 0) {
        throw std::domain_error("c_vals is empty");
    } else if (labels.size() != num_train) {
        throw std::domain_error("labels.size() disagrees with num_train");
    }

    // copy the svm params so we can change them
    svm_parameter svm_p = svm_params;
    svm_p.svm_type = C_SVC;
    svm_p.kernel_type = PRECOMPUTED;

    // make libSVM shut up  -  TODO real logging
    svm_set_print_string_function(&detail::print_null);

    // first compute divergences
    flann::Matrix<double>* divs =
        npdivs::alloc_matrix_array<double>(1, num_train, num_train);
    np_divs(train_bags, num_train, div_func, divs, div_params, false);

    // set up the basic svm_problem
    svm_problem *prob = new svm_problem;
    prob->l = num_train;
    prob->y = new double[num_train];
    for (size_t i = 0; i < num_train; i++)
        prob->y[i] = labels[i];

    // store un-transformed divs in the problem just so it's all allocated
    detail::store_kernel_matrix(*prob, divs[0].ptr(), true);

    ////////////////////////////////////////////////////////////////////////////
    // tuning: cross-validate over possible svm/kernel parameters
    // TODO: optionally parallelize tuning

    // ask the kernel group for the kernels we'll pick from for tuning
    const boost::ptr_vector<Kernel>* kernels =
        kernel_group.getTuningVector(divs->ptr(), num_train);
    size_t num_kernels = kernels->size();

    // want to keep track of the best kernel/C combos...keep all of them, to
    // avoid biasing towards ones we see earlier when accuracies are equal.
    typedef std::pair<size_t, size_t> config; // <kernel, C> indices
    std::vector<config> best_configs;
    size_t best_correct = 0;

    if (num_kernels == 0) {
        throw std::domain_error("no kernels in the kernel group");
    } else if (num_kernels == 1 && c_vals.size() == 1) {
        best_configs.push_back(config(0, 0));
    } else {
        // make a copy of divergences so we can mangle it
        double *km = new double[num_train*num_train];

        // used to store labels into during CV
        double cv_labels[num_train];

        for (size_t k = 0; k < num_kernels; k++) {
            // turn into a kernel matrix
            std::copy(divs[0].ptr(), divs[0].ptr() + num_train*num_train, km);
            (*kernels)[k].transformDivergences(km, num_train);
            project_to_symmetric_psd(km, num_train);

            // is it a constant matrix or something else awful?
            if (num_kernels != 1 && detail::terrible_kernel(km, num_train))
                continue;

            // store in the svm_problem
            detail::store_kernel_matrix(*prob, km, false);

            for (size_t ci = 0; ci < c_vals.size(); ci++) {
                // do SVM cross-validation with these params
                svm_p.C = c_vals[ci];
                svm_cross_validation(prob, &svm_p, tuning_folds, cv_labels);

                size_t num_correct = 0;
                for (size_t i = 0; i < num_train; i++)
                    if (cv_labels[i] == labels[i])
                        num_correct++;

                if (num_correct >= best_correct) {
                    if (num_correct > best_correct) {
                        best_configs.clear();
                        best_correct = num_correct;
                    }
                    best_configs.push_back(std::make_pair(k, ci));
                }
            }
        }

        delete[] km;
    }

    // choose a kernel / C combo as the best one
    const config &best_config = detail::pick_rand(best_configs);
    const Kernel *kernel = new_clone((*kernels)[best_config.first]);
    svm_p.C = c_vals[best_config.second];

    delete kernels; // FIXME: potential leaks in here if something crashes

    ////////////////////////////////////////////////////////////////////////////
    // train final SVM on the whole thing

    kernel->transformDivergences(divs[0].ptr(), num_train);
    project_to_symmetric_psd(divs[0].ptr(), num_train);
    detail::store_kernel_matrix(*prob, divs[0].ptr(), false);
    npdivs::free_matrix_array(divs, 1); // don't need these anymore

    const char* error = svm_check_parameter(prob, &svm_p);
    if (error != NULL) {
        std::cerr << "LibSVM parameter error: " << error << std::endl;
        throw std::domain_error(error);
    }
    svm_model *svm = svm_train(prob, &svm_p);
    SDM<Scalar>* sdm = new SDM<Scalar>(*svm, *prob, div_func, *kernel,
            div_params, svm_get_nr_class(svm), train_bags, num_train);
    delete kernel;
    return sdm;
}

////////////////////////////////////////////////////////////////////////////////
// Prediction

template <typename Scalar>
int SDM<Scalar>::predict(const flann::Matrix<Scalar> &test_bag) const {
    std::vector< std::vector<double> > vals(1);
    return this->predict(&test_bag, 1, vals)[0];
}
template <typename Scalar>
int SDM<Scalar>::predict(const flann::Matrix<Scalar> &test_bag,
        std::vector<double> &val)
const {
    std::vector< std::vector<double> > vals(1);
    const std::vector<int> &pred_labels = this->predict(&test_bag, 1, vals);
    val = vals[0];
    return pred_labels[0];
}

template <typename Scalar>
std::vector<int> SDM<Scalar>::predict(
        const flann::Matrix<Scalar> *test_bags, size_t num_test)
const {
    std::vector< std::vector<double> > vals(num_test);
    return this->predict(test_bags, num_test, vals);
}

template <typename Scalar>
std::vector<int> SDM<Scalar>::predict(
        const flann::Matrix<Scalar> *test_bags, size_t num_test,
        std::vector< std::vector<double> > &vals)
const {
    // TODO: np_divs option to compute things both ways and/or save trees
    // TODO: only compute divergences from support vectors
    double fwd_data[num_train * num_test];
    flann::Matrix<double> forward(fwd_data, num_train, num_test);

    double bwd_data[num_test * num_train];
    flann::Matrix<double> backward(bwd_data, num_test, num_train);

    // compute divergences
    npdivs::np_divs(train_bags, num_train, test_bags, num_test,
            *div_func, &forward, div_params);
    npdivs::np_divs(test_bags, num_test, train_bags, num_train,
            *div_func, &backward, div_params);

    // pass through the kernel
    kernel->transformDivergences(forward.ptr(), num_train, num_test);
    kernel->transformDivergences(backward.ptr(), num_test, num_train);

    // we can't project here, so we just symmetrize
    // TODO - symmetrize divergence estimates or kernel estimates?
    for (size_t i = 0; i < num_test; i++)
        for (size_t j = 0; j < num_train; j++)
            backward[i][j] = (forward[j][i] + backward[i][j]) / 2.0;

    // figure out which prediction function we want to use
    double (*pred_fn)(const svm_model*, const svm_node*, double*) =
        svm_check_probability_model(&svm)
        ? &svm_predict_probability : &svm_predict_values;

    // we'll reuse this svm_node array for testing
    svm_node kernel_row[num_train+2];
    for (size_t i = 0; i <= num_train; i++)
        kernel_row[i].index = i;
    kernel_row[num_train+1].index = -1;

    // even though those fns return doubles, we'll round to an int because
    // we want integer class labels
    std::vector<int> pred_labels(num_test);

    // predict!
    vals.resize(num_test);
    for (size_t i = 0; i < num_test; i++) {
        // fill in our kernel evaluations
        kernel_row[0].value = -i - 1;
        for (size_t j = 0; j < num_train; j++)
            kernel_row[j+1].value = backward[i][j];

        // get space to store our decision/probability values
        vals[i].resize(num_classes);

        // ask the SVM for a prediction
        double res = pred_fn(&svm, kernel_row, &vals[i][0]);
        pred_labels[i] = (int) std::floor(res + .5);
    }

    return pred_labels;
}



} // end namespace

#endif
