#include <UnitTest++.h>

#include <iostream>

#include "../Mesh_MSTK.hh"

#include "MeshAudit.hh"

#include "Epetra_Map.h"
#include "Epetra_MpiComm.h"


TEST(MSTK_HEX_3x3x3)
{

  int i, j, k, err, nc, nf, nv;
  Amanzi::AmanziMesh::Entity_ID faces[6], nodes[8];

  int NV = 64;
  int NF = 108;
  int NC = 27;

  Teuchos::RCP<Epetra_MpiComm> comm_(new Epetra_MpiComm(MPI_COMM_WORLD));

  // Load a mesh consisting of 3x3x3 elements 

  Teuchos::RCP<Amanzi::AmanziMesh::Mesh> mesh(new Amanzi::AmanziMesh::Mesh_MSTK("test/hex_3x3x3_sets.exo",comm_.get(),3));

  nf = mesh->num_entities(Amanzi::AmanziMesh::FACE,Amanzi::AmanziMesh::OWNED);
  CHECK_EQUAL(NF,nf);
  
  nc = mesh->num_entities(Amanzi::AmanziMesh::CELL,Amanzi::AmanziMesh::OWNED);
  CHECK_EQUAL(NC,nc);


  std::vector<Amanzi::AmanziMesh::Entity_ID>  c2f(6);
  Epetra_Map cell_map(mesh->cell_map(false));
  Epetra_Map face_map(mesh->face_map(false));
  for (int c=cell_map.MinLID(); c<=cell_map.MaxLID(); c++)
    {
      CHECK_EQUAL(cell_map.GID(c),mesh->GID(c,Amanzi::AmanziMesh::CELL));
      mesh->cell_get_faces(c, &c2f, true);
      for (int j=0; j<6; j++)
	{
	  int f = face_map.LID(c2f[j]);
	  CHECK( f == c2f[j] );
	}

    }

  std::stringstream fname;
  fname << "test/mstk_hex_3x3x3.out";
  std::ofstream fout(fname.str().c_str());
  Amanzi::MeshAudit auditor(mesh,fout);
  auditor.Verify();

  
}

