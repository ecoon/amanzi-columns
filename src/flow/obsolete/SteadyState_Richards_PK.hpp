#ifndef __SteadyState_Richards_PK_hpp__
#define __SteadyState_Richards_PK_hpp__

#include "Teuchos_RCP.hpp"
#include "Teuchos_ParameterList.hpp"

#include "Epetra_Vector.h"

#include "Flow_PK.hpp"
#include "Flow_State.hpp"
#include "RichardsProblem.hpp"

namespace Amanzi
{
class SteadyState_Richards_PK : public Flow_PK
{
public:
  SteadyState_Richards_PK(Teuchos::ParameterList&, const Teuchos::RCP<const Flow_State>);

  ~SteadyState_Richards_PK ();

  int advance_to_steady_state();
  void commit_state(Teuchos::RCP<Flow_State>) {};

  int advance_transient(double h) {}; 
  int init_transient(double t0, double h0) {}; 

  // After a successful advance() the following routines may be called.

  // Returns a reference to the cell pressure vector.
  const Epetra_Vector& Pressure() const { return *pressure; }

  // Returns a reference to the Darcy face flux vector.
  const Epetra_Vector& Flux() const { return *darcy_flux; }

  // Computes the components of the Darcy velocity on cells.
  void GetVelocity(Epetra_MultiVector &q) const
      { problem->DeriveDarcyVelocity(*solution, q); }

  // Computes the fluid saturation on cells.
  void GetSaturation(Epetra_Vector &s) const;
  
  double get_flow_dT() {return 0.0;};
private:

  Teuchos::RCP<const Flow_State> FS;

  RichardsProblem *problem;

  Teuchos::RCP<Teuchos::ParameterList> nox_param_p;
  Teuchos::RCP<Teuchos::ParameterList> linsol_param_p;

  Epetra_Vector *solution;   // full cell/face solution
  Epetra_Vector *pressure;   // cell pressures
  Epetra_Vector *darcy_flux; // Darcy face fluxes

  int max_itr;      // max number of linear solver iterations
  double err_tol;   // linear solver convergence error tolerance
  int precon_freq;  // preconditioner update frequency

  // Constructor helpers
  void nox_jfnk_setup(Teuchos::RCP<Teuchos::ParameterList>&, Teuchos::RCP<Teuchos::ParameterList>&) const;
  void nox_nlk_setup(Teuchos::ParameterList&, Teuchos::RCP<Teuchos::ParameterList>&, Teuchos::RCP<Teuchos::ParameterList>&) const;
};

} // close namespace Amanzi

#endif