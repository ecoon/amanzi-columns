/*
The transport component of the Amanzi code, serial unit tests.
License: BSD
Author: Konstantin Lipnikov (lipnikov@lanl.gov)
*/

#include <cstdlib>
#include <cmath>
#include <iostream>
#include <vector>

#include "UnitTest++.h"

#include "Teuchos_ParameterList.hpp"
#include "Teuchos_RCP.hpp"
#include "Teuchos_XMLParameterListHelpers.hpp"
#include "Teuchos_SerialDenseMatrix.hpp"

#include "MeshFactory.hh"
#include "MeshAudit.hh"

#include "Mesh.hh"
#include "Point.hh"

#include "mfd3d.hh"
#include "tensor.hh"


/* **************************************************************** */
TEST(DARCY_MASS) {
  using namespace Teuchos;
  using namespace Amanzi::AmanziMesh;
  using namespace Amanzi::WhetStone;

  std::cout << "Test: Mass matrix for Darcy" << endl;
#ifdef HAVE_MPI
  Epetra_MpiComm *comm = new Epetra_MpiComm(MPI_COMM_WORLD);
#else
  Epetra_SerialComm *comm = new Epetra_SerialComm();
#endif

  int num_components = 3;

  FrameworkPreference pref;
  pref.clear();
  pref.push_back(Simple);

  MeshFactory meshfactory(comm);
  meshfactory.preference(pref);
  RCP<Mesh> mesh = meshfactory(0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1, 2, 3); 
 
  MFD3D mfd(mesh);

  int nfaces = 6, cell = 0;
  Tensor T(3, 1);
  T(0, 0) = 1;

  Teuchos::SerialDenseMatrix<int, double> N(nfaces, 3);
  Teuchos::SerialDenseMatrix<int, double> Mc(nfaces, nfaces);
  Teuchos::SerialDenseMatrix<int, double> M(nfaces, nfaces);

  int ok = mfd.L2consistency(cell, T, N, Mc);
  mfd.StabilityScalar(cell, N, Mc, M);

  printf("Mass matrix for cell %3d\n", cell);
  for (int i=0; i<6; i++) {
    for (int j=0; j<6; j++ ) printf("%8.4f ", M(i, j)); 
    printf("\n");
  }

  // verify mass matrix
  for (int i=0; i<6; i++) CHECK(M(i, i) > 0.0);

  delete comm;
}


/* **************************************************************** */
TEST(DARCY_INVERSE_MASS) {
  using namespace Teuchos;
  using namespace Amanzi::AmanziMesh;
  using namespace Amanzi::WhetStone;

  std::cout << "Test: Inverse mass matrix for Darcy" << endl;
#ifdef HAVE_MPI
  Epetra_MpiComm *comm = new Epetra_MpiComm(MPI_COMM_WORLD);
#else
  Epetra_SerialComm *comm = new Epetra_SerialComm();
#endif

  int num_components = 3;
  FrameworkPreference pref;
  pref.clear();
  pref.push_back(Simple);

  MeshFactory factory(comm);
  factory.preference(pref);
  RCP<Mesh> mesh = factory(0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1, 2, 3); 
 
  MFD3D mfd(mesh);

  int nfaces = 6, cell = 0;
  Tensor T(3, 1);
  T(0, 0) = 1;

  Teuchos::SerialDenseMatrix<int, double> R(nfaces, 3);
  Teuchos::SerialDenseMatrix<int, double> Wc(nfaces, nfaces);
  Teuchos::SerialDenseMatrix<int, double> W(nfaces, nfaces);

  int ok = mfd.L2consistencyInverse(cell, T, R, Wc);
  mfd.StabilityScalar(cell, R, Wc, W);

  printf("Inverse of mass matrix for cell %3d\n", cell);
  for (int i=0; i<6; i++) {
    for (int j=0; j<6; j++ ) printf("%8.4f ", W(i, j)); 
    printf("\n");
  }

  // verify inverse mass matrix
  for (int i=0; i<6; i++) CHECK(W(i, i) > 0.0);


  delete comm;
}


