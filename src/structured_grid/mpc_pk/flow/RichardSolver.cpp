#include <RichardSolver.H>
#include <RICHARDSOLVER_F.H>
#include <POROUSMEDIA_F.H>

#include <Utility.H>

#undef PETSC_3_2
#define PETSC_3_2 1

static RichardSolver* static_rs_ptr = 0;
static Real norm0 = -1;
static Real max_residual_growth_factor = 1.e8; // FIXME: set this with rsparams
static Real min_dt = 1.e-2; // FIXME: set this with rsparams
static bool dump_Jacobian_and_exit                    = false;

void
RichardSolver::SetTheRichardSolver(RichardSolver* ptr)
{
  static_rs_ptr = ptr;
}

// Forward declaration of local helper functions
static void MatSqueeze(Mat& J);
PetscErrorCode RichardComputeJacobianColor(SNES snes,Vec x1,Mat *J,Mat *B,MatStructure *flag,void *ctx);
PetscErrorCode RichardMatFDColoringApply(Mat J,MatFDColoring coloring,Vec x1,MatStructure *flag,void *sctx);
PetscErrorCode SemiAnalyticMatFDColoringApply(Mat J,MatFDColoring coloring,Vec x1,MatStructure *flag,void *sctx);
PetscErrorCode PostCheck(SNES snes,Vec x,Vec y,Vec w,void *ctx,PetscBool  *changed_y,PetscBool  *changed_w);
PetscErrorCode PostCheckAlt(SNES snes,Vec x,Vec y,Vec w,void *ctx,PetscBool  *changed_y,PetscBool  *changed_w);
PetscErrorCode RichardJacFromPM(SNES snes, Vec x, Mat* jac, Mat* jacpre, MatStructure* flag, void *dummy);
PetscErrorCode RichardRes_DpDt(SNES snes,Vec x,Vec f,void *dummy);
PetscErrorCode RichardR2(SNES snes,Vec x,Vec f,void *dummy);

struct CheckCtx
{
    RichardSolver* rs;
    NLScontrol* nlsc;
};

#undef __FUNCT__
#define __FUNCT__ "CheckForLargeResidual"
PetscErrorCode
CheckForLargeResidual(SNES snes,PetscReal new_res_norm,bool* res_is_large,void *ctx)
{
  if (norm0<=0) {
    if (ParallelDescriptor::IOProcessor()) {
      std::cout << "****************** Initial residual not properly set " << std::endl;
    }
    PetscFunctionReturn(1);
  }
  *res_is_large = (new_res_norm / norm0 >= max_residual_growth_factor);
  PetscFunctionReturn(0);
}


#undef __FUNCT__
#define __FUNCT__ "CheckForSmallDt"
PetscErrorCode
CheckForSmallDt(PetscReal dt,bool* dt_is_small,void *ctx)
{
  *dt_is_small = (dt > 0 && dt < min_dt);
  PetscFunctionReturn(0);
}


#undef __FUNCT__
#define __FUNCT__ "RichardSolverCtr"
RichardSolver::RichardSolver(RSdata& _rs_data, NLScontrol& _nlsc)
  : rs_data(_rs_data), nlsc(_nlsc)
{
  Layout& layout = GetLayout();
  mftfp = new MFTFillPatch(layout);

  PetscErrorCode ierr;
  int n = layout.NumberOfLocalNodeIds();
  int N = layout.NumberOfGlobalNodeIds();
  MPI_Comm comm = ParallelDescriptor::Communicator();
  ierr = VecCreateMPI(comm,n,N,&RhsV); CHKPETSC(ierr);
  ierr = VecDuplicate(RhsV,&SolnV); CHKPETSC(ierr);
  ierr = VecDuplicate(RhsV,&SolnTypV); CHKPETSC(ierr);
  ierr = VecDuplicate(RhsV,&SolnTypInvV); CHKPETSC(ierr);
  ierr = VecDuplicate(RhsV,&GV); CHKPETSC(ierr);
  ierr = VecDuplicate(RhsV,&AlphaV); CHKPETSC(ierr);

  mftfp->BuildStencil(rs_data.pressure_bc, rs_data.pressure_maxorder);

  // Estmated number of nonzero local columns of J
  int d_nz = (nlsc.use_dense_Jacobian ? N : 1 + (rs_data.pressure_maxorder-1)*(2*BL_SPACEDIM));
  int o_nz = 0; // Estimated number of nonzero nonlocal (off-diagonal) columns of J

#if defined(PETSC_3_2)
  ierr = MatCreateMPIAIJ(comm, n, n, N, N, d_nz, PETSC_NULL, o_nz, PETSC_NULL, &Jac); CHKPETSC(ierr);
#else
  ierr = MatCreate(comm, &Jac); CHKPETSC(ierr);
  ierr = MatSetSizes(Jac,n,n,N,N);  CHKPETSC(ierr);
  ierr = MatSetFromOptions(Jac); CHKPETSC(ierr);
  ierr = MatSeqAIJSetPreallocation(Jac, d_nz*d_nz, PETSC_NULL); CHKPETSC(ierr);
  ierr = MatMPIAIJSetPreallocation(Jac, d_nz, PETSC_NULL, o_nz, PETSC_NULL); CHKPETSC(ierr);
#endif

  BuildOpSkel(Jac);

  if (!rs_data.upwind_krel  &&  rs_data.subgrid_krel) {
    BuildMLPropEval();
  }

  matfdcoloring = 0;
  ierr = SNESCreate(comm,&snes); CHKPETSC(ierr);
  ierr = SNESSetFunction(snes,RhsV,RichardRes_DpDt,(void*)(this)); CHKPETSC(ierr);

  if (nlsc.use_fd_jac) {
    ierr = MatGetColoring(Jac,MATCOLORINGSL,&iscoloring); CHKPETSC(ierr);
    ierr = MatFDColoringCreate(Jac,iscoloring,&matfdcoloring); CHKPETSC(ierr);
    if (rs_data.semi_analytic_J) {
        ierr = MatFDColoringSetFunction(matfdcoloring,
                                        (PetscErrorCode (*)(void))RichardR2,
                                        (void*)(this)); CHKPETSC(ierr);
    }
    else {
        ierr = MatFDColoringSetFunction(matfdcoloring,
                                        (PetscErrorCode (*)(void))RichardRes_DpDt,
                                        (void*)(this)); CHKPETSC(ierr);
    }
    ierr = MatFDColoringSetFromOptions(matfdcoloring); CHKPETSC(ierr);
    ierr = MatFDColoringSetParameters(matfdcoloring,nlsc.errfd,PETSC_DEFAULT);CHKPETSC(ierr);
    ierr = SNESSetJacobian(snes,Jac,Jac,RichardComputeJacobianColor,matfdcoloring);CHKPETSC(ierr);
  }
  else {
    ierr = SNESSetJacobian(snes,Jac,Jac,RichardJacFromPM,(void*)(this));CHKPETSC(ierr);
  }

  ierr = SNESSetTolerances(snes,nlsc.atol,nlsc.rtol,nlsc.stol,nlsc.maxit,nlsc.maxf);CHKPETSC(ierr);
  ierr = SNESSetFromOptions(snes);CHKPETSC(ierr);
}

BoxArray
ComplementIn(const BoxArray& ba, const BoxArray& ba_in, bool do_simplify = false)
{
  BoxList bl;
  for (int i=0; i<ba_in.size(); ++i) {
    bl.join(BoxList(BoxLib::complementIn(ba_in[i],ba)));
  }
  if (do_simplify) {
    bl.simplify();
  }
  return BoxArray(bl);
}

BoxArray
Join(const BoxArray& ba1, const BoxArray& ba2, bool do_simplify = false)
{
  BoxList bl(ba1);
  bl.join(ba2.boxList());
  if (do_simplify) {
    bl.simplify();
  }
  return BoxArray(bl);
}

void
RichardSolver::BuildMLPropEval()
{
  BL_ASSERT(!rs_data.upwind_krel  &&  rs_data.subgrid_krel);
  BoxLib::Abort("RichardSolver::BuildMLPropEval: subgrid_krel not ready for primetime");
#if 0
  MatFiller* matFiller = pm_amr.GetMatFiller();
  bool ret = matFiller != 0 && matFiller->Initialized();
  if (!ret) {
    BoxLib::Abort("RichardSolver::BuildMLPropEval: matFiller not ready");
  }

  Layout& layout = GetLayout();
  const Array<BoxArray>& gridArray = layout.GridArray();
  int num_levs_mixed = matFiller->NumLevels();

  state_to_fill.resize(num_levs_mixed);
  derive_to_fill.resize(num_levs_mixed);

  for (int lev=0; lev<num_levs_mixed; ++lev) {
    const BoxArray& stf = (lev==0 ? gridArray[0] : state_to_fill[lev]);
    BoxArray mixed = BoxLib::intersect(matFiller->Mixed(lev),stf);
    if (mixed.size()>0) {
      if (lev<num_levs_mixed-1) {
	state_to_fill[lev+1] = BoxArray(mixed).refine(matFiller->RefRatio(lev));
	BoxList bl(state_to_fill[lev+1]); bl.simplify(); state_to_fill[lev+1] = BoxArray(bl);
      }
      state_to_fill[lev] = ComplementIn(mixed,state_to_fill[lev],true);
    }
  }

  for (int lev=0; lev<num_levs_mixed; ++lev) {
    if (lev==0) {
      derive_to_fill[lev] = matFiller->Mixed(lev);
    }
    if (lev<num_levs_mixed-1) {
      derive_to_fill[lev] = Join(derive_to_fill[lev],matFiller->Mixed(lev),true);
    }
    if (lev>0) {
      derive_to_fill[lev] = Join(derive_to_fill[lev],
				 BoxArray(matFiller->Mixed(lev-1)).refine(matFiller->RefRatio(lev-1)),true);
    }
    derive_to_fill[lev].removeOverlap();
  }

  for (int lev=0; lev<num_levs_mixed; ++lev) {
    int mg = pm_amr.maxGridSize(lev);
    if (state_to_fill[lev].size()>0) {
      state_to_fill[lev].maxSize(mg);
    }
    if (derive_to_fill[lev].size()>0) {
      derive_to_fill[lev].maxSize(mg);
    }
  }

  int num_fill = state_to_fill.size();
  phif.resize(num_fill,PArrayManage);
  pcPf.resize(num_fill,PArrayManage);
  kf.resize(num_fill,PArrayManage);
  krf.resize(num_fill,PArrayManage);
  pf.resize(num_fill,PArrayManage);
  lf.resize(num_fill,PArrayManage);

  for (int lev=1; lev<num_fill; ++lev) {
    const BoxArray& stff = state_to_fill[lev];
    if (stff.size()>0) {
      int ncPhi = matFiller->nComp("porosity");
      phif.set(lev, new MultiFab(stff,ncPhi,0));

      int ncPcP = matFiller->nComp("capillary_pressure");
      pcPf.set(lev, new MultiFab(stff,ncPcP,0));

      int ncK = matFiller->nComp("permeability");
      kf.set(lev, new MultiFab(stff,ncK,0));

      int ncKr = matFiller->nComp("relative_permeability");
      krf.set(lev, new MultiFab(stff,ncKr,0));

      pf.set(lev, new MultiFab(stff,1,0));
      lf.set(lev, new MultiFab(stff,1,0));
    }
  }

  int num_derive = derive_to_fill.size();
  lc.resize(num_derive,PArrayManage);
  for (int lev=0; lev<num_derive; ++lev) {
    if (derive_to_fill[lev].size()>0) {
      lc.set(lev,new MultiFab(derive_to_fill[lev],BL_SPACEDIM,0));
    }
  }
#endif
}

#undef __FUNCT__
#define __FUNCT__ "RichardSolverDtr"
RichardSolver::~RichardSolver()
{
    PetscErrorCode ierr;

    ierr = MatFDColoringDestroy(&matfdcoloring); CHKPETSC(ierr);
    ierr = ISColoringDestroy(&iscoloring);
    ierr = SNESDestroy(&snes); CHKPETSC(ierr);
    ierr = MatDestroy(&Jac); CHKPETSC(ierr);

    ierr = VecDestroy(&AlphaV); CHKPETSC(ierr);
    ierr = VecDestroy(&GV); CHKPETSC(ierr);
    ierr = VecDestroy(&SolnTypInvV); CHKPETSC(ierr);
    ierr = VecDestroy(&SolnTypV); CHKPETSC(ierr);
    ierr = VecDestroy(&SolnV); CHKPETSC(ierr);
    ierr = VecDestroy(&RhsV); CHKPETSC(ierr);

    delete mftfp;
}

static int dump_cnt = 0;

#undef __FUNCT__
#define __FUNCT__ "Richard_SNESConverged"
PetscErrorCode Richard_SNESConverged(SNES snes, PetscInt it,PetscReal xnew_norm,
                                     PetscReal dx_norm, PetscReal fnew_norm,
                                     SNESConvergedReason *reason, void *ctx)
{
    CheckCtx *check_ctx = (CheckCtx *) ctx;
    RichardSolver* rs = check_ctx->rs;
    const NLScontrol& nlsc = rs->GetNLScontrol();

    PetscErrorCode ierr;
    if (!nlsc.ls_success) {
      *reason = SNES_DIVERGED_LINE_SEARCH;
      ierr = PETSC_ERR_NOT_CONVERGED;
    }
    else {
      PetscReal atol, rtol, stol;
      PetscInt maxit, maxf;
      ierr = SNESGetTolerances(snes,&atol,&rtol,&stol,&maxit,&maxf); CHKPETSC(ierr);
      ierr = SNESDefaultConverged(snes,it,xnew_norm,dx_norm,fnew_norm,reason,ctx); CHKPETSC(ierr);
    }

    if (*reason > 0) {
        dump_cnt = 0;
    }

    PetscFunctionReturn(ierr);
}

void
RichardSolver::ResetRemainingJacobianReuses()
{
    num_remaining_Jacobian_reuses = GetRSdata().max_num_Jacobian_reuses;
}

