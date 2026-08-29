#pragma once

#include "portfolio.h"


/**************************************************************************************
 * Type    : TargetPortfolio
 * Purpose : Monetary target produced after final target weights receive strategy capital
 *
 * This is the object that can later be aggregated across independently sized strategies
 * and passed to execution. In the current system its values are USD exposures, although
 * the type itself remains currency-agnostic.
 **************************************************************************************/
using TargetPortfolio = Portfolio;
