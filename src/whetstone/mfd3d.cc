/*
  This is the mimetic discretization component of Amanzi. 

  Copyright 2010-2012 held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Release name: ara-to.
  Author: Konstantin Lipnikov (lipnikov@lanl.gov)
*/

#include <cmath>
#include <vector>

#include "Mesh.hh"
#include "Point.hh"

#include "mfd3d.hh"
#include "tensor.hh"


namespace Amanzi {
namespace WhetStone {

/* ******************************************************************
* Constructors
****************************************************************** */
MFD3D::MFD3D(const Teuchos::RCP<const AmanziMesh::Mesh>& mesh) :
  mesh_(mesh) {
  stability_method_ = WHETSTONE_STABILITY_GENERIC;
  simplex_functional_ = 0.0;
  simplex_num_itrs_ = -1;
}


/* ******************************************************************
* Calculate stability factor using matrix and optional scaling.
****************************************************************** */
double MFD3D::CalculateStabilityScalar(DenseMatrix& Mc)
{
  int nrows = Mc.NumRows();

  scalar_stability_ = 0.0;
  for (int i = 0; i < nrows; i++) scalar_stability_ += Mc(i, i);
  scalar_stability_ /= nrows;

  if (stability_method_ == WHETSTONE_STABILITY_GENERIC_SCALED) {
    scalar_stability_ *= scaling_factor_;
  }

  return scalar_stability_;
}


/* ******************************************************************
* Set up positive scaling factor for a scalar stability term.
* Warning: Ignores silently negative factors.
****************************************************************** */
void MFD3D::ModifyStabilityScalingFactor(double factor)
{
  if (factor > 0.0) {
    stability_method_ = WHETSTONE_STABILITY_GENERIC_SCALED;
    scaling_factor_ = factor;
  }
}


/* ******************************************************************
* Simplest stability term is added to the consistency term. 
****************************************************************** */
void MFD3D::StabilityScalar(int cell, DenseMatrix& N,
                            DenseMatrix& Mc, DenseMatrix& M)
{
  GrammSchmidt(N);
  CalculateStabilityScalar(Mc);

  int nrows = Mc.NumRows();
  int ncols = N.NumCols();

  for (int i = 0; i < nrows; i++) {
    for (int j = i; j < nrows; j++) M(i, j) = Mc(i, j);
  }

  for (int i = 0; i < nrows; i++) {  // add projector ss * (I - N^T N) to matrix M
    M(i, i) += scalar_stability_;

    for (int j = i; j < nrows; j++) {
      double s = 0.0;
      for (int k = 0; k < ncols; k++)  s += N(i, k) * N(j, k);
      M(i, j) -= s * scalar_stability_;
    }
  }

  for (int i = 0; i < nrows; i++) {  // symmetrization
    for (int j = i+1; j < nrows; j++) M(j, i) = M(i, j);
  }
}


/* ******************************************************************
* A simple optimization procedure that returns a diagonal mass
* matrix for a 2D and 3D orthogonal cells and diagonal tensors. 
* The algorithm minimizes off-diagonal entries in the mass matrix.
* WARNING: the routine is used for inverse of mass matrix only.
****************************************************************** */
int MFD3D::StabilityOptimized(const Tensor& T, DenseMatrix& N,
                              DenseMatrix& Mc, DenseMatrix& M)
{
  int d = mesh_->space_dimension();
  int nrows = N.NumRows();
  int ncols = N.NumCols();

  // find correct scaling of a stability term
  double lower, upper, eigmin = Mc(0, 0);
  // T.spectral_bounds(&lower, &upper);
  for (int k = 1; k < nrows; k++) eigmin = std::min(eigmin, Mc(k, k));

  // find null space of N^T
  DenseMatrix U(nrows, nrows);
  int info, ldv = 1, size = 5 * d + 3 * nrows;
  double V, S[nrows], work[size];

  DGESVD_F77("A", "N", &nrows, &ncols, N.Values(), &nrows,  // N = u s v
             S, U.Values(), &nrows, &V, &ldv, work, &size, &info);

  if (info != 0) return WHETSTONE_ELEMENTAL_MATRIX_FAILED;

  // calculate vectors C and C0
  int mrows = nrows * (nrows - 1) / 2;
  int mcols = nrows - ncols;
  int nparam = (mcols + 1) * mcols / 2;
  DenseMatrix C(mrows, nparam);
  DenseVector F(mrows);

  int m, n = 0;
  for (int k = ncols; k < nrows; k++) {
    m = 0;  // calculate diagonal entries of M_kk = U_k * U_k^T
    for (int i = 0; i < nrows; i++) 
      for (int j = i+1; j < nrows; j++) C(m++, n) = U(i, k) * U(j, k);
    n++; 
  }

  for (int k = ncols; k < nrows; k++) {
    for (int l = k+1; l < nrows; l++) {
      m = 0;  // calculate off-diagonal entries of M_kk + M_ll - M_kl - M_lk 
      for (int i = 0; i < nrows; i++) { 
        for (int j = i+1; j < nrows; j++) {
          C(m, n) = C(m, k-ncols) + C(m, l-ncols) - U(i, k) * U(j, l) - U(i, l) * U(j, k);
          m++;
        }
      }
      n++;
    }
  }

  m = 0;
  for (int i = 0; i < nrows; i++) { 
    for (int j = i+1; j < nrows; j++) F(m++) = -Mc(i, j);
  }

  // Form a linear system for parameters
  DenseMatrix A(nparam, nparam);
  DenseVector G(nparam);

  A.Multiply(C, C, true);  // A = C^T C
  C.Multiply(F, G, true);

  // Find parameters
  int nrhs = 1;
  DPOSV_F77("U", &nparam, &nrhs, A.Values(), &nparam, G.Values(), &nparam, &info);
  if (info != 0) return WHETSTONE_ELEMENTAL_MATRIX_FAILED;

  // project solution on the positive quadrant and convert to matrix
  DenseMatrix P(mcols, mcols);
  P.PutScalar(0.0);

  int status = WHETSTONE_ELEMENTAL_MATRIX_OK;
  for (int loop = 0; loop < 3; loop++) {
    if (loop == 1) {   
      for (int i = 0; i < mcols; i++) G(i) = std::max(G(i), 0.0);
      status = WHETSTONE_ELEMENTAL_MATRIX_PASSED;
    } else if (loop == 2) {
      for (int i = mcols; i < nparam; i++) G(i) = std::max(G(i), 0.0);
      status = WHETSTONE_ELEMENTAL_MATRIX_PASSED;
    }

    for (int k = 0; k < mcols; k++) P(k, k) = G(k);

    n = mcols;
    for (int k = 0; k < mcols; k++) {
      for (int l = k+1; l < mcols; l++) {
        P(k, k) += G(n);
        P(l, l) += G(n);
        P(l, k) = P(k, l) = -G(n);
        n++;  
      }
    }

    // check SPD property (we use allocated memory)
    DenseMatrix Ptmp(P);
    DSYEV_F77("N", "U", &mcols, Ptmp.Values(), &mcols, S, work, &size, &info); 
    if (info != 0) return WHETSTONE_ELEMENTAL_MATRIX_FAILED;

    if (S[0] > eigmin) {
      break;
    } else if (loop == 2) {
      for (int k = 0; k < mcols; k++) if (P(k, k) == 0.0) P(k, k) = eigmin;
    }
  }

  // add stability term U G U^T
  for (int i = 0; i < nrows; i++) {
    for (int j = i; j < nrows; j++) M(i, j) = Mc(i, j);
  }

  DenseMatrix UP(nrows, mcols);
  UP.PutScalar(0.0);
  for (int i = 0; i < nrows; i++) {
    for (int j = 0; j < mcols; j++) {
      double& entry = UP(i, j);
      for (int k = 0; k < mcols; k++) entry += U(i, k+ncols) * P(k, j);
    }
  }

  for (int i = 0; i < nrows; i++) {
    for (int j = i; j < nrows; j++) {
      double& entry = M(i, j);
      for (int k = 0; k < mcols; k++) entry += UP(i, k) * U(j, k+ncols);
      M(j, i) = M(i, j); 
    }
  }

  return status;
}


/* ******************************************************************
* A wrapper for the simplex method that finds monotone parameters. 
****************************************************************** */
int MFD3D::StabilityMMatrix_(
    int cell, DenseMatrix& N, DenseMatrix& Mc, DenseMatrix& M, int objective)
{
  int d = mesh_->space_dimension();
  int nrows = N.NumRows();
  int ncols = N.NumCols();

  for (int i = 0; i < nrows; i++) {
    for (int j = i; j < nrows; j++) M(j, i) = M(i, j) = Mc(i, j);
  }

  // compute null space
  int mcols = nrows - ncols;
  DenseMatrix D(nrows, mcols);
  int ierr = N.NullSpace(D);

  // populate the tableau
  int m1(0), m2(nrows), m12, n12, mx, nx, ir;
  double tmp;

  m12 = nrows * (nrows + 1) / 2;
  mx = mcols * mcols;

  // Simplex method requires one auxiliary row in the tableau.
  DenseMatrix T(m12 + 2, mx + 1);
  T.PutScalar(0.0);

  // first condition M_ij < 0
  n12 = m12 - nrows;
  for (int i = 0; i < nrows; i++) {
    for (int j = i + 1; j < nrows; j++) {
      double b = M(i, j);
      if (b < 0.0) {
        ir = ++m1;
      } else {
        ir = n12--;
        m2++;
      }
      T(ir, 0) = b;
      
      nx = 0;
      for (int k = 0; k < mcols; k++) {
        tmp = D(i, k) * D(j, k);
        T(ir, ++nx) = tmp;
        for (int l = k + 1; l < mcols; l++) {
          tmp = D(i, k) * D(j, l) + D(j, k) * D(i, l);
          T(ir, ++nx) = tmp;
          T(ir, ++nx) = -tmp;
        }
      }

      if (b < 0.0) {
        for (int k = 0; k < mx + 1; k++) T(ir, k) *= -1;
      }
    }
  }

  // second condition sum_j M_ij > 0
  for (int i = 0; i < nrows; i++) {
    for (int j = i; j < nrows; j++) {
      nx = 0;
      for (int k = 0; k < mcols; k++) {
        tmp = D(i, k) * D(j, k);
        nx++;
        T(m12 - i, nx) -= tmp;
        if (i != j) T(m12 - j, nx) -= tmp;

        for (int l = k + 1; l < mcols; l++) {
          tmp = D(i, k) * D(j, l) + D(j, k) * D(i, l);
          nx++;
          T(m12 - i, nx) -= tmp;
          if (i != j) T(m12 - j, nx) -= tmp;

          nx++;
          T(m12 - i, nx) += tmp;
          if (i != j) T(m12 - j, nx) += tmp;
        }
      }
    }
  }

  // objective functional
  if (objective == WHETSTONE_SIMPLEX_FUNCTIONAL_SUMALL) {
    n12 = m12 - nrows + 1;
    for (int k = 0; k < mx + 1; k++) {
      double q1 = 0.0;
      for (int i = n12; i <= m12; i++) q1 += T(i, k);
      T(0, k) = -q1;
    }
  } else if (objective == WHETSTONE_SIMPLEX_FUNCTIONAL_TRACE) {
    for (int i = 0; i < nrows; i++) {
      T(0, 0) += M(i, i);
      nx = 0;
      for (int k = 0; k < mcols; k++) {
        tmp = D(i, k) * D(i, k);
        nx++;
        T(0, nx) += tmp;

        for (int l = k + 1; l < mcols; l++) {
          tmp = 2 * D(i, k) * D(i, l);
          nx++;
          T(0, nx) += tmp;

          nx++;
          T(0, nx) -= tmp;
        }
      }
    }
  }

  // find a feasible basic solution
  int izrow[mx + 1], iypos[m12 + 1], itrs;
  itrs = SimplexFindFeasibleSolution_(T, m1, m2, 0, izrow, iypos);
  if (itrs < 0) return WHETSTONE_ELEMENTAL_MATRIX_FAILED;
  simplex_functional_ = T(0,0); 
  simplex_num_itrs_ = itrs; 

  double u[mx];
  for (int i = 0; i < mx; i++) u[i] = 0.0;
  for (int i = 1; i < m12 + 1; i++) {
    int k = iypos[i] - 1;
    if (k < mx) u[k] = T(i,0);
  }

  // add matrix D' U D
  for (int i = 0; i < nrows; i++) {
    for (int j = i; j < nrows; j++) { 
      nx = 0;
      for (int k = 0; k < mcols; k++) {
        M(i, j) += D(i, k) * D(j, k) * u[nx];
        nx++;
        for (int l = k + 1; l < mcols; l++) {
          double tmp = D(i, k) * D(j, l) + D(j, k) * D(i, l);
          M(i, j) += tmp * (u[nx] - u[nx + 1]);
          nx += 2;
        }
      }
      M(j, i) = M(i, j);
    }
  }

  return WHETSTONE_ELEMENTAL_MATRIX_OK;
}


/* ******************************************************************
* A simplex method for fining monotone parameters. 
* We assume that m3 = 0; otherwise, routine MaxRowValue() has 
* to be modified by looping over columns in array l1.
****************************************************************** */
int MFD3D::SimplexFindFeasibleSolution_(
    DenseMatrix& T, int m1, int m2, int m3, int* izrow, int* iypos)
{
  int m = m1 + m2 + m3;     // Number of constraints.
  int n = T.NumCols() - 1;  // Number of unknowns.
  
  for (int k = 0; k < n + 1; k++) {
    double q1 = 0.0;
    for (int i = m1 + 1; i < m + 1; i++) q1 += T(i, k);
    T(m + 1, k) = -q1;
  }

  // scaling
  double tol = WHETSTONE_SIMPLEX_TOLERANCE * n;

  // work memory
  int ip, kp, itr_max = WHETSTONE_SIMPLEX_MAX_ITERATIONS * n;
  int itr1(0), itr2(0), nl1(n), l1[n + 1];

  for (int k = 0; k < n + 1; k++) l1[k] = izrow[k] = k;
  for (int i = 0; i < m + 1; i++) iypos[i] = n + i;

  // Start of phase I.
  if (m2 + m3 > 0) {
    int flag(0), l3[m2];
    for (int i = 0; i < m2; i++) l3[i] = 1;

    for (int itr = 0; itr < itr_max; itr++) {
      // find maximum coeffient of the auxiliary functional
      double vmax;
      T.MaxRowValue(m + 1, 1, n, &kp, &vmax); 

      // feasible solution does not exist 
      if (vmax < tol && T(m + 1, 0) < -tol) 
          return WHETSTONE_SIMPLEX_NO_FEASIBLE_SET;

      // feasible solution has been found
      if (vmax < tol && fabs(T(m + 1, 0)) < tol) {
        /*
        for (int ip = m1 + m2 + 1; ip < m + 1; ip++) {
          if (iypos[ip] == ip + n) {
            // Found an artificial variable for an equality constraint.
            T.MaxRowMagnitude(ip, 1, n, &kp, &vmax);
            if (vmax > tol) goto one;
          }
        }
        */

        for (int i = m1 + 1; i < m + 1; i++) {
          if (l3[i - m1 - 1] == 1) {
            for (int k = 0; k < n + 1; k++) T(i, k) *= -1; 
          }
        }
        itr1 = itr;
        flag = 1;
        break;
      }

      // locate a pivot element in column kp (skipping degeneracy)
      SimplexPivotElement_(T, kp, &ip);
      if (ip == 0) return WHETSTONE_SIMPLEX_UNBOUNDED_PROBLEM;

      // Exchange left and right-hand variables
      SimplexExchangeVariables_(T, kp, ip);

      // Exchanged out an artificial variable foranequality constraint.
      // Make sure it stays out by removing it from the l1 list.
      if (iypos[ip] >= n + m1 + m2 + 1) {
        for (int k = 1; k <= nl1; k++) {
          if (l1[k] == kp) { 
            --nl1;
            for (int i = k; i <= nl1; i++) l1[i] = l1[i + 1];
            break;
          }
        }
      } else {
      // Exchanged out an m2 type constraint for the first time. 
      // Correct sign of the pivot column and the implicit artificial variable.
        int kh = iypos[ip] - m1 - n - 1;
        if (kh >= 0 && l3[kh] == 1) {
          l3[kh] = 0;
          T(m + 1, kp) += 1.0;
          for (int i = 0; i < m + 2; i++) T(i, kp) *= -1;
        }
      }
      // Update lists of left-hand and right-hand variables.
      int is = izrow[kp];
      izrow[kp] = iypos[ip];
      iypos[ip] = is;
    }
    if (flag == 0) return WHETSTONE_SIMPLEX_NO_CONVERGENCE;
  }

  // Start of phase II.
  int flag(0);
  for (int itr = 0; itr < itr_max; itr++) {
    double vmax;
    T.MaxRowValue(0, 1, n, &kp, &vmax);

    // solution has been found
    if (vmax < tol) {
      itr2 = itr;
      flag = 1;
      break;
    }

    // Locate a pivot element.
    SimplexPivotElement_(T, kp, &ip);
    if (ip == 0) return WHETSTONE_SIMPLEX_UNBOUNDED_PROBLEM;

    // Exchange a left-hand and a right-hand variables.
    SimplexExchangeVariables_(T, kp, ip);

    int is = izrow[kp];
    izrow[kp] = iypos[ip];
    iypos[ip] = is;
  }
  if (flag == 0) return WHETSTONE_SIMPLEX_NO_CONVERGENCE;

  return itr1 + itr2;
}


/* ******************************************************************
* Locates a pivot elements taking degeneracy into account.
****************************************************************** */
void MFD3D::SimplexPivotElement_(DenseMatrix& T, int kp, int* ip)
{
  int m = T.NumRows() - 2;
  int n = T.NumCols() - 1;
  double qmin, q, qp, q0, tmp;
  double tol = WHETSTONE_SIMPLEX_TOLERANCE * n;

  *ip = 0;
  for (int i = 1; i < m + 1; i++) {
    tmp = T(i, kp);
    if (tmp < -tol) {
      q = -T(i, 0) / tmp;

      if (*ip == 0) {
        *ip = i;
        qmin = q;
      } else if (q < qmin) {
        *ip = i;
        qmin = q;
      } else if (q == qmin) {  // we have a degeneracy.
        double tmp0 = T(*ip, kp);
        for (int k = 1; k <= n; k++) {
          qp = -T(*ip, k) / tmp0;
          q0 = -T(i, k) / tmp;
          if (q0 != qp) break;
        }
        if (q0 < qp) *ip = i;
      }
    }
  }
}


/* ******************************************************************
* Exchanges lenf-hand and rihgt-hand variables.
****************************************************************** */
void MFD3D::SimplexExchangeVariables_(DenseMatrix& T, int kp, int ip)
{
  int m = T.NumRows() - 2;
  int n = T.NumCols() - 1;

  double tmp = 1.0 / T(ip, kp);
  for (int i = 0; i < m + 2; i++) {
    if (i != ip) {
      T(i, kp) *= tmp;
      for (int k = 0; k < n + 1; k++) {
        if (k != kp) T(i, k) -= T(ip, k) * T(i, kp);
      }
    }
  }
  for (int k = 0; k < n + 1; k++) {
    if (k != kp) T(ip, k) *= -tmp;
  }
  T(ip, kp) = tmp;
}


/* ******************************************************************
* Conventional Gramm-Schimdt orthogonalization of colums of matrix N. 
****************************************************************** */
void MFD3D::GrammSchmidt(DenseMatrix& N)
{
  int nrows = N.NumRows();
  int ncols = N.NumCols();

  int i, j, k;
  for (i = 0; i < ncols; i++) {
    double l22 = 0.0;
    for (k = 0; k < nrows; k++) l22 += N(k, i) * N(k, i);

    l22 = 1.0 / sqrt(l22);
    for (k = 0; k < nrows; k++) N(k, i) *= l22;

    for (j = i+1; j < ncols; j++) {
      double s = 0.0;
      for (k = 0; k < nrows; k++) s += N(k, i) * N(k, j);
      for (k = 0; k < nrows; k++) N(k, j) -= s * N(k, i);  // orthogonolize i and j
    }
  }
}


/* ******************************************************************
* Extension of Mesh API. 
****************************************************************** */
int MFD3D::cell_get_face_adj_cell(const int cell, const int face)
{
  Entity_ID_List cells;
  mesh_->face_get_cells(face, (ParallelTypeCast)WhetStone::USED, &cells);
  int ncells = cells.size();

  if (ncells == 2) {
    int c2 = cells[0];
    if (cell == c2) c2 = cells[1];
    return c2;
  }
  return -1;
}


/* ******************************************************************
* Returns position of the number v in the list of nodes.  
****************************************************************** */
int MFD3D::FindPosition_(int v, Entity_ID_List nodes)
{
  for (int i = 0; i < nodes.size(); i++) {
    if (nodes[i] == v) return i;
  }
  return -1;
}

}  // namespace WhetStone
}  // namespace Amanzi