void
RichardSolver::UnsetRemainingJacobianReuses()
{
    num_remaining_Jacobian_reuses = 0;
}

bool
RichardSolver::ReusePreviousJacobian()
{
    --num_remaining_Jacobian_reuses;
    if (ParallelDescriptor::IOProcessor() && num_remaining_Jacobian_reuses > 0) {
        std::cout << "Reusing J, " << num_remaining_Jacobian_reuses << " reuses left." << std::endl;
    }
    return (num_remaining_Jacobian_reuses > 0);
}

#undef __FUNCT__
#define __FUNCT__ "Solve"
int
RichardSolver::Solve(Real prev_time, Real cur_time, int timestep, NLScontrol& nlsc)
{
  CheckCtx check_ctx;
  check_ctx.rs = this;
  check_ctx.nlsc = &nlsc;

  bool dt_is_small;
  PetscErrorCode ierr;
  Real delta_t = cur_time - prev_time;
  if (delta_t > 0) {
    ierr = CheckForSmallDt(delta_t,&dt_is_small,(void*)(&check_ctx)); CHKPETSC(ierr);
    if (dt_is_small) {
      PetscFunctionReturn(-9);
    }
  }

  MFTower& RhsMFT = GetResidual();
  MFTower& SolnMFT = GetPressureNp1();
  MFTower& PCapParamsMFT = GetPCapParams();

  Vec& RhsV = GetResidualV();
  Vec& SolnV = GetPressureV();
  Vec& SolnTypInvV = GetSolnTypInvV();

  // Copy from MFTowers in state to Vec structures
  Layout& layout = GetLayout();
  ierr = layout.MFTowerToVec(RhsV,RhsMFT,0); CHKPETSC(ierr);
  ierr = layout.MFTowerToVec(SolnV,SolnMFT,0); CHKPETSC(ierr);

  if (check_ctx.rs->GetRSdata().IsSaturated()) {
    ierr = VecSet(SolnTypInvV,1); CHKPETSC(ierr);
  }
  else {
    ierr = layout.MFTowerToVec(SolnTypInvV,PCapParamsMFT,2); CHKPETSC(ierr); // sigma = 1/P_typ
  }

  if (nlsc.scale_soln_before_solve) {
      ierr = VecPointwiseMult(SolnV,SolnV,SolnTypInvV); // Mult(w,x,y): w=x.y  -- Scale IC
  }
  ierr = VecCopy(SolnTypInvV,SolnTypV); // Copy(x,y): y <- x
  ierr = VecReciprocal(SolnTypV); // Create vec to unscale as needed

  SetTime(cur_time);
  SetDt(delta_t);

  if (rs_data.variable_switch_saturation_threshold>0) {
      ierr = SNESLineSearchSetPostCheck(snes,PostCheckAlt,(void *)(&check_ctx));CHKPETSC(ierr);
  }
  else {
      ierr = SNESLineSearchSetPostCheck(snes,PostCheck,(void *)(&check_ctx));CHKPETSC(ierr);  
  }
  ierr = SNESSetConvergenceTest(snes,Richard_SNESConverged,(void*)(&check_ctx),PETSC_NULL); CHKPETSC(ierr);

  UnsetRemainingJacobianReuses();

  // set dependent data
  rs_data.FillStateBndry(GetPressureN(),prev_time);
  if (!rs_data.IsSaturated()) {
    rs_data.calcInvPressure(GetRhoSatN(),GetPressureN());
  }

  // Evaluate the function
  PetscErrorCode (*func)(SNES,Vec,Vec,void*);
  void *fctx;
  ierr = SNESGetFunction(snes,PETSC_NULL,&func,&fctx);
  ierr = (*func)(snes,SolnV,RhsV,fctx); CHKPETSC(ierr);
  ierr = VecNorm(RhsV,NORM_2,&norm0); CHKPETSC(ierr); // Save initial norm FIXME: This is already done internally, better to access that

  RichardSolver::SetTheRichardSolver(this);
  dump_cnt = 0;
  nlsc.ls_success = true;
  ierr = SNESSolve(snes,PETSC_NULL,SolnV);// CHKPETSC(ierr);
  RichardSolver::SetTheRichardSolver(0);

  int iters;
  ierr = SNESGetIterationNumber(snes,&iters);CHKPETSC(ierr);
  nlsc.SetNLIterationsTaken(iters);

  SNESConvergedReason reason;
  ierr = SNESGetConvergedReason(snes,&reason); CHKPETSC(ierr);

  if (nlsc.scale_soln_before_solve) {
      ierr = VecPointwiseMult(SolnV,SolnV,GetSolnTypV()); // Unscale current candidate solution
  }

  if (reason <= 0) {
      return reason;
  }

  // Copy solution from Vec back into state
  ierr = layout.VecToMFTower(SolnMFT,SolnV,0); CHKPETSC(ierr);

  return reason;
}

#undef __FUNCT__  
#define __FUNCT__ "BuildOpSkel"
void
RichardSolver::BuildOpSkel(Mat& J)
{
  int num_rows = 1;
  int rows[1]; // At the moment, only set one row at a time
  Array<Real> vals;
  Array<int> cols;
  
  Layout& layout = GetLayout();
  const Array<Geometry>& geomArray = layout.GeomArray();
  const Array<BoxArray>& gridArray = layout.GridArray();
  const Array<IntVect>& refRatio = layout.RefRatio();
  const PArray<Layout::MultiNodeFab>& nodes = layout.Nodes();
  const PArray<Layout::MultiIntFab>& nodeIds = layout.NodeIds();
  const Array<BoxArray>& bndryCells = layout.BndryCells();
  const Array<Array<IVSMap> >& growCellStencil = mftfp->GrowCellStencil();
  int nLevs = layout.NumLevels();
  
  PetscErrorCode ierr;
  int num_nbrs_reg = 2*BL_SPACEDIM+1;
  Layout::IntFab reg_neighbors;
  std::set<int> neighbors;
  typedef BaseFab<std::set<int> > ISetFab;
  typedef FabArray<ISetFab> MultiSetFab;
  PArray<MultiSetFab> crseContribs(nLevs,PArrayManage);
  
  int myproc = ParallelDescriptor::MyProc();
  int numprocs = ParallelDescriptor::NProcs();
  
  for (int lev=nLevs-1; lev>=0; --lev) 
    {
      const Array<IVSMap>& growCellStencilLev = growCellStencil[lev];
      const Layout::MultiNodeFab& nodeLev = nodes[lev];
      const Layout::MultiIntFab& nodeIdsLev = nodeIds[lev];

      Layout::MultiIntFab crseIds; // coarse cell ids at fine grid, distributed per fine patches
      crseContribs.set(lev,new MultiSetFab);
      if (lev>0) {
	BoxArray bacg = BoxArray(gridArray[lev]).coarsen(refRatio[lev-1]).grow(1);
	crseIds.define(bacg,1,0,Fab_allocate);
            
	const Layout::MultiIntFab& crseIds_orig = nodeIds[lev-1]; // crse cells through periodic boundary
	BoxArray gcba = BoxArray(crseIds_orig.boxArray()).grow(crseIds_orig.nGrow());
	Layout::MultiIntFab tmp(gcba,1,0);
	for (MFIter mfi(crseIds_orig); mfi.isValid(); ++mfi) {
	  tmp[mfi].copy(crseIds_orig[mfi]); // NOTE: Assumes grow cells already filled
	}
	crseIds.copy(tmp); // Parallel copy

	crseContribs[lev].define(bacg,1,0,Fab_allocate);
      }

      std::map<IntVect,std::set<int>,IntVect::Compare> stencil;
      if (lev<nLevs-1) {
	// Pack up the crseContribs for a parallel copy
	const BoxArray& ba = gridArray[lev];
	MultiSetFab& crseContribsFine = crseContribs[lev+1];
        const DistributionMapping& dm = nodeLev.DistributionMap();
	std::map<int,Array<int> > ccArrays;
	for (MFIter mfi(crseContribsFine); mfi.isValid(); ++mfi) {
	  const ISetFab& ccFab = crseContribsFine[mfi];
	  const Box& vbox = mfi.validbox();
	  std::vector< std::pair<int,Box> > isects = ba.intersections(vbox);
	  for (int i=0; i<isects.size(); ++i) {
            int dst_proc = dm[isects[i].first];

            // HACK  This was originally written for parallel, but when I tried it in serial, the entire 
            // crseContribs structure was ignored!!  For now, set this up as a communication, even if 
            // serial...probably an easy logic issue to clear up....famous last words...
	    if (1 || dst_proc != myproc) {
	      for (IntVect iv(vbox.smallEnd()), iEnd=vbox.bigEnd(); iv<=iEnd; vbox.next(iv))
		{
		  const std::set<int>& ids = ccFab(iv,0);
		  int thisSize = ids.size();
		  if (thisSize) {
		    Array<int>& ints = ccArrays[dst_proc];
		    int old_cc_size = ints.size();
		    int delta_cc = BL_SPACEDIM + 1 + ids.size();
		    int new_cc_size = old_cc_size + delta_cc;

		    ints.resize(new_cc_size);
		    for (int d=0; d<BL_SPACEDIM; ++d) {
		      ints[old_cc_size+d] = iv[d];
		    }
		    ints[old_cc_size+BL_SPACEDIM] = ids.size();
		    int cnt=0;
		    for (std::set<int>::const_iterator it=ids.begin(), End=ids.end(); it!=End; ++it, ++cnt) {
		      ints[old_cc_size+BL_SPACEDIM+1+cnt] = *it;
		    }
		  }
		}
	    }
	  }
	}

	int total_num_to_send = 0;
	Array<int> sends(numprocs,0);
	Array<int> soffsets(numprocs,0);
	for (int i=0; i<numprocs; ++i) {
	  sends[i] = ccArrays[i].size();
	  total_num_to_send += sends[i];
	  if (i>0) {
	    soffsets[i] = soffsets[i-1] + ccArrays[i-1].size();
	  }
	}
	Array<int> sbuf(total_num_to_send);
	for (int i=0; i<numprocs; ++i) {
	  for (int j=0; j<ccArrays[i].size(); ++j) {
	    sbuf[soffsets[i] + j] = ccArrays[i][j];
	  }
	}

	Array<int> recvs(numprocs);
	BL_MPI_REQUIRE( MPI_Alltoall(sends.dataPtr(),
				     1,
				     ParallelDescriptor::Mpi_typemap<int>::type(),
				     recvs.dataPtr(),
				     1,
				     ParallelDescriptor::Mpi_typemap<int>::type(),
				     ParallelDescriptor::Communicator()) );
            
	int total_num_to_recv = 0;
	Array<int> roffsets(numprocs,0);
	for (int i=0; i<numprocs; ++i) {
	  total_num_to_recv += recvs[i];
	  if (i>0) {
	    roffsets[i] = roffsets[i-1] + recvs[i-1];
	  }
	}
	Array<int> rbuf(total_num_to_recv);
	BL_MPI_REQUIRE( MPI_Alltoallv(total_num_to_send == 0 ? 0 : sbuf.dataPtr(),
				      sends.dataPtr(),
				      soffsets.dataPtr(),
				      ParallelDescriptor::Mpi_typemap<int>::type(),
				      total_num_to_recv == 0 ? 0 : rbuf.dataPtr(),
				      recvs.dataPtr(),
				      roffsets.dataPtr(),
				      ParallelDescriptor::Mpi_typemap<int>::type(),
				      ParallelDescriptor::Communicator()) );
            
	for (int i=0; i<numprocs; ++i) {
	  int jcnt = roffsets[i];
	  while (jcnt < roffsets[i] + recvs[i]) {
	    IntVect iv(&(rbuf[jcnt]));
	    int size = rbuf[jcnt+BL_SPACEDIM];
	    std::set<int>& iset = stencil[iv];
	    for (int k=0; k<size; ++k) {
	      iset.insert(rbuf[jcnt+BL_SPACEDIM+1+k]);
	    }
	    jcnt += BL_SPACEDIM+1+size;
	  }
	}
      }

      for (MFIter mfi(nodeLev); mfi.isValid(); ++mfi) {
	const Layout::NodeFab& nodeFab = nodeLev[mfi];
	const Layout::IntFab& nodeIdFab = nodeIdsLev[mfi];
	const Layout::IntFab* crseIdFab = (lev>0  ?  &(crseIds[mfi])  : 0);
	const Box& vbox = mfi.validbox();
	Box gbox = Box(vbox).grow(1);

	for (IntVect iv(vbox.smallEnd()), iEnd=vbox.bigEnd(); iv<=iEnd; vbox.next(iv))
	  {
	    const Node& nC = nodeFab(iv,0);
	    if (nC.type==Node::VALID) {
	      rows[0] = nodeIdFab(iv,0);
	      neighbors.clear();

	      std::map<IntVect,std::set<int>,IntVect::Compare>::const_iterator sit=stencil.find(iv);
	      if (sit!=stencil.end()) {
		const std::set<int>& iset = sit->second;
		neighbors.insert(iset.begin(),iset.end());
	      }
	      neighbors.insert(rows[0]);

	      for (int d=0; d<BL_SPACEDIM; ++d) {
		for (int pm = -1; pm<2; pm+=2) {
		  std::set<int> nd;
		  IntVect ivA = iv  +  pm * BoxLib::BASISV(d);
		  IVScit it=growCellStencilLev[d].find(ivA);
		  if (it!=growCellStencilLev[d].end()) {
		    const Stencil& s = it->second;
		    for (Stencil::const_iterator it=s.begin(), End=s.end(); it!=End; ++it) {
		      const Node& node = it->first;
		      const IntVect& ivs = node.iv;
		      int slev = node.level;
		      if (slev==lev) {
			BL_ASSERT(nodeIdFab.box().contains(ivs));
			int idx = nodeIdFab(ivs,0);
			if (ivs != iv && idx>=0) { // idx<0 is Dirichlet data, iv added above
			  nd.insert(idx);
			}
		      }
		      else if (slev==lev-1) {
			BL_ASSERT(crseIdFab);
			BL_ASSERT(crseIdFab->box().contains(ivs));
			nd.insert((*crseIdFab)(ivs,0));
		      }
		      else {
			std::cout << "stencil: " << s << std::endl;
			BoxLib::Abort("Bad stencil");
		      }
		    }

		    // contribute to coarse cell stencil, if appropriate
		    const Node& offcenter_node = nodeFab(ivA,0);
		    if (offcenter_node.type==Node::VALID  &&  offcenter_node.level==lev-1) {
		      crseContribs[lev][mfi](offcenter_node.iv,0).insert(rows[0]);
		      crseContribs[lev][mfi](offcenter_node.iv,0).insert(nd.begin(),nd.end());
		    }
		  }
		  else {
		    int idx = nodeIdFab(ivA,0);
		    if (idx>=0) { // idx<0 is a covered cell
		      neighbors.insert(idx);
		    }
		  }

		  // Merge this arm into full set
		  neighbors.insert(nd.begin(),nd.end());

		}
	      }

	      int num_cols = -1;
	      if (nlsc.use_dense_Jacobian) 
		{
		  num_cols = layout.NumberOfGlobalNodeIds();
		  cols.resize(num_cols);
		  vals.resize(num_cols,0);
		  for (int i=0; i<num_cols; ++i) cols[i] = i;
		}
	      else
		{
		  num_cols = neighbors.size();
		  cols.resize(num_cols);
		  vals.resize(num_cols,0);
		  int cnt = 0;
		  for (std::set<int>::const_iterator it=neighbors.begin(), End=neighbors.end(); it!=End; ++it) {
		    cols[cnt++] = *it;
		  }
		}

	      ierr = MatSetValues(J,num_rows,rows,num_cols,cols.dataPtr(),vals.dataPtr(),INSERT_VALUES); CHKPETSC(ierr);
	    }
	  }
      }
    }

  ierr = MatAssemblyBegin(J,MAT_FINAL_ASSEMBLY); CHKPETSC(ierr);
  ierr = MatAssemblyEnd(J,MAT_FINAL_ASSEMBLY); CHKPETSC(ierr);
}

