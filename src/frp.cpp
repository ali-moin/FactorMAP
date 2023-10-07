// Author: Alberto Quaini

#include "frp.h"
#include "hac_standard_errors.h"

/////////////////
///// FRPCpp ////

Rcpp::List FRPCpp(
  const arma::mat& returns,
  const arma::mat& factors,
  const bool misspecification_robust,
  const bool include_standard_errors
) {

  if (include_standard_errors) {

    const arma::mat covariance_factors_returns = arma::cov(factors, returns);
    const arma::mat variance_returns = arma::cov(returns);
    const arma::vec mean_returns = arma::mean(returns).t();

    const arma::mat beta = arma::solve(
      arma::cov(factors),
      covariance_factors_returns,
      arma::solve_opts::likely_sympd
    ).t();

    const arma::vec frp = misspecification_robust ?
      KRSFRPCpp(beta, mean_returns, variance_returns) :
      FMFRPCpp(beta, mean_returns);

    return Rcpp::List::create(
      Rcpp::Named("risk_premia") =  frp,
      Rcpp::Named("standard_errors") = misspecification_robust ?
        StandardErrorsKRSFRPCpp(
          frp,
          returns,
          factors,
          beta,
          covariance_factors_returns,
          variance_returns,
          mean_returns
        ) :
        StandardErrorsFRPCpp(
          frp,
          returns,
          factors,
          beta,
          covariance_factors_returns,
          variance_returns,
          mean_returns
        )
    );

  } else {

    const arma::mat beta = arma::solve(
      arma::cov(factors),
      arma::cov(factors, returns),
      arma::solve_opts::likely_sympd
    ).t();

    return Rcpp::List::create(
      Rcpp::Named("risk_premia") = misspecification_robust ?
      KRSFRPCpp(beta, arma::mean(returns).t(), arma::cov(returns)) :
      FMFRPCpp(beta, arma::mean(returns).t())
    );

  }

}

arma::vec FMFRPCpp(
  const arma::mat& beta,
  const arma::vec& mean_returns
) {

  return arma::solve(
    beta.t() * beta,
    beta.t(),
    arma::solve_opts::likely_sympd
  ) * mean_returns;

}

////////////////////
///// KRSFRPCpp ////

arma::vec KRSFRPCpp(
  const arma::mat& beta,
  const arma::vec& mean_returns,
  const arma::mat& weighting_matrix
) {

  const arma::mat beta_t_wei_mat_inv = arma::solve(
    weighting_matrix,
    beta,
    arma::solve_opts::likely_sympd
  ).t();

  return arma::solve(
    beta_t_wei_mat_inv * beta,
    beta_t_wei_mat_inv,
    arma::solve_opts::likely_sympd
  ) * mean_returns;

}

//////////////////////////////
///// IterativeKRSFRPCpp ////

arma::vec IterativeKRSFRPCpp(
  const arma::mat& returns, // N x Returns
  const arma::mat& factors, // N x Factors
  const arma::mat& beta, // Returns x Factors
  const arma::mat& covariance_factors_returns, // Factors x Returns
  const arma::mat& variance_returns, // Returns x Returns
  const arma::vec& mean_returns, // Returns
  const arma::mat& weighting_matrix, // Returns x Returns
  const double alpha
) {


  arma::mat factors_ = factors;
  arma::mat beta_ = beta;
  arma::mat covariance_factors_returns_ = covariance_factors_returns;

  int number_of_factors = mean_returns.n_elem;
  int bonferroni_constant = mean_returns.n_elem;

  const double critical_value = R::qnorm(1 - (alpha / (2 * bonferroni_constant)),
                                   0.0, 1.0, true, false);

  std::vector<unsigned int> kept_factors(factors.n_cols);
  std::iota(kept_factors.begin(), kept_factors.end(), 0);

  while (number_of_factors > 0) {
    const arma::vec frp = KRSFRPCpp(beta_, mean_returns, weighting_matrix);

    const arma::vec se = StandardErrorsKRSFRPCpp(frp, returns, factors_, beta_,
                                           covariance_factors_returns_,
                                           variance_returns, mean_returns);

    const arma::vec t_statistics = frp / se;

    const unsigned int factor_to_remove = arma::index_min(arma::abs(t_statistics));

    if (std::abs(t_statistics(factor_to_remove)) > critical_value) {
      return arma::conv_to<arma::vec>::from(kept_factors);
      break;
    } else {
      kept_factors.erase(kept_factors.begin() + factor_to_remove);
      factors_.shed_col(factor_to_remove);
      beta_.shed_col(factor_to_remove);
      covariance_factors_returns_.shed_row(factor_to_remove);
      --number_of_factors;
    }
  }

  // TODO: Is this elegant to return if nothing survives iterative elimination?
  return arma::vec();

}

