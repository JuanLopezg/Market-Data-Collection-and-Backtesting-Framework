#pragma once

#include "portfolio_weights.h"


/**************************************************************************************
 * Type    : TargetWeights
 * Purpose : Semantic name for portfolio weights after portfolio/risk processing
 *
 * TargetWeights are still dimensionless. Capital is intentionally introduced only when
 * these final strategy weights are converted into a TargetPortfolio.
 **************************************************************************************/
using TargetWeights = PortfolioWeights;