void
RichardSolver::CenterToEdgeUpwind(PArray<MFTower>&       mfte,
				  MFTower&               mftc,
				  const PArray<MFTower>& sgn,
				  int                    nComp,
                                  const BCRec&           bc) const
{
  int nLevs = rs_data.nLevs;
  for (int lev=0; lev<nLevs; ++lev) {
    MultiFab& clev = mftc[lev];
    BL_ASSERT(nComp<=clev.nComp());
    const Box& domain = GeomArray()[lev].Domain();
    for (MFIter mfi(clev); mfi.isValid(); ++mfi) {
      FArrayBox& cfab = clev[mfi];
      const Box& vccbox = mfi.validbox();
      for (int d=0; d<BL_SPACEDIM; ++d) {            
	FArrayBox& efab = mfte[d][lev][mfi];
	const FArrayBox& sgnfab = sgn[d][lev][mfi];
	BL_ASSERT(nComp<=efab.nComp());
	BL_ASSERT(nComp<=sgnfab.nComp());
	BL_ASSERT(Box(vccbox).surroundingNodes(d).contains(efab.box()));
	BL_ASSERT(Box(vccbox).surroundingNodes(d).contains(sgnfab.box()));
                
	int dir_bc_lo = bc.lo()[d]==EXT_DIR && vccbox.smallEnd()[d]==domain.smallEnd()[d];
	int dir_bc_hi = bc.hi()[d]==EXT_DIR && vccbox.bigEnd()[d]==domain.bigEnd()[d];

	int upwind_flag = (int)rs_data.upwind_krel;
	FORT_RS_CTE_UPW(efab.dataPtr(), ARLIM(efab.loVect()), ARLIM(efab.hiVect()),
			cfab.dataPtr(), ARLIM(cfab.loVect()), ARLIM(cfab.hiVect()),
			sgnfab.dataPtr(),ARLIM(sgnfab.loVect()), ARLIM(sgnfab.hiVect()),
			vccbox.loVect(), vccbox.hiVect(), &d, &nComp, &dir_bc_lo, &dir_bc_hi,
			&upwind_flag);
      }
    }
  }
}

void 
RichardSolver::XmultYZ(MFTower&       X,
		       const MFTower& Y,
		       const MFTower& Z,
		       int            sCompY,
		       int            sCompZ,
		       int            dComp,
		       int            nComp,
		       int            nGrow)
{
  Layout& layout = GetLayout();
  BL_ASSERT(layout.IsCompatible(X));
  BL_ASSERT(layout.IsCompatible(Y));
  BL_ASSERT(layout.IsCompatible(Z));
  BL_ASSERT(X.NComp()>=dComp+nComp);
  BL_ASSERT(Y.NComp()>=sCompY+nComp);
  BL_ASSERT(Z.NComp()>=sCompZ+nComp);
  BL_ASSERT(X.NGrow()>=nGrow);
  BL_ASSERT(Y.NGrow()>=nGrow);
  BL_ASSERT(Z.NGrow()>=nGrow);
  const Array<BoxArray>& gridArray = GridArray();
  const Array<Geometry>& geomArray = GeomArray();
  const Array<IntVect>& refRatio = RefRatio();

  FArrayBox tfabY, tfabZ;
  int nLevs = rs_data.nLevs;
  for (int lev=0; lev<nLevs; ++lev)
    {
      MultiFab& Xlev = X[lev];
      const MultiFab& Ylev = Y[lev];
      const MultiFab& Zlev = Z[lev];
      BoxArray fba;
      if (lev<nLevs-1) {
	fba = BoxArray(X[lev+1].boxArray()).coarsen(refRatio[lev]);
      }

      for (MFIter mfi(Y[lev]); mfi.isValid(); ++mfi)
        {
	  const Box& vbox = mfi.validbox();
	  Box gbox = Box(vbox).grow(nGrow);
	  FArrayBox& Xfab = Xlev[mfi];
	  const FArrayBox& Yfab = Ylev[mfi];
	  const FArrayBox& Zfab = Zlev[mfi];

	  tfabY.resize(gbox,nComp);
	  tfabZ.resize(gbox,nComp);
	  tfabY.copy(Yfab,sCompY,0,nComp);
	  tfabZ.copy(Zfab,sCompZ,0,nComp);

	  // Zero out parts of Y,Z covered by fine grid (to ensure valid data)
	  if (lev<nLevs-1) {
	    std::vector< std::pair<int,Box> > isects = fba.intersections(gbox);
	    for (int i = 0; i < isects.size(); i++)
	      {
		tfabY.setVal(0,isects[i].second,0,nComp);
		tfabZ.setVal(0,isects[i].second,0,nComp);
	      }
	  }

	  FORT_RS_XMULTYZ(Xfab.dataPtr(dComp),ARLIM(Xfab.loVect()), ARLIM(Xfab.hiVect()),
			  tfabY.dataPtr(),ARLIM(tfabY.loVect()), ARLIM(tfabY.hiVect()),
			  tfabZ.dataPtr(),ARLIM(tfabZ.loVect()), ARLIM(tfabZ.hiVect()),
			  gbox.loVect(), gbox.hiVect(), &nComp);
        }
    }
}



//
// Compute mfte[dir][comp] = Grad(mftc[comp]) + a[dir][comp]
//
void
RichardSolver::CCtoECgradAdd(PArray<MFTower>&            mfte,
                             const MFTower&              mftc,
                             const Array<Array<Real> >&  a,
                             int                         sComp,
                             int                         dComp,
                             int                         nComp) const
{
  const Layout& layout = GetLayout();
  for (int d=0; d<BL_SPACEDIM; ++d) {            
    BL_ASSERT(layout.IsCompatible(mfte[d]));
  }
  BL_ASSERT(layout.IsCompatible(mftc));

  const Array<Geometry>& geomArray = layout.GeomArray();
  int nLevs = rs_data.nLevs;
  for (int lev=0; lev<nLevs; ++lev) {
    const MultiFab& mfc = mftc[lev];
    BL_ASSERT(mfc.nGrow()>=1);
    BL_ASSERT(sComp+nComp<=mfc.nComp());
    const Real* dx = geomArray[lev].CellSize();

    for (MFIter mfi(mfc); mfi.isValid(); ++mfi) {
      const FArrayBox& cfab = mfc[mfi];
      const Box& vcbox = mfi.validbox();
            
      for (int d=0; d<BL_SPACEDIM; ++d) {            
	FArrayBox& efab = mfte[d][lev][mfi];
	BL_ASSERT(dComp+nComp<=efab.nComp());
	BL_ASSERT(Box(vcbox).surroundingNodes(d).contains(efab.box()));
	efab.setVal(0);
	FORT_RS_GXPA(efab.dataPtr(dComp),ARLIM(efab.loVect()), ARLIM(efab.hiVect()),
		     cfab.dataPtr(sComp),ARLIM(cfab.loVect()), ARLIM(cfab.hiVect()),
		     vcbox.loVect(),vcbox.hiVect(),dx,a[d].dataPtr(),&d,&nComp);
      }
    }
  }
}

void
RichardSolver::FillPatch(MFTower& mft,
			 int sComp,
			 int nComp,
			 bool do_piecewise_constant)
{
  int nLevs = rs_data.nLevs;
  mftfp->FillGrowCells(mft,sComp,nComp,do_piecewise_constant,nLevs);
}

void 
RichardSolver::ComputeDarcyVelocity(MFTower& pressure,
                                    Real     t)
{
  // On Dirichlet boundaries, the grow cells of pressure will hold the value to apply at
  // the cell wall.  Note that since we use "calcInvPressure" to fill rho.sat, these
  // are then values on the wall as well.  As a result, the lambda values computed
  // with rho.sat are evaluated at the wall as well.  
  
  // We use the FillPatch operation to set pressure values in the grow cells using 
  // polynomial extrapolation, and will then use these p values only for the puposes
  // of evaluating the pressure gradient on cell faces via a simple centered difference.
  
  // Assumes lev=0 here corresponds to Amr.level=0, sets dirichlet values of rho.sat and
  // lambda on dirichlet pressure faces

  PArray<MFTower>& darcy_vel = GetDarcyVelocity();
  MFTower& rhoSat = GetRhoSatNp1();
  MFTower& lambda = GetLambda();

  const Array<Real>& rho = GetDensity();
  const Array<Real>& gravity = GetGravity();

  int nComp = 1;
  Array<Array<Real> > rhog(BL_SPACEDIM,Array<Real>(nComp));
  for (int n=0; n<nComp; ++n) {
    for (int d=0; d<BL_SPACEDIM; ++d) {
      rhog[d][n] = rho[n] * gravity[d];
    }
  }

  int nLevs = rs_data.nLevs;

  rs_data.FillStateBndry(pressure,t); // Set new boundary data
  if (!rs_data.IsSaturated()) {
    rs_data.calcInvPressure(rhoSat,pressure);
  }

  // Convert grow cells of pressure into extrapolated values so that from here on out,
  // the values are only used to compute gradients at faces.
  bool do_piecewise_constant = false;
  FillPatch(pressure,0,nComp,do_piecewise_constant);
  
  // Get  -(Grad(p) + rho.g)
  CCtoECgradAdd(darcy_vel,pressure,rhog);

  if (rs_data.upwind_krel) {

    rs_data.calcLambda(lambda,rhoSat); // FIXME: Writes/reads only to comp=0, does 1 grow

    // Get edge-centered lambda (= krel/mu) based on the sign of -(Grad(p) + rho.g)
    const BCRec& pressure_bc = rs_data.pressure_bc;
    CenterToEdgeUpwind(GetRichardCoefs(),lambda,darcy_vel,nComp,pressure_bc);
 
    // Get Darcy velocity = - lambda * kappa * (Grad(p) + rho.g)
    const PArray<MFTower>& kappaEC = GetKappaEC(t);
    for (int d=0; d<BL_SPACEDIM; ++d) {
      XmultYZ(darcy_vel[d],GetRichardCoefs()[d],kappaEC[d]);
    }
  }
  else {

    MFTower& CoeffCC = GetCoeffCC();
    if (rs_data.IsSaturated()) {
      for (int lev=0; lev<nLevs; ++lev) {
	CoeffCC[lev].setVal(1/rs_data.GetViscosity()[0],0,BL_SPACEDIM,1);
      }
    }
    else {
      rs_data.calcLambda(lambda,rhoSat);
      for (int lev=0; lev<nLevs; ++lev) {
	MultiFab::Copy(CoeffCC[lev],lambda[lev],0,0,1,1);
	for (int d=1; d<BL_SPACEDIM; ++d) {
	  MultiFab::Copy(CoeffCC[lev],CoeffCC[lev],0,d,1,1);
	}
      }
      
      if (rs_data.subgrid_krel) {
	BoxLib::Abort("Subgrid Krel not ready for primetime");
#if 0
	const Array<IntVect>& refRatio = layout.RefRatio();
	const Array<BoxArray>& gridArray = layout.GridArray();
	const Array<Geometry>& geomArray = layout.GeomArray();
	MatFiller* matFiller = pm_amr.GetMatFiller();
	bool ret = matFiller != 0 && matFiller->Initialized();
	if (!ret) {
	  BoxLib::Abort("RichardSolver:: matFiller not ready");
	}
	int num_levs_mixed = matFiller->NumLevels();
	int num_fill = state_to_fill.size();
	
	for (int lev=1; lev<num_fill; ++lev) {
	  if (state_to_fill[lev].size()>0) {
	    
	    pm[lev].FillCoarsePatch(pf[lev],0,t,Press_Type,0,1);
	    pf[lev].mult(-1);
	    
	    FArrayBox rsf;
	    for (MFIter mfi(pf[lev]); mfi.isValid(); ++mfi) {
	      FArrayBox&         lamf = lf[lev][mfi];
	      const FArrayBox&   pfab = pf[lev][mfi];
	      const FArrayBox& phifab = phif[lev][mfi];
	      const FArrayBox&   kfab = kf[lev][mfi];
	      const FArrayBox& pcPfab = pcPf[lev][mfi];
	      const FArrayBox&  krfab = krf[lev][mfi];
	      int ncKr  = krfab.nComp();
	      int ncPcP = pcPfab.nComp();
	      rsf.resize(pfab.box(),1);
	      
	      PorousMedia::calcInvCapillary(rsf, pfab, phifab, kfab, pcPfab);
	      PorousMedia::calcLambda(lamf, rsf, krfab);
	    }
	  }
	}
	
	// Average down, insert into CoeffCC
	int num_derive = derive_to_fill.size();
	for (int lev=num_derive-2; lev>=0; --lev) {
	  if (derive_to_fill[lev].size()>0) {
	    const IntVect& crat = matFiller->RefRatio(lev);
	    const BoxArray& cba = matFiller->Mixed(lev);
	    BoxArray fcba = BoxArray(cba).refine(crat);
	    MultiFab tlc(cba,BL_SPACEDIM,0);
	    MultiFab tlf(fcba,BL_SPACEDIM,0);
	    tlf.setVal(-1);
	    tlf.copy(lf[lev+1],0,0,1);
	    for (int d=1; d<BL_SPACEDIM; ++d) {
	      tlf.copy(tlf,0,d,1);
	    }
	    if (lev<num_derive-2) {
	      tlf.copy(lc[lev+1],0,0,BL_SPACEDIM);
	    }
	    for (MFIter mfi(tlc); mfi.isValid(); ++mfi) {
	      const Box& crse_box = mfi.validbox();
	      const Box fine_box = Box(crse_box).refine(crat);
	      
	      matFiller->CoarsenData(tlf[mfi],0,tlc[mfi],crse_box,0,BL_SPACEDIM,crat,
				     matFiller->coarsenRule("relative_permeability"));
	    }
	    lc[lev].copy(tlc,0,0,BL_SPACEDIM);
	    if (lev>0) {
	      for (int d=0; d<BL_SPACEDIM; ++d) {
		lc[lev].copy(lf[lev],0,d,1);
	      }
	    }
	    if (lev<nLevs) {
	      CoeffCC[lev].copy(lc[lev],0,0,BL_SPACEDIM);
	    }
	  }
	}
#endif
      }

      // Make sure grow cells are consistent
      for (int lev=0; lev<nLevs; ++lev) {
	CoeffCC[lev].FillBoundary(0,BL_SPACEDIM);
	rs_data.layout.GeomArray()[lev].FillPeriodicBoundary(CoeffCC[lev],0,BL_SPACEDIM);
      }
    }

    // Get (lambda*kappa) at cell centers
    const MFTower* KappaCCdir = GetKappaCCdir(t);
    for (int lev=0; lev<nLevs; ++lev) {
      MultiFab::Multiply(CoeffCC[lev],(*KappaCCdir)[lev],0,0,BL_SPACEDIM,1);
    }

    int do_harmonic = 1;
    int nComp = -1; // Note signal to take multiple components of cc to single comp of ec
    MFTower::CCtoECavg(GetRichardCoefs(),CoeffCC,1.0,0,0,nComp,do_harmonic);
    
    for (int lev=0; lev<nLevs; ++lev) {
      for (int d=0; d<BL_SPACEDIM; ++d) {
	MultiFab::Multiply(darcy_vel[d][lev],GetRichardCoefs()[d][lev],0,0,1,0);
      }
    }
  }
  
  // Overwrite face velocities at boundary with boundary conditions
  rs_data.SetInflowVelocity(darcy_vel,t);

  // Average down velocities
  int sComp = 0;
  for (int d=0; d<BL_SPACEDIM; ++d) {
    MFTower::AverageDown(darcy_vel[d],sComp,nComp,nLevs);
  }
}