///////////////////////////////
///// StandardErrorsFRPCpp ////
arma::vec StandardErrorsFRPCpp(
  const arma::vec& frp,
  const arma::mat& returns,
  const arma::mat& factors,
  const arma::mat& beta,
  const arma::mat& covariance_factors_returns,
  const arma::mat& variance_returns,
  const arma::vec& mean_returns
) {

  const arma::mat h_matrix  = arma::inv_sympd(beta.t() * beta);

  const arma::mat a_matrix = h_matrix * beta.t();

  const arma::mat returns_centred = returns.each_row() - mean_returns.t();

  const arma::vec mean_factors = arma::mean(factors).t();

  const arma::mat factors_centred = factors.each_row() - mean_factors.t();

  const arma::mat gamma = returns_centred * a_matrix.t();

  const arma::vec gamma_true = a_matrix * mean_returns;

  const arma::mat phi = gamma - factors;

  const arma::mat phi_centred = phi.each_row() - (gamma_true - mean_factors).t();

  const arma::mat variance_factors = arma::cov(factors);

  const arma::mat fac_centred_var_fac_inv = arma::solve(
    variance_factors,
    factors_centred.t(),
    arma::solve_opts::likely_sympd
  ).t();

  const arma::mat z = fac_centred_var_fac_inv.each_col() % (
    returns_centred * (mean_returns - beta * frp)
  );

  return HACStandardErrorsCpp(
    // mean term
    (gamma.each_row() - (a_matrix * mean_returns).t()) -
    // beta term
    phi_centred.each_col() % (factors_centred * arma::solve(
        variance_factors, gamma_true, arma::solve_opts::likely_sympd
    )) +
    // error term
    (fac_centred_var_fac_inv.each_col() % (
        returns_centred * (mean_returns - beta * frp)
    )) * h_matrix
  );

}

//////////////////////////////////
///// StandardErrorsKRSFRPCpp ////

arma::vec StandardErrorsKRSFRPCpp(
  const arma::vec& krs_frp,
  const arma::mat& returns,
  const arma::mat& factors,
  const arma::mat& beta,
  const arma::mat& covariance_factors_returns,
  const arma::mat& variance_returns,
  const arma::vec& mean_returns
) {

  const arma::mat var_ret_inv_beta = arma::solve(
    variance_returns,
    beta,
    arma::solve_opts::likely_sympd
  );

  const arma::mat a_matrix = arma::solve(
    beta.t() * var_ret_inv_beta,
    var_ret_inv_beta.t(),
    arma::solve_opts::likely_sympd
  );

  const arma::mat returns_centred = returns.each_row() - mean_returns.t();

  const arma::mat factors_centred = factors.each_row() - arma::mean(factors);

  const arma::mat term1 = returns_centred * a_matrix.t();

  const arma::vec var_ret_inv_mean_ret = arma::solve(
    variance_returns,
    mean_returns,
    arma::solve_opts::likely_sympd
  );

  const arma::mat var_fac_inv = arma::inv_sympd(arma::cov(factors));

  const arma::mat hkrs_var_fac_inv = arma::solve(
    beta.t() * var_ret_inv_beta,
    var_fac_inv,
    arma::solve_opts::likely_sympd
  );

  const arma::vec var_ret_inv_err_krs =
    var_ret_inv_mean_ret - var_ret_inv_beta * krs_frp;

  const arma::mat fac_cen_hkrs_var_fac_inv =
    factors_centred * hkrs_var_fac_inv.t();

  const arma::mat term2 = fac_cen_hkrs_var_fac_inv.each_col() %
    (returns_centred * var_ret_inv_err_krs);

  const arma::mat term3 = term1.each_col() % (returns_centred * var_ret_inv_err_krs);

  const arma::mat akrs_ret_cen_minus_fac_cen = term1 - factors_centred;

  const arma::mat term4 = akrs_ret_cen_minus_fac_cen.each_col() %
    (factors_centred * var_fac_inv * a_matrix * mean_returns);

  return HACStandardErrorsCpp(term1 + term2 - term3 - term4);

}
