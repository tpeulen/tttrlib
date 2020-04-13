/****************************************************************************
 * Copyright (C) 2020 by Thomas-Otavio Peulen                               *
 *                                                                          *
 * This file is part of the library tttrlib.                                *
 *                                                                          *
 *   tttrlib is free software: you can redistribute it and/or modify it     *
 *   under the terms of the MIT License.                                    *
 *                                                                          *
 *   tttrlib is distributed in the hope that it will be useful,             *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of         *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.                   *
 *                                                                          *
 ****************************************************************************/

#ifndef TTTRLIB_PDA_H
#define TTTRLIB_PDA_H

#include <math.h>
#include <string.h>
#include <vector>

/* "Histogram" and "Experimental" clusters */
#pragma pack(1)
typedef struct {
    double xmin;
    double xmax;
    int Nbins;
    char log_x;
    unsigned short equation;
    int Nmax;
    int Nmin;
} HistCluster;

#pragma pack(1)
typedef struct {
    double Bg;
    double Br;
    double crosstalk;
    double phiD;
    double phiA;
    double R0;
    double Gfactor;
    double l1;
    double l2;
    double mumlimol_p;
} ExpCluster;


// delta-T histogram cluster
#pragma pack(1)
typedef struct {
    int deltaN;
    double maxT;
    int Nbins;
    char log_x;
} DeltaTHistCluster;



class Pda {

protected:

    // This is set to true if the content of SgSr is valid for the inputs
    bool sgsr_valid = false;
    double Bg = 0.0;
    double Br = 0.0;
    int Nmax = 300;
    int NBins = 81;
    int Nmin = 3;
    bool log_x = true;
    double xmin = 0.01;
    double xmax = 100.0;

    std::vector<double> SgSr;
    std::vector<double> pF;
    std::vector<double> amplitudes;
    std::vector<double> probability_green_theor;

    void evaluate();

public:

    // Constructor and Destructor
    Pda() = default;
    ~Pda() = default;

    // Methods
    void append(double amplitude, double probability_green);
    void set_probability_green(double *input, int n_input);
    void clear_probability_green();
    void get_amplitudes(double **output, int *n_output);
    void get_probability_green(double **output, int *n_output);

    // Getter and Setter
    unsigned int get_max_number_of_photons() const;
    void set_max_number_of_photons(unsigned int nmax);
    double get_green_background() const;
    void set_green_background(double bg);
    double get_red_background() const;
    void set_red_background(double br);
    void setPF(double *input, int n_input);
    void get_SgSr_matrix(double **output, int *n_output1, int *n_output2);
    void set_n_bins(unsigned int n){
        NBins = n;
    }

    /*!
     * Histogram routine that
     * @tparam T
     * @param input the SgSr matrix that contains P(Sgreen, Sred)
     * @param Nmax max N photons
     * @param histogram_param "Histogram" cluster from Tatiana
     * @param exp_param "Experimental" cluster from Tatiana
     * @param histogram_x histogram X axis (return)
     * @param histogram_y histogram itself (return)
     */
    void sgsr_histogram(
            double **histogram_x, int *n_histogram_x,
            double **histogram_y, int *n_histogram_y
    );
};


namespace PdaFunctions {

    /*!
     *
     * calculating p(G,R) according to Matthew
     *
     * @param SgSr  SgSr(i,j) = p(Sg=i, Sr=j)
     * @param pN p(N)
     * @param Nmax max number of photons (max N)
     * @param Bg <background green>, per time window (!)
     * @param Br <background red>, -"-
     * @param pg_theor
     */
    void sgsr_pN(
            double *SgSr,
            double *pN,
            unsigned int Nmax,
            double Bg,
            double Br,
            double pg_theor
    );

    /*!
     * calculating p(G,R), one ratio, one P(F)
     *
     * @param SgSr sgsr_pN
     * @param pF input p(F)
     * @param Nmax
     * @param Bg
     * @param Br
     * @param pg_theor
     */
    void sgsr_pF(
            double *SgSr,
            double *pF,
            unsigned int Nmax,
            double Bg,
            double Br,
            double pg_theor
    );

    /*!
     *
     * calculating p(G,R), several ratios, same P(N)
     *
     * @param SgSr see sgsr_pN
     * @param pN input: p(N)
     * @param Nmax
     * @param Bg
     * @param Br
     * @param N_pg size of pg_theor
     * @param pg_theor
     * @param a corresponding amplitudes
     */
    void sgsr_pN_manypg(
            double *SgSr,
            double *pN,
            unsigned int Nmax,
            double Bg,
            double Br,
            unsigned int N_pg,
            double *pg_theor,
            double *a
    );

    /*!
     *
     * calculating p(G,R), several ratios, same P(F)
     *
     * @param SgSr see sgsr_pN
     * @param pF input: p(F)
     * @param Nmax
     * @param Bg
     * @param Br
     * @param N_pg size of pg_theor
     * @param pg_theor
     * @param a corresponding amplitudes
     */
    void sgsr_pF_manypg(
            double *SgSr,
            double *pF,
            unsigned int Nmax,
            double Bg,
            double Br,
            std::vector<double> &pg_theor,
            std::vector<double> &a);

    void sgsr_manypF(
            double *SgSr,        // see sgsr_pN
            double *pF,        // input: p(F); size = (Nmax+1)xN_pg
            unsigned int Nmax,
            double Bg,
            double Br,
            unsigned int N_pg,        // size of pg_theor
            double *pg_theor,
            double *a            // corresponding amplitudes
    );

    /*!
     *
     * @param SgSr
     * @param FgFr
     * @param Nmax
     * @param Bg
     * @param Br
     */
    void conv_pF(double *SgSr, double *FgFr, unsigned int Nmax, double Bg, double Br);

    /*!
     *
     * @param SgSr
     * @param FgFr
     * @param Nmax
     * @param Bg
     * @param Br
     * @param pN
     */
    void conv_pN(double *SgSr, double *FgFr, unsigned int Nmax, double Bg, double Br, double *pN);

    /*!
    * generates Poisson distribution witn average= lam, for 0..N
    *
    * @param return_p
    * @param lam
    * @param return_dim
    */
    void poisson_0toN(double *return_p, double lam, size_t return_dim);

    /*!
     * generates Poisson distribution for a set of lambdas
     */
    void poisson_0toN_multi(double *, double *, unsigned int, unsigned int);

    /*!
     * convolves vectors p and [p2 1-p2]
     */
    void polynom2_conv(double *, double *, unsigned int, double);

}


#endif //TTTRLIB_PDA_H