Real TotalVolume()
{
    const RealBox& rb = Geometry::ProbDomain();
    Real vol = 1;
    for (int d=0; d<BL_SPACEDIM; ++d) {
        vol *= rb.length(d);
    }
    return vol;
}

void
RichardSolver::DivRhoU(MFTower& DivRhoU,
                       MFTower& pressure,
                       Real     t)
{
  // Get the Darcy flux
  ComputeDarcyVelocity(pressure,t);

  // Get the divergence of the Darcy velocity flux = darcy vel . rho 
  //   leave velocity unscaled
  int sComp=0;
  int dComp=0;
  int nComp=1;
  MFTower::ECtoCCdiv(DivRhoU,GetDarcyVelocity(),GetDensity(),sComp,dComp,nComp,rs_data.nLevs);
}

void
RichardSolver::CalcResidual(MFTower& residual,
			    MFTower& pressure,
			    Real     t,
			    Real     dt)
{
  DivRhoU(residual,pressure,t);

  if (dt>0) {
    int sComp=0;
    int dComp=0;
    int nComp=1;
    int nGrow=0;

    Real gInv = 0;
    const Array<Real>& g = rs_data.GetGravity();
    for (int d=0; d<g.size(); ++d) {
      gInv += g[d]*g[d];
    }
    if (gInv != 0) {
      gInv = 1/std::sqrt(gInv);
    }

    Layout& layout = GetLayout();
    const Array<BoxArray>& gridArray = layout.GridArray();
    const Array<IntVect>& refRatio = layout.RefRatio();
    FArrayBox source, st;
    int nLevs = rs_data.nLevs;
    for (int lev=0; lev<nLevs; ++lev) {
      MultiFab& Rlev = residual[lev];
      for (MFIter mfi(Rlev); mfi.isValid(); ++mfi) {
	const Box& vbox = mfi.validbox();
	FArrayBox& Res = Rlev[mfi];
	const FArrayBox& phi = GetPorosity()[lev][mfi];
        const FArrayBox& sfab = (*rs_data.GetSource(t))[lev][mfi];

	if (rs_data.IsSaturated()) {
	  const FArrayBox& p_n = GetPressureN()[lev][mfi];
	  const FArrayBox& p_np1 = pressure[lev][mfi];
	  const FArrayBox& ss = GetSpecificStorage()[lev][mfi];
	  st.resize(vbox,1);
	  st.copy(ss);
	  st.mult(gInv);
	  FORT_RS_SATURATEDRES(Res.dataPtr(),   ARLIM(Res.loVect()),   ARLIM(Res.hiVect()),
			       p_n.dataPtr(),   ARLIM(p_n.loVect()),   ARLIM(p_n.hiVect()),
			       p_np1.dataPtr(), ARLIM(p_np1.loVect()), ARLIM(p_np1.hiVect()),
			       st.dataPtr(),    ARLIM(st.loVect()),    ARLIM(st.hiVect()),
			       phi.dataPtr(),   ARLIM(phi.loVect()),   ARLIM(phi.hiVect()),
			       sfab.dataPtr(),  ARLIM(sfab.loVect()),  ARLIM(sfab.hiVect()),
			       &dt, vbox.loVect(), vbox.hiVect(), &nComp);
	} else {
	  const FArrayBox& rs_n = GetRhoSatN()[lev][mfi];
	  const FArrayBox& rs_np1 = GetRhoSatNp1()[lev][mfi];
	  FORT_RS_RICHARDRES(Res.dataPtr(),    ARLIM(Res.loVect()),    ARLIM(Res.hiVect()),
			     rs_n.dataPtr(),   ARLIM(rs_n.loVect()),   ARLIM(rs_n.hiVect()),
			     rs_np1.dataPtr(), ARLIM(rs_np1.loVect()), ARLIM(rs_np1.hiVect()),
			     phi.dataPtr(),    ARLIM(phi.loVect()),    ARLIM(phi.hiVect()),
			     sfab.dataPtr(),  ARLIM(sfab.loVect()),    ARLIM(sfab.hiVect()),
			     &dt, vbox.loVect(), vbox.hiVect(), &nComp);
	}
      }
    }
  }
}

#undef __FUNCT__  
#define __FUNCT__ "CreatJac"
void RichardSolver::CreateJac(Mat& J, 
			      MFTower& pressure,
                              Real t,
			      Real dt)
{
  Layout& layout = GetLayout();
  const Array<BoxArray>& gridArray = layout.GridArray();
  const Array<IntVect>& refRatio   = layout.RefRatio();
  BaseFab<int> nodeNums;
  PetscErrorCode ierr;
  const BCRec& theBC = rs_data.pressure_bc;
  int nLevs = rs_data.nLevs;
  PArray<MultiFab> kr_rs_data(nLevs,PArrayNoManage);
  
  for (int lev=0; lev<nLevs; ++lev) {
    kr_rs_data.set(lev, &(GetKrParams()[lev]));
  }
  MFTower& PCapParamsaMFT = GetPCapParams();
  MFTower KrParamsMFT(layout,kr_rs_data,nLevs);

  const Array<int>& rinflow_bc_lo = rs_data.rinflowBCLo();
  const Array<int>& rinflow_bc_hi = rs_data.rinflowBCHi();

  // may not necessary since this should be same as the residual
  rs_data.calcInvPressure(GetRhoSatNp1(),pressure); 
  rs_data.calcLambda(GetLambda(),GetRhoSatNp1()); 

  int do_upwind = (int)rs_data.upwind_krel;
  for (int lev=0; lev<nLevs; ++lev) {
    const Box& domain = GeomArray()[lev].Domain();
    const Real* dx = GeomArray()[lev].CellSize();
    MultiFab& Plev = pressure[lev];

    PArray<MultiFab> jacflux;
    jacflux.resize(BL_SPACEDIM,PArrayManage);
    for (int d=0; d<BL_SPACEDIM; ++d) {
      BoxArray ba = BoxArray(pressure[lev].boxArray()).surroundingNodes(d);
      jacflux.set(d,new MultiFab(ba,3,0));
    }

    for (MFIter mfi(Plev); mfi.isValid(); ++mfi) {
      const Box& vbox = mfi.validbox();
      const int idx   = mfi.index();
      const int* bc   = rs_data.pressure_bc.vect();

      Box gbox = Box(vbox).grow(1);
      nodeNums.resize(gbox,1);
      layout.SetNodeIds(nodeNums,lev,idx);

      // reusing RichardCoefs to store Jacobian flux term
      FArrayBox& jfabx = jacflux[0][mfi];
      FArrayBox& jfaby = jacflux[1][mfi];
      FArrayBox& vfabx = GetDarcyVelocity()[0][lev][mfi];
      FArrayBox& vfaby = GetDarcyVelocity()[1][lev][mfi];
      const FArrayBox& kfabx = GetKappaEC(t)[0][lev][mfi];
      const FArrayBox& kfaby = GetKappaEC(t)[1][lev][mfi];
      
#if (BL_SPACEDIM==3)
      FArrayBox& jfabz = jacflux[2][mfi];
      FArrayBox& vfabz = GetDarcyVelocity()[2][lev][mfi];
      const FArrayBox& kfabz = GetKappaEC(t)[2][lev][mfi];
#endif
      FArrayBox& ldfab = GetLambda()[lev][mfi];
      FArrayBox& prfab = pressure[lev][mfi];
      FArrayBox& pofab = GetPorosity()[lev][mfi];
      FArrayBox& kcfab = GetKappaCCavg()[lev][mfi];
      FArrayBox& cpfab = GetPCapParams()[lev][mfi];
      const int n_cp_coef = cpfab.nComp();
      FArrayBox& krfab = KrParamsMFT[lev][mfi];
      const int n_kr_coef = krfab.nComp();
      Real deps = 1.e-8;

      FORT_RICHARD_NJAC2(jfabx.dataPtr(), ARLIM(jfabx.loVect()),ARLIM(jfabx.hiVect()),
			 jfaby.dataPtr(), ARLIM(jfaby.loVect()),ARLIM(jfaby.hiVect()),

#if(BL_SPACEDIM==3)
			 jfabz.dataPtr(), ARLIM(jfabz.loVect()),ARLIM(jfabz.hiVect()),
#endif	
			 vfabx.dataPtr(), ARLIM(vfabx.loVect()),ARLIM(vfabx.hiVect()),
			 vfaby.dataPtr(), ARLIM(vfaby.loVect()),ARLIM(vfaby.hiVect()),
#if(BL_SPACEDIM==3)
			 vfabz.dataPtr(), ARLIM(vfabz.loVect()),ARLIM(vfabz.hiVect()),
#endif
			 kfabx.dataPtr(), ARLIM(kfabx.loVect()),ARLIM(kfabx.hiVect()),
			 kfaby.dataPtr(), ARLIM(kfaby.loVect()),ARLIM(kfaby.hiVect()),
#if(BL_SPACEDIM==3)
			 kfabz.dataPtr(), ARLIM(kfabz.loVect()),ARLIM(kfabz.hiVect()),  
#endif
			 ldfab.dataPtr(), ARLIM(ldfab.loVect()),ARLIM(ldfab.hiVect()),
			 
			 prfab.dataPtr(), ARLIM(prfab.loVect()),ARLIM(prfab.hiVect()),
			 pofab.dataPtr(), ARLIM(pofab.loVect()),ARLIM(pofab.hiVect()),
			 kcfab.dataPtr(), ARLIM(kcfab.loVect()),ARLIM(kcfab.hiVect()),
			 krfab.dataPtr(), ARLIM(krfab.loVect()),ARLIM(krfab.hiVect()), &n_kr_coef,
			 cpfab.dataPtr(), ARLIM(cpfab.loVect()),ARLIM(cpfab.hiVect()), &n_cp_coef,
			 vbox.loVect(), vbox.hiVect(), domain.loVect(), domain.hiVect(), 
			 dx, bc, 
			 rinflow_bc_lo.dataPtr(),rinflow_bc_hi.dataPtr(), 
			 &deps, &do_upwind);


      FArrayBox dalpha(gbox,1);
      FArrayBox& nfab = GetRhoSatNp1()[lev][mfi];
      
      FORT_RICHARD_ALPHA(dalpha.dataPtr(), ARLIM(dalpha.loVect()), ARLIM(dalpha.hiVect()),
			 nfab.dataPtr(), ARLIM(nfab.loVect()),ARLIM(nfab.hiVect()),
			 pofab.dataPtr(), ARLIM(pofab.loVect()),ARLIM(pofab.hiVect()),
			 kcfab.dataPtr(), ARLIM(kcfab.loVect()), ARLIM(kcfab.hiVect()),
			 cpfab.dataPtr(), ARLIM(cpfab.loVect()), ARLIM(cpfab.hiVect()), &n_cp_coef,
			 vbox.loVect(), vbox.hiVect());
      
      Array<int> cols(1+2*BL_SPACEDIM);
      Array<int> rows(1);
      Array<Real> vals(cols.size(),0);

      const Array<double>& rho = GetDensity();
      int nc = 0;

      for (IntVect iv(vbox.smallEnd()), iEnd=vbox.bigEnd(); iv<=iEnd; vbox.next(iv))
      {
	  cols[0] = nodeNums(iv,0);
          if (cols[0]>=0) {
              rows[0] = cols[0];
              vals[0] = dalpha(iv,0);
              Real rdt = (dt>0  ?  rho[nc]*dt : 1); // The "b" factor
              int cnt = 1;
              for (int d=0; d<BL_SPACEDIM; ++d) {
                  vals[0] -= rdt * jacflux[d][mfi](iv,2);
                  IntVect ivp = iv + BoxLib::BASISV(d);
                  int np = nodeNums(ivp,0);
                  if (np>=0) {
                      cols[cnt]  = np; 
                      vals[cnt]  = -rdt * jacflux[d][mfi](iv,0);
                      cnt++;
                  }
                  else {
                      if (theBC.hi()[d]==FOEXTRAP) {
                          vals[0] -= rdt * jacflux[d][mfi](iv,0);
                      }
                  }
                  
                  IntVect ivn = iv - BoxLib::BASISV(d);
                  int nn = nodeNums(ivn,0);
                  if (nn>=0) {
                      cols[cnt]  = nn; 
                      vals[cnt]  = -rdt * jacflux[d][mfi](iv,1);
                      cnt++;
                  }
                  else {
                      if (theBC.lo()[d]==FOEXTRAP) {
                          vals[0] -= rdt * jacflux[d][mfi](iv,1);
                      }
                  }
              }
              ierr = MatSetValues(J,rows.size(),rows.dataPtr(),cnt,cols.dataPtr(),vals.dataPtr(),INSERT_VALUES); CHKPETSC(ierr);
	  }
      }
    }
  }
  ierr = MatAssemblyBegin(J,MAT_FINAL_ASSEMBLY); CHKPETSC(ierr);
  ierr = MatAssemblyEnd(J,MAT_FINAL_ASSEMBLY); CHKPETSC(ierr);
}

