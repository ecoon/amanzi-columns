#include "LinearFunction.hh"
#include "errors.hh"

namespace Amanzi {

LinearFunction::LinearFunction(double y0, const std::vector<double> &grad, const std::vector<double> &x0)
{
  if (grad.size() < 1) {
    Errors::Message m;
    m << "At least one value required for the gradient vector";
    Exceptions::amanzi_throw(m);
  }
  if (x0.size() != grad.size()) {
    Errors::Message m;
    m << "Mismatch of gradient and point dimensions.";
    Exceptions::amanzi_throw(m);
  }
  y0_ = y0;
  grad_ = grad;
  x0_ = x0;
}

LinearFunction::LinearFunction(double y0, const std::vector<double> &grad)
{
  if (grad.size() < 1) {
    Errors::Message m;
    m << "at least one value required for the gradient vector";
    Exceptions::amanzi_throw(m);
  }
  y0_ = y0;
  grad_ = grad;
  x0_.assign(grad.size(), 0.0);
}

double LinearFunction::operator()(const std::vector<double>& x) const
{
  double y = y0_;
  if (x.size() < grad_.size()) {
    Errors::Message m;
    m << "LinearFunction expects higher-dimensional argument.";
    Exceptions::amanzi_throw(m);
  }    
  for (int j = 0; j < grad_.size(); ++j) y += grad_[j]*(x[j] - x0_[j]);
  return y;
}

}  // namespace Amanzi