#undef __FUNCT__  
#define __FUNCT__ "RichardRes_DpDt"
PetscErrorCode 
RichardRes_DpDt(SNES snes,Vec x,Vec f,void *dummy)
{
    PetscErrorCode ierr; 
    RichardSolver* rs = static_cast<RichardSolver*>(dummy);
    if (!rs) {
        BoxLib::Abort("Bad cast in RichardRes_DpDt");
    }

    if (rs->GetNLScontrol().scale_soln_before_solve) {
        ierr = VecPointwiseMult(x,x,rs->GetSolnTypV()); CHKPETSC(ierr); // Unscale solution
    }

    MFTower& xMFT = rs->GetPressureNp1();
    MFTower& fMFT = rs->GetResidual();

    Layout& layout = rs->GetLayout();
    ierr = layout.VecToMFTower(xMFT,x,0); CHKPETSC(ierr);

    Real t = rs->GetTime();
    Real dt = rs->GetDt();
    rs->CalcResidual(fMFT,xMFT,t,dt);

#if 0
    // Scale residual by cell volume/sqrt(total volume)
    Real sqrt_total_volume_inv = std::sqrt(1/TotalVolume());
    int nComp = 1;
    int nLevs = rs->GetNumLevels();
    for (int lev=0; lev<nLevs; ++lev)
    {
      MultiFab::Multiply(fMFT[lev],layout.Volume(lev),0,0,nComp,0);
      fMFT[lev].mult(sqrt_total_volume_inv,0,1);
    }
#endif

    ierr = layout.MFTowerToVec(f,fMFT,0); CHKPETSC(ierr);

    if (rs->GetNLScontrol().scale_soln_before_solve) {
        ierr = VecPointwiseMult(x,x,rs->GetSolnTypInvV()); CHKPETSC(ierr); // Reset solution scaling
    }
    PetscFunctionReturn(0);
}

#undef __FUNCT__  
#define __FUNCT__ "RichardR2"
PetscErrorCode 
RichardR2(SNES snes,Vec x,Vec f,void *dummy)
{
    PetscErrorCode ierr; 
    RichardSolver* rs = static_cast<RichardSolver*>(dummy);
    if (!rs) {
        BoxLib::Abort("Bad cast in RichardR2");
    }

    if (rs->GetNLScontrol().scale_soln_before_solve) {
        ierr = VecPointwiseMult(x,x,rs->GetSolnTypV()); CHKPETSC(ierr); // Unscale solution
    }

    MFTower& xMFT = rs->GetPressureNp1();
    MFTower& fMFT = rs->GetResidual();

    Layout& layout = rs->GetLayout();
    ierr = layout.VecToMFTower(xMFT,x,0); CHKPETSC(ierr);

    Real t = rs->GetTime();
    rs->DivRhoU(fMFT,xMFT,t);

    ierr = layout.MFTowerToVec(f,fMFT,0); CHKPETSC(ierr);

    if (rs->GetNLScontrol().scale_soln_before_solve) {
        ierr = VecPointwiseMult(x,x,rs->GetSolnTypInvV()); CHKPETSC(ierr); // Reset solution scaling
    }
    PetscFunctionReturn(0);
}

#if defined(PETSC_3_2)

#undef __FUNCT__  
#define __FUNCT__ "RichardJacFromPM"
PetscErrorCode 
RichardJacFromPM(SNES snes, Vec x, Mat* jac, Mat* jacpre, MatStructure* flag, void *dummy)
{
  PetscErrorCode ierr;
  RichardSolver* rs = static_cast<RichardSolver*>(dummy);
  if (!rs) {
    BoxLib::Abort("Bad cast in RichardJacFromPM");
  }
  MFTower& xMFT = rs->GetPressureNp1();
  
  Layout& layout = rs->GetLayout();
  ierr = layout.VecToMFTower(xMFT,x,0); CHKPETSC(ierr);
  Real dt = rs->GetDt();
  Real t = rs->GetTime();
  rs->CreateJac(*jacpre,xMFT,t,dt);
  if (*jac != *jacpre) {
    ierr = MatAssemblyBegin(*jac,MAT_FINAL_ASSEMBLY);CHKPETSC(ierr);
    ierr = MatAssemblyEnd(*jac,MAT_FINAL_ASSEMBLY);CHKPETSC(ierr);
  }
  PetscFunctionReturn(0);
} 

#undef __FUNCT__  
#define __FUNCT__ "RecordSolve"
void RecordSolve(Vec& p,Vec& dp,Vec& dp_orig,Vec& pnew,Vec& F,Vec& G,CheckCtx* check_ctx)
{
  RichardSolver* rs = check_ctx->rs;
  const std::string& record_file = rs->GetRecordFile();
  BL_ASSERT(!record_file.empty());
  Layout& layout = check_ctx->rs->GetLayout();
  int nLevs = rs->GetNumLevels();

  int num_out = 9;
  Array<MFTower*> dMFT(num_out);
  Array<std::string> names(num_out);

  PetscErrorCode ierr;
  for (int i=0; i<num_out; ++i) {
    dMFT[i] = new MFTower(layout,IndexType(IntVect::TheZeroVector()),1,1,nLevs);
  }

  MFTower& ResMFT     = *(dMFT[0]);
  MFTower& DpMFT      = *(dMFT[1]);
  MFTower& Dp_origMFT = *(dMFT[2]);
  MFTower& PoldMFT    = *(dMFT[3]);
  MFTower& PnewMFT    = *(dMFT[4]);
  MFTower& SnewMFT    = *(dMFT[5]);
  MFTower& SoldMFT    = *(dMFT[6]);
  MFTower& DsMFT      = *(dMFT[7]);
  MFTower& fMFT       = *(dMFT[8]);

  ierr = layout.VecToMFTower(    ResMFT,      G,0); CHKPETSC(ierr);
  ierr = layout.VecToMFTower(     DpMFT,     dp,0); CHKPETSC(ierr);
  ierr = layout.VecToMFTower(Dp_origMFT,dp_orig,0); CHKPETSC(ierr);
  ierr = layout.VecToMFTower(   PoldMFT,      p,0); CHKPETSC(ierr);
  ierr = layout.VecToMFTower(   PnewMFT,   pnew,0); CHKPETSC(ierr);

  Real cur_time = rs->GetTime();
  Real dt = rs->GetDt();
  Real rho = rs->GetDensity()[0];

  Real junk_val = -1.e20;
  Dp_origMFT.SetValCovered(junk_val);

  RSdata& rs_data = rs->GetRSdata();

  rs_data.FillStateBndry(PnewMFT,cur_time);
  rs_data.FillStateBndry(PoldMFT,cur_time-dt);
  rs_data.calcInvPressure(SnewMFT,PnewMFT);
  rs_data.calcInvPressure(SoldMFT,PoldMFT);

  for (int lev=0; lev<nLevs; ++lev) {
    SnewMFT[lev].mult(1/rho,0,1);
    SoldMFT[lev].mult(1/rho,0,1);

    MultiFab::Copy(DsMFT[lev],SnewMFT[lev],0,0,1,0);
    MultiFab::Subtract(DsMFT[lev],SoldMFT[lev],0,0,1,0);

    for (MFIter mfi(fMFT[lev]); mfi.isValid(); ++mfi) {
      const Box& box = mfi.validbox();
      for (IntVect iv=box.smallEnd(), End=box.bigEnd(); iv<=End; box.next(iv)) {
	const Real& num = DpMFT[lev][mfi](iv,0);
	const Real& den = Dp_origMFT[lev][mfi](iv,0);
	fMFT[lev][mfi](iv,0) = den==junk_val ? 1 : std::abs(num/den);
      }
    }
  }

  for (int i=0; i<num_out; ++i) {
    dMFT[i]->SetValCovered(0);
  }

  names[0] = "Res_undamped";
  names[1] = "Dp_damped";
  names[2] = "Dp_undamped";
  names[3] = "Pold";
  names[4] = "Pnew_damped";
  names[5] = "Snew_damped";
  names[6] = "Sold";
  names[7] = "dS";
  names[8] = "DampingFactor";

  int timestep = rs->GetCurrentTimestep();
  std::string step_file = BoxLib::Concatenate(record_file + "/Step_",timestep,3);
  step_file = BoxLib::Concatenate(step_file + "/iteration_",dump_cnt,3);

  if (ParallelDescriptor::IOProcessor()) {
    std::cout << "****************** Writing file: " << step_file << std::endl;
  }
  Real time = 0;
  MFTower::WriteSet(step_file,dMFT,names,time);
  dump_cnt++;
}

/*
   PostCheck - User-defined routine that checks the validity of
   candidate steps of a line search method.  Set by SNESLineSearchSetPostCheck().
   In:
   snes 	- nonlinear context
   checkctx 	- optional user-defined context for use by step checking routine
   x     	- previous iterate
   y 	        - new search direction and length
   w 	        - current candidate iterate
   
   Out:
   y            - search direction (possibly changed)
   w            - current iterate (possibly modified)
   changed_y 	- indicates search direction was changed by this routine
   changed_w 	- indicates current iterate was changed by this routine 

 */

#undef __FUNCT__  
#define __FUNCT__ "PostCheck"
PetscErrorCode 
PostCheck(SNES snes,Vec x,Vec y,Vec w,void *ctx,PetscBool  *changed_y,PetscBool  *changed_w)
{
    std::string tag = "       Newton step: ";
    std::string tag_ls = "  line-search:  ";
    CheckCtx* check_ctx = (CheckCtx*)ctx;
    RichardSolver* rs = check_ctx->rs;
    NLScontrol* nlsc = check_ctx->nlsc;

    if (rs==0) {
        BoxLib::Abort("Context cast failed in PostCheck");
    }

    nlsc->ls_success = true;
    nlsc->ls_reason = "In Progress";

    PetscErrorCode ierr;
    PetscReal fnorm, xnorm, ynorm, gnorm;

    PetscErrorCode (*func)(SNES,Vec,Vec,void*);
    void *fctx;

    ierr = SNESGetFunction(snes,PETSC_NULL,&func,&fctx); CHKPETSC(ierr);

    Vec& F = rs->GetResidualV();
    Vec& G = rs->GetTrialResV();
    
    ierr = (*func)(snes,x,F,fctx); CHKPETSC(ierr);
    ierr = VecNorm(F,NORM_2,&fnorm); CHKPETSC(ierr);

    ierr = (*func)(snes,w,G,fctx); CHKPETSC(ierr);
    ierr = VecNorm(G,NORM_2,&gnorm); CHKPETSC(ierr);

    bool res_is_large;
    ierr = CheckForLargeResidual(snes,gnorm,&res_is_large,ctx); CHKPETSC(ierr);
    if (res_is_large) {
      std::string reason = "Solution rejected.  Norm of residual has grown too large";
      if (ParallelDescriptor::IOProcessor() && nlsc->monitor_line_search) {
        std::cout << tag << tag_ls << reason << std::endl;
      }
      nlsc->ls_success = false;
      nlsc->ls_reason = reason;
      PetscFunctionReturn(0);
    }

    Vec y_orig;
    if (!(rs->GetRecordFile().empty())) {
      ierr = VecDuplicate(y,&y_orig); CHKPETSC(ierr);
      ierr = VecCopy(y,y_orig); CHKPETSC(ierr);
    }

    bool norm_acceptable = gnorm < fnorm * nlsc->ls_acceptance_factor;
    int ls_iterations = 0;
    Real ls_factor = 1;
    bool finished = norm_acceptable 
        || ls_iterations > nlsc->max_ls_iterations
        || ls_factor <= nlsc->min_ls_factor;

    Real gnorm_0 = gnorm;
    while (!finished) 
    {
        ls_factor *= nlsc->ls_reduction_factor;
        if (ls_factor < nlsc->min_ls_factor) {
            ls_factor = nlsc->min_ls_factor;
        }

        PetscReal mone = -1;
        ierr=VecWAXPY(w,mone*ls_factor,y,x); CHKPETSC(ierr); /* w = -y + x */
        *changed_w = PETSC_TRUE;
        
        ierr = (*func)(snes,w,G,fctx); CHKPETSC(ierr);
        ierr=VecNorm(G,NORM_2,&gnorm);CHKPETSC(ierr); CHKPETSC(ierr);
        norm_acceptable = gnorm < fnorm * nlsc->ls_acceptance_factor;
        
        if (ls_factor < 1 
            && nlsc->monitor_line_search 
            && ParallelDescriptor::IOProcessor())
	{
            std::cout << tag << tag_ls
                      << "iter=" << ls_iterations
                      << ", step length=" << ls_factor
                      << ", Newton norm=" << gnorm_0
                      << ", damped norm=" << gnorm << '\n';
	}
        
        finished = norm_acceptable 
            || ls_iterations > nlsc->max_ls_iterations
            || ls_factor <= nlsc->min_ls_factor;      
        ls_iterations++;
    }
    
    if (ls_iterations > nlsc->max_ls_iterations) 
    {
        std::string reason = "Solution rejected.  Linear system solved, but ls_iterations too large";
        if (ParallelDescriptor::IOProcessor() && nlsc->monitor_line_search) {
            std::cout << tag << tag_ls << reason << std::endl;
        }
        nlsc->ls_success = false;
        nlsc->ls_reason = reason;
    }
    else if (ls_factor <= nlsc->min_ls_factor) {
        std::string reason = "Solution rejected.  Linear system solved, but ls_factor too small";
        if (ParallelDescriptor::IOProcessor() && nlsc->monitor_line_search) {
            std::cout << tag << tag_ls << reason << std::endl;
        }
        nlsc->ls_success = false;
        nlsc->ls_reason = reason;
    }
    else {
        if (ls_factor == 1) {
          nlsc->ls_reason = std::string("Full linear step accepted");
          if (ParallelDescriptor::IOProcessor() && nlsc->monitor_line_search>1) {
            std::cout << tag << tag_ls << nlsc->ls_reason << std::endl;
          }
        }
        else {
            // Set update to the one actually used
            ierr=VecScale(y,ls_factor); CHKPETSC(ierr);
            *changed_y = PETSC_TRUE;
            nlsc->ls_reason = "Damped step successful";
        }
        nlsc->ls_success = true;

        int iters = nlsc->NLIterationsTaken() + 1;
    }

    if (!(rs->GetRecordFile().empty())) {
      RecordSolve(x,y,y_orig,w,F,G,check_ctx);
      ierr = VecDestroy(&y_orig); CHKPETSC(ierr);
    }
    
    PetscFunctionReturn(0);
}

#undef __FUNCT__  
#define __FUNCT__ "AltUpdate"
PetscErrorCode
AltUpdate(SNES snes,Vec pk,Vec dp,Vec pkp1,void *ctx,Real ls_factor,PetscBool *changed_dp,PetscBool *changed_pkp1)
{
    PetscErrorCode ierr;
    CheckCtx* check_ctx = (CheckCtx*)ctx;
    RichardSolver* rs = check_ctx->rs;
    if (rs==0) {
        BoxLib::Abort("Context cast failed in AltUpdate");
    }
    NLScontrol& nlsc = rs->GetNLScontrol();

    nlsc.ls_success = true;
    nlsc.ls_reason = "In Progress";

    if (rs->GetNLScontrol().scale_soln_before_solve) {
        Vec& Ptyp = rs->GetSolnTypV();
        ierr = VecPointwiseMult(dp,dp,Ptyp); CHKPETSC(ierr);
        ierr = VecPointwiseMult(pk,pk,Ptyp); CHKPETSC(ierr);
    }

    MFTower& P_MFT = rs->GetPressureNp1();
    MFTower& RS_MFT = rs->GetRhoSatNp1();
    MFTower& DP_MFT = rs->GetAlpha(); //Handy data container
    const MFTower& K_MFT = rs->GetKappaCCavg();

    Layout& layout = rs->GetLayout();
    ierr = layout.VecToMFTower(P_MFT,pk,0); CHKPETSC(ierr);
    ierr = layout.VecToMFTower(DP_MFT,dp,0); CHKPETSC(ierr);

    // Fill (rho.sat)^{n+1,k} from p^{n+1,k}
    rs->GetRSdata().calcInvPressure(RS_MFT,P_MFT);  

    int nLevs = layout.NumLevels();
    for (int lev=0; lev<nLevs; ++lev) {

        // Get full set of Pcap parameters directly
        // (not just the sigma part stored in my copy of PCapParams)
        const MultiFab& PCapParams = rs->GetPCapParams()[lev];

        // Compute the "Alternating Update" according to Krabbenhoft, AWR30 p.483
        for (MFIter mfi(P_MFT[lev]); mfi.isValid(); ++mfi) {
            const Box& vbox = mfi.validbox();
            const FArrayBox& kcf = K_MFT[lev][mfi];
            const FArrayBox& cpf = PCapParams[mfi];
            int n_cp_coefs = cpf.nComp();
            
            FArrayBox& rsf = RS_MFT[lev][mfi];
            FArrayBox& pf  = P_MFT[lev][mfi];
            FArrayBox& dpf = DP_MFT[lev][mfi];

            FORT_RS_ALTUP(rsf.dataPtr(),ARLIM(rsf.loVect()), ARLIM(rsf.hiVect()),
                          pf.dataPtr(), ARLIM(pf.loVect()),  ARLIM(pf.hiVect()),
                          dpf.dataPtr(),ARLIM(dpf.loVect()), ARLIM(dpf.hiVect()),
                          kcf.dataPtr(),ARLIM(kcf.loVect()), ARLIM(kcf.hiVect()),
                          cpf.dataPtr(),ARLIM(cpf.loVect()), ARLIM(cpf.hiVect()),
                          &n_cp_coefs, &ls_factor, vbox.loVect(), vbox.hiVect(),
                          &(rs->GetRSdata()).variable_switch_saturation_threshold);
        }
    }
    
    // Put modified dp into Vec
    ierr = layout.MFTowerToVec(dp,DP_MFT,0); CHKPETSC(ierr);

    // Compute new p = p - dp, then scale all p, pnew and dt
    ierr = VecWAXPY(pkp1,-1.0,dp,pk);CHKPETSC(ierr);

    if (rs->GetNLScontrol().scale_soln_before_solve) {
        Vec& PtypInv = rs->GetSolnTypInvV();
        ierr = VecPointwiseMult(dp,dp,PtypInv); CHKPETSC(ierr);
        ierr = VecPointwiseMult(pk,pk,PtypInv); CHKPETSC(ierr);
        ierr = VecPointwiseMult(pkp1,pkp1,PtypInv); CHKPETSC(ierr);
    }
    *changed_dp = PETSC_FALSE; // We changed dp and pnew, but we took care of the update already
    *changed_pkp1 = PETSC_TRUE;

    nlsc.ls_success = true;
    nlsc.ls_reason = "Damped step successful";

    PetscFunctionReturn(0);
}

#if defined(PETSC_3_2)
#include <private/snesimpl.h>
#else
#include <petsc-private/snesimpl.h> 
#endif

#undef __FUNCT__  
#define __FUNCT__ "PostCheckAlt"
PetscErrorCode 
PostCheckAlt(SNES snes,Vec p,Vec dp,Vec pnew,void *ctx,PetscBool  *changed_dp,PetscBool  *changed_pnew)
{
    std::string tag = "       Newton step: ";
    std::string tag_ls = "  line-search:  ";
    CheckCtx* check_ctx = (CheckCtx*)ctx;
    RichardSolver* rs = check_ctx->rs;
    NLScontrol* nlsc = check_ctx->nlsc;
    if (rs==0) {
        BoxLib::Abort("Context cast failed in PostCheckAlt");
    }

    nlsc->ls_success = true;
    nlsc->ls_reason = "In Progress";

    PetscErrorCode ierr;
    PetscReal fnorm, xnorm, ynorm, gnorm;
    PetscErrorCode (*func)(SNES,Vec,Vec,void*);
    void *fctx;

    Real ls_factor = 1;
    ierr = AltUpdate(snes,p,dp,pnew,ctx,ls_factor,changed_dp,changed_pnew);CHKPETSC(ierr);
    ierr = SNESGetFunction(snes,PETSC_NULL,&func,&fctx);CHKPETSC(ierr);

    Vec& F = rs->GetResidualV();
    Vec& G = rs->GetTrialResV();
    
    ierr = (*func)(snes,p,F,fctx);CHKPETSC(ierr);
    ierr = VecNorm(F,NORM_2,&fnorm);CHKPETSC(ierr);

    ierr = (*func)(snes,pnew,G,fctx);CHKPETSC(ierr);
    ierr = VecNorm(G,NORM_2,&gnorm);CHKPETSC(ierr);

    bool res_is_large;
    ierr = CheckForLargeResidual(snes,gnorm,&res_is_large,ctx); CHKPETSC(ierr);
    if (res_is_large) {
      std::string reason = "Solution rejected.  Norm of residual has grown too large";
      if (ParallelDescriptor::IOProcessor() && nlsc->monitor_line_search) {
        std::cout << tag << tag_ls << reason << std::endl;
      }
      nlsc->ls_success = false;
      nlsc->ls_reason = reason;
      PetscFunctionReturn(0);
    }

    Vec dp_orig;
    if (!(rs->GetRecordFile().empty())) {
      ierr = VecDuplicate(dp,&dp_orig);
      ierr = VecCopy(dp,dp_orig);
    }
    
    bool norm_acceptable = gnorm < fnorm * nlsc->ls_acceptance_factor;
    int ls_iterations = 0;
    bool finished = norm_acceptable 
        || ls_iterations > nlsc->max_ls_iterations
        || ls_factor <= nlsc->min_ls_factor;

    Real gnorm_0 = gnorm;
    while (!finished) 
    {
        ls_factor *= nlsc->ls_reduction_factor;
        if (ls_factor < nlsc->min_ls_factor) {
            ls_factor = nlsc->min_ls_factor;
        }

        ierr = AltUpdate(snes,p,dp,pnew,ctx,ls_factor,changed_dp,changed_pnew);CHKPETSC(ierr);
        ierr = (*func)(snes,pnew,G,fctx);CHKPETSC(ierr);
        ierr = VecNorm(G,NORM_2,&gnorm);CHKPETSC(ierr);
        norm_acceptable = gnorm < fnorm * nlsc->ls_acceptance_factor;
        
        if (ls_factor < 1 
            && nlsc->monitor_line_search 
            && ParallelDescriptor::IOProcessor())
	{
            std::cout << tag << tag_ls
                      << "iter=" << ls_iterations
                      << ", step length=" << ls_factor
                      << ", Newton norm=" << gnorm_0
                      << ", damped norm=" << gnorm << '\n';
	}
        
        finished = norm_acceptable 
            || ls_iterations > nlsc->max_ls_iterations
            || ls_factor <= nlsc->min_ls_factor;      
        ls_iterations++;
    }
    
    if (ls_iterations > nlsc->max_ls_iterations) 
    {
        std::string reason = "Solution rejected.  Linear system solved, but ls_iterations too large";
        if (ParallelDescriptor::IOProcessor()) {
            std::cout << tag << tag_ls << reason << std::endl;
        }
        snes->reason = SNES_DIVERGED_LINE_SEARCH;
        nlsc->ls_success = false;
        nlsc->ls_reason = reason;
    }
    else if (ls_factor <= nlsc->min_ls_factor) {
        std::string reason = "Solution rejected.  Linear system solved, but ls_factor too small";
        if (ParallelDescriptor::IOProcessor()) {
            std::cout << tag << tag_ls << reason << std::endl;
        }
        snes->reason = SNES_DIVERGED_LINE_SEARCH;
        nlsc->ls_success = false;
        nlsc->ls_reason = reason;
    }
    else {
        if (ls_factor == 1) {
            std::string reason = "Full linear step accepted";
            if (ParallelDescriptor::IOProcessor() && nlsc->monitor_line_search>1) {
                std::cout << tag << tag_ls << reason << std::endl;
            }
            nlsc->ls_reason = reason;
        }
        nlsc->ls_success = true;

        int iters = nlsc->NLIterationsTaken() + 1;
    }

    if (!(rs->GetRecordFile().empty())) {
      RecordSolve(p,dp,dp_orig,pnew,F,G,check_ctx);
      ierr = VecDestroy(&dp_orig);CHKPETSC(ierr);
    }

    PetscFunctionReturn(0);
}

#endif


#if defined(PETSC_3_2)
#include <private/matimpl.h>
#else
#include <petsc-private/matimpl.h> 
#endif

#undef __FUNCT__  
#define __FUNCT__ "RichardMatFDColoringApply"
PetscErrorCode  
RichardMatFDColoringApply(Mat J,MatFDColoring coloring,Vec x1,MatStructure *flag,void *sctx)
{
  PetscErrorCode (*f)(void*,Vec,Vec,void*) = (PetscErrorCode (*)(void*,Vec,Vec,void *))coloring->f;
  PetscErrorCode ierr;
  PetscInt       k,start,end,l,row,col,srow,**vscaleforrow,m1,m2;
  PetscScalar    dx,*y,*w3_array;
  PetscScalar    *vscale_array, *solnTyp_array;
  PetscReal      epsilon = coloring->error_rel,umin = coloring->umin,unorm; 
  Vec            w1=coloring->w1,w2=coloring->w2,w3;
  void           *fctx = coloring->fctx;
  PetscBool      flg = PETSC_FALSE;
  PetscInt       ctype=coloring->ctype,N,col_start=0,col_end=0;
  Vec            x1_tmp;

  PetscFunctionBegin;    
  PetscValidHeaderSpecific(J,MAT_CLASSID,1);
  PetscValidHeaderSpecific(coloring,MAT_FDCOLORING_CLASSID,2);
  PetscValidHeaderSpecific(x1,VEC_CLASSID,3);
  if (!f) SETERRQ(((PetscObject)J)->comm,PETSC_ERR_ARG_WRONGSTATE,"Must call MatFDColoringSetFunction()");

  ierr = PetscLogEventBegin(MAT_FDColoringApply,coloring,J,x1,0);CHKPETSC(ierr);
  ierr = MatSetUnfactored(J);CHKPETSC(ierr);
  ierr = PetscOptionsGetBool(PETSC_NULL,"-mat_fd_coloring_dont_rezero",&flg,PETSC_NULL);CHKPETSC(ierr);
  if (flg) {
    ierr = PetscInfo(coloring,"Not calling MatZeroEntries()\n");CHKPETSC(ierr);
  } else {
    PetscBool  assembled;
    ierr = MatAssembled(J,&assembled);CHKPETSC(ierr);
    if (assembled) {
      ierr = MatZeroEntries(J);CHKPETSC(ierr);
    }
  }

  x1_tmp = x1; 
  if (!coloring->vscale){ 
    ierr = VecDuplicate(x1_tmp,&coloring->vscale);CHKPETSC(ierr);
  }
    
  /*
    This is a horrible, horrible, hack. See DMMGComputeJacobian_Multigrid() it inproperly sets
    coloring->F for the coarser grids from the finest
  */
  if (coloring->F) {
    ierr = VecGetLocalSize(coloring->F,&m1);CHKPETSC(ierr);
    ierr = VecGetLocalSize(w1,&m2);CHKPETSC(ierr);
    if (m1 != m2) {  
      coloring->F = 0; 
      }    
    }   


  RichardSolver* rs = static_rs_ptr;
  BL_ASSERT(rs);
  ierr = VecGetOwnershipRange(w1,&start,&end);CHKPETSC(ierr); /* OwnershipRange is used by ghosted x! */
      
  /* Set w1 = F(x1) */
  if (coloring->F) {
    w1          = coloring->F; /* use already computed value of function */
    coloring->F = 0; 
  } else {
    ierr = PetscLogEventBegin(MAT_FDColoringFunction,0,0,0,0);CHKPETSC(ierr);
    ierr = (*f)(sctx,x1_tmp,w1,fctx);CHKPETSC(ierr);
    ierr = PetscLogEventEnd(MAT_FDColoringFunction,0,0,0,0);CHKPETSC(ierr);
  }
      
  if (!coloring->w3) {
    ierr = VecDuplicate(x1_tmp,&coloring->w3);CHKPETSC(ierr);
    ierr = PetscLogObjectParent(coloring,coloring->w3);CHKPETSC(ierr);
  }
  w3 = coloring->w3;

    /* Compute all the local scale factors, including ghost points */
  ierr = VecGetLocalSize(x1_tmp,&N);CHKPETSC(ierr);

  if (rs->GetNLScontrol().scale_soln_before_solve) {
      ierr = VecSet(coloring->vscale,1);
      ierr = VecScale(coloring->vscale,1/epsilon);
  }
  else {
      Vec& SolnTypV = rs->GetSolnTypV();
      ierr = VecGetArray(SolnTypV,&solnTyp_array);CHKPETSC(ierr);
      ierr = VecGetArray(coloring->vscale,&vscale_array);CHKPETSC(ierr);
      if (ctype == IS_COLORING_GHOSTED){
          col_start = 0; col_end = N;
      } else if (ctype == IS_COLORING_GLOBAL){
          solnTyp_array = solnTyp_array - start;
          vscale_array = vscale_array - start;
          col_start = start; col_end = N + start;
      }
      for (col=col_start; col<col_end; col++) { 
          vscale_array[col] = (PetscScalar)(1.0 / (solnTyp_array[col] * epsilon));
      } 
      if (ctype == IS_COLORING_GLOBAL)  {
          vscale_array = vscale_array + start;      
          solnTyp_array = solnTyp_array + start;      
      }
      ierr = VecRestoreArray(coloring->vscale,&vscale_array);CHKPETSC(ierr);
      ierr = VecRestoreArray(SolnTypV,&solnTyp_array);CHKPETSC(ierr);
  }

  if (ctype == IS_COLORING_GLOBAL){
      ierr = VecGhostUpdateBegin(coloring->vscale,INSERT_VALUES,SCATTER_FORWARD);CHKPETSC(ierr);
      ierr = VecGhostUpdateEnd(coloring->vscale,INSERT_VALUES,SCATTER_FORWARD);CHKPETSC(ierr);
  }
  
  if (coloring->vscaleforrow) {
    vscaleforrow = coloring->vscaleforrow;
  } else SETERRQ(((PetscObject)J)->comm,PETSC_ERR_ARG_NULL,"Null Object: coloring->vscaleforrow");

  /*
    Loop over each color
  */
  int p = ParallelDescriptor::MyProc();
  if (rs->GetNLScontrol().scale_soln_before_solve) {
      //
      // In this case, since the soln is scaled, the perturbation is a simple constant, epsilon
      // Compared to the case where dx=dx_i, the logic cleans up quite a bit here.
      //
      for (k=0; k<coloring->ncolors; k++) { 
          coloring->currentcolor = k;

          ierr = VecCopy(x1_tmp,w3);CHKPETSC(ierr);
          ierr = VecGetArray(w3,&w3_array);CHKPETSC(ierr);
          if (ctype == IS_COLORING_GLOBAL) w3_array = w3_array - start;          
          for (l=0; l<coloring->ncolumns[k]; l++) {
              col = coloring->columns[k][l];    /* local column of the matrix we are probing for */
              w3_array[col] += epsilon;
          } 
          if (ctype == IS_COLORING_GLOBAL) w3_array = w3_array + start;
          ierr = VecRestoreArray(w3,&w3_array);CHKPETSC(ierr);
          
          // w2 = F(w3) - F(x1) = F(x1 + dx) - F(x1)
          ierr = PetscLogEventBegin(MAT_FDColoringFunction,0,0,0,0);CHKPETSC(ierr);
          ierr = (*f)(sctx,w3,w2,fctx);CHKPETSC(ierr);        
          ierr = PetscLogEventEnd(MAT_FDColoringFunction,0,0,0,0);CHKPETSC(ierr);
          ierr = VecAXPY(w2,-1.0,w1);CHKPETSC(ierr); 
          
          // Insert (w2_j / dx) into J_ij
          PetscReal epsilon_inv = 1/epsilon;
          ierr = VecGetArray(w2,&y);CHKPETSC(ierr);          
          for (l=0; l<coloring->nrows[k]; l++) {
              row    = coloring->rows[k][l];             /* local row index */
              col    = coloring->columnsforrow[k][l];    /* global column index */
              y[row] *= epsilon_inv;                     /* dx = epsilon */
              srow   = row + start;                      /* global row index */
              ierr   = MatSetValues(J,1,&srow,1,&col,y+row,INSERT_VALUES);CHKPETSC(ierr);
          }
          ierr = VecRestoreArray(w2,&y);CHKPETSC(ierr);
          
      } /* endof for each color */

  }
  else {
      ierr = VecGetArray(coloring->vscale,&vscale_array);CHKPETSC(ierr);
      if (ctype == IS_COLORING_GLOBAL) {
          vscale_array = vscale_array - start;
      }
      
      for (k=0; k<coloring->ncolors; k++) { 
          coloring->currentcolor = k;
          ierr = VecCopy(x1_tmp,w3);CHKPETSC(ierr);
          ierr = VecGetArray(w3,&w3_array);CHKPETSC(ierr);
          if (ctype == IS_COLORING_GLOBAL) {
              w3_array = w3_array - start;
          }
          
          /*
            Loop over each column associated with color 
            adding the perturbation to the vector w3.
          */
          for (l=0; l<coloring->ncolumns[k]; l++) {
              col = coloring->columns[k][l];    /* local column of the matrix we are probing for */
              w3_array[col] += 1/vscale_array[col];
          } 
          if (ctype == IS_COLORING_GLOBAL) {
              w3_array = w3_array + start;
          }
          ierr = VecRestoreArray(w3,&w3_array);CHKPETSC(ierr);
          
          /*
            Evaluate function at w3 = x1 + dx (here dx is a vector of perturbations)
            w2 = F(x1 + dx) - F(x1)
          */
          ierr = PetscLogEventBegin(MAT_FDColoringFunction,0,0,0,0);CHKPETSC(ierr);
          ierr = (*f)(sctx,w3,w2,fctx);CHKPETSC(ierr);        
          ierr = PetscLogEventEnd(MAT_FDColoringFunction,0,0,0,0);CHKPETSC(ierr);
          ierr = VecAXPY(w2,-1.0,w1);CHKPETSC(ierr); 
          
          /*
            Loop over rows of vector, putting results into Jacobian matrix
          */
          
          
          ierr = VecGetArray(w2,&y);CHKPETSC(ierr);
          for (l=0; l<coloring->nrows[k]; l++) {
              row    = coloring->rows[k][l];             /* local row index */
              col    = coloring->columnsforrow[k][l];    /* global column index */
              y[row] *= vscale_array[vscaleforrow[k][l]];
              srow   = row + start;
              ierr   = MatSetValues(J,1,&srow,1,&col,y+row,INSERT_VALUES);CHKPETSC(ierr);
          }
          ierr = VecRestoreArray(w2,&y);CHKPETSC(ierr);
                    
      } /* endof for each color */
      if (ctype == IS_COLORING_GLOBAL) {
          vscale_array = vscale_array + start;
      }
      
      ierr = VecRestoreArray(coloring->vscale,&vscale_array);CHKPETSC(ierr);
  }
   
  coloring->currentcolor = -1;
  ierr  = MatAssemblyBegin(J,MAT_FINAL_ASSEMBLY);CHKPETSC(ierr);
  ierr  = MatAssemblyEnd(J,MAT_FINAL_ASSEMBLY);CHKPETSC(ierr);
  ierr = PetscLogEventEnd(MAT_FDColoringApply,coloring,J,x1,0);CHKPETSC(ierr);

  if (dump_Jacobian_and_exit) {
    std::string viewer_filename="mat.output";
    PetscViewer viewer;
    ierr = PetscViewerASCIIOpen(PETSC_COMM_WORLD,viewer_filename.c_str(),&viewer); CHKPETSC(ierr);
    ierr = MatView(J,viewer); CHKPETSC(ierr);
    ierr = PetscViewerDestroy(&viewer); CHKPETSC(ierr);
    ierr = VecGetSize(w2,&N); CHKPETSC(ierr);
    if (ParallelDescriptor::IOProcessor()) {
      std::cout << "There are " << N << " rows in the Jacobian" << std::endl;
    }
    std::string str = "Jacobian written in ASCII to " + viewer_filename + " and run killed from RichardSolver.cpp";
    BoxLib::Abort(str.c_str());
  }

  flg  = PETSC_FALSE;
  ierr = PetscOptionsGetBool(PETSC_NULL,"-mat_null_space_test",&flg,PETSC_NULL);CHKPETSC(ierr);
  if (flg) {
    ierr = MatNullSpaceTest(J->nullsp,J,PETSC_NULL);CHKPETSC(ierr);
  }

  PetscFunctionReturn(0);
}

#undef __FUNCT__  
#define __FUNCT__ "ComputeRichardAlpha"
void
RichardSolver::ComputeRichardAlpha(Vec& Alpha,const Vec& Pressure,Real t)
{
  MFTower& PCapParamsMFT = GetPCapParams();
  MFTower& PMFT = GetPressureNp1();
  MFTower& aMFT = GetAlpha();
  PetscErrorCode ierr = GetLayout().VecToMFTower(PMFT,Pressure,0); CHKPETSC(ierr);

  rs_data.calcInvPressure(GetRhoSatNp1(),PMFT);
  rs_data.calcRichardAlpha(aMFT,GetRhoSatNp1(),t);

  // Put into Vec data structure
  ierr = GetLayout().MFTowerToVec(Alpha,aMFT,0); CHKPETSC(ierr);
}


#undef __FUNCT__  
#define __FUNCT__ "SemiAnalyticMatFDColoringApply"
PetscErrorCode  
SemiAnalyticMatFDColoringApply(Mat J,MatFDColoring coloring,Vec x1,MatStructure *flag,void *sctx)
{
  PetscErrorCode (*f)(void*,Vec,Vec,void*) = (PetscErrorCode (*)(void*,Vec,Vec,void *))coloring->f;
  PetscErrorCode ierr;
  PetscInt       k,start,end,l,row,col,srow,**vscaleforrow,m1,m2;
  PetscScalar    dx,*y,*w3_array;
  PetscScalar    *vscale_array, *solnTyp_array, *a_array;
  PetscReal      epsilon = coloring->error_rel,umin = coloring->umin,unorm; 
  Vec            w1=coloring->w1,w2=coloring->w2,w3;
  void           *fctx = coloring->fctx;
  PetscBool      flg = PETSC_FALSE;
  PetscInt       ctype=coloring->ctype,N,col_start=0,col_end=0;
  Vec            x1_tmp;

  PetscFunctionBegin;    
  PetscValidHeaderSpecific(J,MAT_CLASSID,1);
  PetscValidHeaderSpecific(coloring,MAT_FDCOLORING_CLASSID,2);
  PetscValidHeaderSpecific(x1,VEC_CLASSID,3);
  if (!f) SETERRQ(((PetscObject)J)->comm,PETSC_ERR_ARG_WRONGSTATE,"Must call MatFDColoringSetFunction()");

  ierr = PetscLogEventBegin(MAT_FDColoringApply,coloring,J,x1,0);CHKPETSC(ierr);
  ierr = MatSetUnfactored(J);CHKPETSC(ierr);
  ierr = PetscOptionsGetBool(PETSC_NULL,"-mat_fd_coloring_dont_rezero",&flg,PETSC_NULL);CHKPETSC(ierr);
  if (flg) {
    ierr = PetscInfo(coloring,"Not calling MatZeroEntries()\n");CHKPETSC(ierr);
  } else {
    PetscBool  assembled;
    ierr = MatAssembled(J,&assembled);CHKPETSC(ierr);
    if (assembled) {
      ierr = MatZeroEntries(J);CHKPETSC(ierr);
    }
  }

  x1_tmp = x1; 
  if (!coloring->vscale){ 
    ierr = VecDuplicate(x1_tmp,&coloring->vscale);CHKPETSC(ierr);
  }
    
  /*
    This is a horrible, horrible, hack. See DMMGComputeJacobian_Multigrid() it inproperly sets
    coloring->F for the coarser grids from the finest
  */
  if (coloring->F) {
    ierr = VecGetLocalSize(coloring->F,&m1);CHKPETSC(ierr);
    ierr = VecGetLocalSize(w1,&m2);CHKPETSC(ierr);
    if (m1 != m2) {  
      coloring->F = 0; 
      }    
    }   
  RichardSolver* rs = static_rs_ptr;
  BL_ASSERT(rs);
  Vec& AlphaV = rs->GetAlphaV();
  
  Vec& press = rs->GetTrialResV(); // A handy Vec to use
  ierr = VecCopy(x1_tmp,press); CHKPETSC(ierr);
  if (rs->GetNLScontrol().scale_soln_before_solve) {
      ierr = VecPointwiseMult(press,x1_tmp,rs->GetSolnTypV()); CHKPETSC(ierr); // Mult(w,x,y): w=x.y, p=pbar.ptyp
  }
  rs->ComputeRichardAlpha(AlphaV,press,rs->GetTime());

  if (rs->GetNLScontrol().scale_soln_before_solve) {
      ierr = VecPointwiseMult(AlphaV,AlphaV,rs->GetSolnTypV()); CHKPETSC(ierr); // Mult(w,x,y): w=x.y, alphabar=alpha.ptyp
  }

  Real dt_inv = 1/rs->GetDt();

  ierr = VecGetOwnershipRange(w1,&start,&end);CHKPETSC(ierr); /* OwnershipRange is used by ghosted x! */

  if (!rs->GetNLScontrol().centered_diff_J) {
      /* Set w1 = F(x1) */
      if (coloring->F) {
          w1          = coloring->F; /* use already computed value of function */
          coloring->F = 0; 
      } else {
          ierr = PetscLogEventBegin(MAT_FDColoringFunction,0,0,0,0);CHKPETSC(ierr);
          ierr = (*f)(sctx,x1_tmp,w1,fctx);CHKPETSC(ierr);
          ierr = PetscLogEventEnd(MAT_FDColoringFunction,0,0,0,0);CHKPETSC(ierr);
      }
  }
      
  if (!coloring->w3) {
    ierr = VecDuplicate(x1_tmp,&coloring->w3);CHKPETSC(ierr);
    ierr = PetscLogObjectParent(coloring,coloring->w3);CHKPETSC(ierr);
  }
  w3 = coloring->w3;

    /* Compute all the local scale factors, including ghost points */
  ierr = VecGetLocalSize(x1_tmp,&N);CHKPETSC(ierr);

  ierr = VecSet(coloring->vscale,1); CHKPETSC(ierr);
  ierr = VecScale(coloring->vscale,1/epsilon); CHKPETSC(ierr);

  if (ctype == IS_COLORING_GLOBAL){
      ierr = VecGhostUpdateBegin(coloring->vscale,INSERT_VALUES,SCATTER_FORWARD);CHKPETSC(ierr);
      ierr = VecGhostUpdateEnd(coloring->vscale,INSERT_VALUES,SCATTER_FORWARD);CHKPETSC(ierr);
  }
  
  if (coloring->vscaleforrow) {
    vscaleforrow = coloring->vscaleforrow;
  } else SETERRQ(((PetscObject)J)->comm,PETSC_ERR_ARG_NULL,"Null Object: coloring->vscaleforrow");

  /*
    Loop over each color
  */
  int p = ParallelDescriptor::MyProc();
  //
  // In this case, since the soln is scaled, the perturbation is a simple constant, epsilon
  // Compared to the case where dx=dx_i, the logic cleans up quite a bit here.
  //
  Vec& w4 = rs->GetTrialResV(); // A handy Vec to use if centered diff for J

  for (k=0; k<coloring->ncolors; k++) { 
      coloring->currentcolor = k;
      
      ierr = VecCopy(x1_tmp,w3);CHKPETSC(ierr);
      ierr = VecGetArray(w3,&w3_array);CHKPETSC(ierr);
      if (ctype == IS_COLORING_GLOBAL) w3_array = w3_array - start;          

      ierr = VecCopy(x1_tmp,w4);CHKPETSC(ierr);
      PetscReal *w4_array;
      ierr = VecGetArray(w4,&w4_array);CHKPETSC(ierr);
      if (ctype == IS_COLORING_GLOBAL) w4_array = w4_array - start;          

      for (l=0; l<coloring->ncolumns[k]; l++) {
          col = coloring->columns[k][l];    /* local column of the matrix we are probing for */
          w3_array[col] += epsilon;
          w4_array[col] -= epsilon;
      } 
      if (ctype == IS_COLORING_GLOBAL) w3_array = w3_array + start;
      ierr = VecRestoreArray(w3,&w3_array);CHKPETSC(ierr);
      
      if (ctype == IS_COLORING_GLOBAL) w4_array = w4_array + start;
      ierr = VecRestoreArray(w4,&w4_array);CHKPETSC(ierr);

      PetscReal epsilon_inv;
      if (rs->GetNLScontrol().centered_diff_J) {
          // w2 <- w2 - w1 = F(w3) - F(w4) = F(x1 + dx) - F(x1 - dx)
          ierr = PetscLogEventBegin(MAT_FDColoringFunction,0,0,0,0);CHKPETSC(ierr);
          ierr = (*f)(sctx,w3,w2,fctx);CHKPETSC(ierr);        
          ierr = (*f)(sctx,w4,w1,fctx);CHKPETSC(ierr);        
          ierr = PetscLogEventEnd(MAT_FDColoringFunction,0,0,0,0);CHKPETSC(ierr);
          ierr = VecAXPY(w2,-1.0,w1);CHKPETSC(ierr); 
          epsilon_inv = 0.5/epsilon;
      }
      else {
          // w2 = F(w3) - F(x1) = F(x1 + dx) - F(x1)
          ierr = PetscLogEventBegin(MAT_FDColoringFunction,0,0,0,0);CHKPETSC(ierr);
          ierr = (*f)(sctx,w3,w2,fctx);CHKPETSC(ierr);        
          ierr = PetscLogEventEnd(MAT_FDColoringFunction,0,0,0,0);CHKPETSC(ierr);
          ierr = VecAXPY(w2,-1.0,w1);CHKPETSC(ierr); 
          epsilon_inv = 1/epsilon;
      }
      
      // Insert (w2_j / dx) into J_ij [include diagonal term, dR1_i/dpbar_i = alphabar
      ierr = VecGetArray(w2,&y);CHKPETSC(ierr);          
      ierr = VecGetArray(AlphaV,&a_array);CHKPETSC(ierr);          

      if (ctype == IS_COLORING_GLOBAL) a_array -= start;          

      for (l=0; l<coloring->nrows[k]; l++) {
          row    = coloring->rows[k][l];             /* local row index */
          col    = coloring->columnsforrow[k][l];    /* global column index */
          y[row] *= epsilon_inv;                     /* dx = epsilon */
          srow   = row + start;                      /* global row index */

          if (dt_inv>0 && srow == col) {
              y[row] += a_array[srow] * dt_inv;
          }
          ierr   = MatSetValues(J,1,&srow,1,&col,y+row,INSERT_VALUES);CHKPETSC(ierr);
      }
      if (ctype == IS_COLORING_GLOBAL) {
          a_array += start;          
      }
      ierr = VecRestoreArray(AlphaV,&a_array);CHKPETSC(ierr);
      ierr = VecRestoreArray(w2,&y);CHKPETSC(ierr);
      
  } /* end of for each color */
   
  coloring->currentcolor = -1;
  ierr  = MatAssemblyBegin(J,MAT_FINAL_ASSEMBLY);CHKPETSC(ierr);
  ierr  = MatAssemblyEnd(J,MAT_FINAL_ASSEMBLY);CHKPETSC(ierr);
  ierr = PetscLogEventEnd(MAT_FDColoringApply,coloring,J,x1,0);CHKPETSC(ierr);

  if (dump_Jacobian_and_exit) {
    std::string viewer_filename="mat.output";
    PetscViewer viewer;
    ierr = PetscViewerASCIIOpen(PETSC_COMM_WORLD,viewer_filename.c_str(),&viewer); CHKPETSC(ierr);
    ierr = MatView(J,viewer); CHKPETSC(ierr);
    ierr = PetscViewerDestroy(&viewer); CHKPETSC(ierr);
    ierr = VecGetSize(w2,&N); CHKPETSC(ierr);
    if (ParallelDescriptor::IOProcessor()) {
      std::cout << "There are " << N << " rows in the Jacobian" << std::endl;
    }
    std::string str = "Jacobian written in ASCII to " + viewer_filename + " and run killed from RichardSolver.cpp";
    BoxLib::Abort(str.c_str());
  }

  flg  = PETSC_FALSE;
  ierr = PetscOptionsGetBool(PETSC_NULL,"-mat_null_space_test",&flg,PETSC_NULL);CHKPETSC(ierr);
  if (flg) {
    ierr = MatNullSpaceTest(J->nullsp,J,PETSC_NULL);CHKPETSC(ierr);
  }

  PetscFunctionReturn(0);
}

#undef __FUNCT__  
#define __FUNCT__ "RichardComputeJacobianColor"
PetscErrorCode 
RichardComputeJacobianColor(SNES snes,Vec x1,Mat *J,Mat *B,MatStructure *flag,void *ctx)
{
  MatFDColoring  color = (MatFDColoring) ctx;
  PetscErrorCode ierr;
  Vec            f;
  PetscErrorCode (*ff)(void),(*fd)(void);

  // ick!
  RichardSolver* rs = static_rs_ptr;
  BL_ASSERT(rs);

  PetscFunctionBegin;
  PetscValidHeaderSpecific(color,MAT_FDCOLORING_CLASSID,6);

  if (rs->ReusePreviousJacobian()) {
      PetscFunctionReturn(0);
  }

  *flag = SAME_NONZERO_PATTERN;
  ierr  = SNESGetFunction(snes,&f,(PetscErrorCode (**)(SNES,Vec,Vec,void*))&ff,0);CHKPETSC(ierr);
  ierr  = MatFDColoringGetFunction(color,&fd,PETSC_NULL);CHKPETSC(ierr);
  if (fd == ff) { /* reuse function value computed in SNES */
    ierr  = MatFDColoringSetF(color,f);CHKPETSC(ierr);
  }
  if (rs->GetRSdata().semi_analytic_J) {
      ierr = SemiAnalyticMatFDColoringApply(*B,color,x1,flag,snes);CHKPETSC(ierr);
  } 
  else {
      ierr = RichardMatFDColoringApply(*B,color,x1,flag,snes);CHKPETSC(ierr);
  }
  if (*J != *B) {
    ierr = MatAssemblyBegin(*J,MAT_FINAL_ASSEMBLY);CHKPETSC(ierr);
    ierr = MatAssemblyEnd(*J,MAT_FINAL_ASSEMBLY);CHKPETSC(ierr);
  }

  rs->ResetRemainingJacobianReuses();

  PetscFunctionReturn(0);
}

#undef __FUNCT__  
#define __FUNCT__ "MatSqueeze"
static void
MatSqueeze(Mat& J,
	   Layout& layout) 
{
  PetscErrorCode ierr;
  Mat A;
  MatCreate(PETSC_COMM_WORLD,&A);
  MatSetType(A,MATSEQAIJ);
  int n = layout.NumberOfLocalNodeIds();
  int N = layout.NumberOfGlobalNodeIds();

  ierr = MatSetSizes(A,n,n,N,N);  CHKPETSC(ierr);
  
  int rstart, rend;
  ierr = MatGetOwnershipRange(J,&rstart,&rend);CHKPETSC(ierr);
  int nrows = 0;
  int Jncols, Ancols;
  const PetscInt *Jcols;
  const PetscScalar *Jvals;
  PetscReal dtol = 1.e-20;
  for (int row=rstart; row<rend; row++){
    Array<PetscInt> Acols(0);
    Array<PetscReal> Avals(0);
    ierr = MatGetRow(J,row,&Jncols,&Jcols,&Jvals);CHKPETSC(ierr);
    for (int j=0; j<Jncols; j++){
      PetscScalar Jval = Jvals[j];
      if (std::abs(Jval) > dtol) {
	Acols.push_back(Jcols[j]);
	Avals.push_back(Jval);
      }
    }
    BL_ASSERT(Acols.size()>0 && Acols.size()==Avals.size());
    int one = 1;
    ierr = MatSetValues(A,one,&row,Avals.size(),Acols.dataPtr(),Avals.dataPtr(),INSERT_VALUES); CHKPETSC(ierr);
    ierr = MatRestoreRow(J,row,&Jncols,&Jcols,&Jvals);CHKPETSC(ierr);
  }
  ierr = MatAssemblyBegin(A,MAT_FINAL_ASSEMBLY); CHKPETSC(ierr);
  ierr = MatAssemblyEnd(A,MAT_FINAL_ASSEMBLY); CHKPETSC(ierr);

  MatView(A,PETSC_VIEWER_STDOUT_SELF);
}
